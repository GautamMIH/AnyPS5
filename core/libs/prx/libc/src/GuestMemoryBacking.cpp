#include "prx/libc/include/GuestMemoryBacking.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestMemoryTracking.hpp"
#include "prx/libc/include/MemoryBackingPlatform.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

namespace GuestMemoryBacking {
namespace {

std::uint64_t nextSegmentId = 1;

std::set<std::uint64_t>& liveSegments() {
    static auto* value = new std::set<std::uint64_t>;
    return *value;
}

// Created and destroyed under the tracking mutex.
struct SegmentHolder {
    Platform::Segment segment;
    std::uint64_t id;
    explicit SegmentHolder(std::size_t bytes) : segment(Platform::CreateSegment(bytes)), id(nextSegmentId++) { liveSegments().insert(id); }
    ~SegmentHolder() {
        liveSegments().erase(id);
        Platform::DestroySegment(segment);
    }
    SegmentHolder(const SegmentHolder&) = delete;
    SegmentHolder& operator=(const SegmentHolder&) = delete;
};

// One area of guest address space [begin, end). Committed areas map segment bytes starting at
// offset; a direct area's offset is its physical address.
struct Record {
    std::uint64_t end;
    Kind kind;
    int protection;
    bool noCoalesce;
    std::shared_ptr<SegmentHolder> segment;
    std::uint64_t offset;
};

std::map<std::uint64_t, Record>& areas() {
    static auto* value = new std::map<std::uint64_t, Record>;
    return *value;
}

std::atomic<std::uint64_t> mappingGeneration{0};

// Direct memory: one physical object, so every view of a physical page shares its contents.
const std::shared_ptr<SegmentHolder>& physicalSegment() {
    static const auto segment = std::make_shared<SegmentHolder>(static_cast<std::size_t>(kPhysicalBytes));
    return segment;
}

bool committed(Kind kind) {
    return kind != Kind::Reserved;
}

bool hostReadable(int protection) {
    return (protection & (kProtCpuRead | kProtCpuWrite | kProtGpuRead | kProtGpuWrite)) != 0;
}

bool hostWritable(int protection) {
    return (protection & (kProtCpuWrite | kProtGpuWrite)) != 0;
}

int hostProtection(int protection) {
    return (hostReadable(protection) ? 1 : 0) | (hostWritable(protection) ? 2 : 0) | ((protection & kProtCpuExec) != 0 ? 5 : 0);
}

// Splits the area containing address so an area begins there.
void split(std::uint64_t address) {
    auto& map = areas();
    auto it = map.upper_bound(address);
    if (it == map.begin()) return;
    --it;
    if (it->first == address || it->second.end <= address) return;
    Record right = it->second;
    right.offset += address - it->first;
    it->second.end = address;
    map.emplace(address, std::move(right));
}

// Starts of the areas overlapping [first, last).
std::vector<std::uint64_t> overlapping(std::uint64_t first, std::uint64_t last) {
    std::vector<std::uint64_t> result;
    auto& map = areas();
    auto it = map.upper_bound(first);
    if (it != map.begin() && std::prev(it)->second.end > first) --it;
    for (; it != map.end() && it->first < last; ++it) result.push_back(it->first);
    return result;
}

bool anyOverlap(std::uint64_t first, std::uint64_t last) {
    return !overlapping(first, last).empty();
}

// Removes the areas in [first, last): committed views return to the claimed, inaccessible state
// (the range stays claimed for the caller) and the GPU registry forgets them.
void carve(std::uint64_t first, std::uint64_t last) {
    split(first);
    split(last);
    for (const auto begin : overlapping(first, last)) {
        auto& map = areas();
        const auto it = map.find(begin);
        const auto end = it->second.end;
        if (committed(it->second.kind)) {
            Platform::UnmapView(begin, static_cast<std::size_t>(end - begin));
            if (it->second.kind != Kind::Heap) GuestAllocations::GuestAllocationsCarve_nid_postfix(nullptr, reinterpret_cast<const void*>(begin), static_cast<std::size_t>(end - begin), [] {});
        }
        map.erase(it);
    }
}

// Claims the unclaimed gaps of [first, last) for a fixed mapping, or returns false (after
// releasing what it claimed) when some of it belongs to someone else.
bool claimGaps(std::uint64_t first, std::uint64_t last) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> gaps;
    auto cursor = first;
    for (const auto begin : overlapping(first, last)) {
        const auto& record = areas().at(begin);
        if (begin > cursor) gaps.emplace_back(cursor, begin);
        cursor = std::max(cursor, record.end);
    }
    if (cursor < last) gaps.emplace_back(cursor, last);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> claimed;
    for (const auto& [begin, end] : gaps) {
        if (Platform::ClaimRange(begin, static_cast<std::size_t>(end - begin), kPageBytes, true) == 0) {
            for (const auto& [b, e] : claimed) Platform::ReleaseRange(b, static_cast<std::size_t>(e - b));
            return false;
        }
        claimed.emplace_back(begin, end);
    }
    return true;
}

void registerCommitted(std::uint64_t begin, std::uint64_t end, const Record& record) {
    if (!committed(record.kind) || record.kind == Kind::Heap) return;
    GuestAllocations::GuestAllocationsAdd_nid_postfix(nullptr, reinterpret_cast<void*>(begin), static_cast<std::size_t>(end - begin), hostReadable(record.protection), hostWritable(record.protection));
}

bool validRange(std::uint64_t address, std::size_t bytes) {
    return address != 0 && bytes != 0 && address % kPageBytes == 0 && bytes % kPageBytes == 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address;
}

Status unmapRange(std::uint64_t address, std::size_t bytes, bool heap) {
    if (!validRange(address, bytes)) return Status::Invalid;
    const auto last = address + bytes;
    for (const auto begin : overlapping(address, last)) {
        if ((areas().at(begin).kind == Kind::Heap) != heap) return Status::Invalid;
    }
    GuestAllocations::GuestAllocationsRequireUnpinned_nid_postfix(nullptr, reinterpret_cast<const void*>(address), bytes);
    GuestMemoryTracking::GuestMemoryTrackingInvalidate_nid_postfix(address, bytes);
    mappingGeneration.fetch_add(1, std::memory_order_acq_rel);
    split(address);
    split(last);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pieces;
    for (const auto begin : overlapping(address, last)) pieces.emplace_back(begin, areas().at(begin).end);
    carve(address, last);
    for (const auto& [begin, end] : pieces) Platform::ReleaseRange(begin, static_cast<std::size_t>(end - begin));
    return Status::Ok;
}

// A direct view: physical bytes [first, last) shown at guest address `address`.
struct DirectView {
    std::uint64_t first;
    std::uint64_t last;
    std::uint64_t address;
};

// Every direct view, sorted by physical address; rebuilt when mappings change (tracking mutex held).
const std::vector<DirectView>& directViews() {
    static auto* views = new std::vector<DirectView>;
    static std::uint64_t built = std::numeric_limits<std::uint64_t>::max();
    const auto generation = mappingGeneration.load(std::memory_order_acquire);
    if (built != generation) {
        views->clear();
        for (const auto& [begin, record] : areas()) {
            if (record.kind == Kind::Direct) views->push_back({record.offset, record.offset + (record.end - begin), begin});
        }
        std::sort(views->begin(), views->end(), [](const DirectView& left, const DirectView& right) { return left.first < right.first; });
        built = generation;
    }
    return *views;
}

// The direct views that show physical bytes of [address, last) (the range's own pieces included),
// each cut to those bytes; empty when no other guest address shows any of them (tracking mutex held).
std::vector<DirectView> aliasGroup(std::uint64_t address, std::uint64_t last) {
    const auto& views = directViews();
    std::vector<DirectView> group;
    if (views.size() < 2) return group;
    bool aliased = false;
    for (const auto begin : overlapping(address, last)) {
        const auto& record = areas().at(begin);
        if (record.kind != Kind::Direct) continue;
        const auto first = record.offset + (std::max(address, begin) - begin);
        const auto end = record.offset + (std::min(last, record.end) - begin);
        for (const auto& view : views) {
            if (view.first >= end) break;
            if (view.last <= first) continue;
            const auto overlapFirst = std::max(first, view.first);
            const auto overlapLast = std::min(end, view.last);
            const auto viewAddress = view.address + (overlapFirst - view.first);
            group.push_back({overlapFirst, overlapLast, viewAddress});
            if (viewAddress != begin + (overlapFirst - record.offset)) aliased = true;
        }
    }
    if (!aliased) group.clear();
    return group;
}

// Visits the committed pieces covering [address, address + bytes) as (area start, record, begin,
// end); contiguous guest memory may span several areas.
template<typename TVisit>
void forEachPiece(std::uint64_t address, std::size_t bytes, TVisit&& visit) {
    if (address == 0 || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) throw std::invalid_argument("invalid guest backing range");
    const auto last = address + bytes;
    const auto unmapped = [&] {
        char message[96];
        std::snprintf(message, sizeof(message), "guest memory backing range 0x%llx+0x%zx is unmapped", static_cast<unsigned long long>(address), bytes);
        throw std::runtime_error(message);
    };
    auto& map = areas();
    auto it = map.upper_bound(address);
    if (it == map.begin()) unmapped();
    --it;
    for (auto cursor = address; cursor < last; ++it) {
        if (it == map.end() || it->first > cursor || it->second.end <= cursor || !committed(it->second.kind)) unmapped();
        const auto end = std::min(last, it->second.end);
        visit(it->first, it->second, cursor, end);
        cursor = end;
    }
}

}

extern "C" {

Status GuestVirtualMap_nid_postfix(void** address, std::size_t bytes, std::size_t alignment, Kind kind, int protection, int flags, std::int64_t physical) {
    constexpr int kKnownFlags = kMapFixed | kMapNoOverwrite | kMapNoCoalesce | 0x400 | 0x1000 | 0x2000 | 0x8000 | 0x20000 | static_cast<int>(0xff000000u);
    if (address == nullptr || bytes == 0 || bytes % kPageBytes != 0 || (flags & ~kKnownFlags) != 0 || (protection & ~0x3f7) != 0) return Status::Invalid;
    // Bits 24-31 of the flags may request an alignment of 1 << n (KytyPS5).
    if (const auto shift = static_cast<unsigned>(flags) >> 24u; shift != 0) {
        if (shift < 14u || shift > 31u) return Status::Invalid;
        alignment = std::max<std::size_t>(alignment, std::size_t{1} << shift);
    }
    if (alignment == 0) alignment = kPageBytes;
    if (alignment < kPageBytes || (alignment & (alignment - 1)) != 0) return Status::Invalid;
    if ((kind == Kind::Direct || kind == Kind::Flexible) && (protection & kProtCpuExec) != 0) return Status::Access;
    if (kind == Kind::Direct && (physical < 0 || static_cast<std::uint64_t>(physical) % kPageBytes != 0 || static_cast<std::uint64_t>(physical) >= kPhysicalBytes || bytes > kPhysicalBytes - static_cast<std::uint64_t>(physical))) return Status::Invalid;
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    auto target = reinterpret_cast<std::uint64_t>(*address);
    const bool fixed = (flags & kMapFixed) != 0;
    if (fixed) {
        if (target == 0 || target % alignment != 0 || bytes > std::numeric_limits<std::uint64_t>::max() - target) return Status::Invalid;
        const auto last = target + bytes;
        if ((flags & kMapNoOverwrite) != 0 && anyOverlap(target, last)) return Status::NoMemory;
        for (const auto begin : overlapping(target, last)) {
            if (areas().at(begin).kind == Kind::Heap) return Status::NoMemory;
        }
        // Everything is checked before anything changes: the gaps are claimed first.
        if (!claimGaps(target, last)) return Status::NoMemory;
        GuestAllocations::GuestAllocationsRequireUnpinned_nid_postfix(nullptr, reinterpret_cast<const void*>(target), bytes);
        GuestMemoryTracking::GuestMemoryTrackingInvalidate_nid_postfix(target, bytes);
        carve(target, last);
    } else {
        // The address is a search hint.
        target = Platform::ClaimRange(target, bytes, alignment, false);
        if (target == 0) return Status::NoMemory;
    }
    mappingGeneration.fetch_add(1, std::memory_order_acq_rel);
    Record record{target + bytes, kind, protection, (flags & kMapNoCoalesce) != 0, nullptr, 0};
    try {
        if (kind == Kind::Direct) {
            record.segment = physicalSegment();
            record.offset = static_cast<std::uint64_t>(physical);
        } else if (committed(kind)) {
            record.segment = std::make_shared<SegmentHolder>(bytes);
        }
        if (committed(kind)) Platform::MapView(target, bytes, record.segment->segment, record.offset, hostProtection(protection));
        areas().emplace(target, record);
        registerCommitted(target, target + bytes, record);
    } catch (...) {
        areas().erase(target);
        Platform::ReleaseRange(target, bytes);
        throw;
    }
    *address = reinterpret_cast<void*>(target);
    return Status::Ok;
}

Status GuestVirtualUnmap_nid_postfix(void* address, std::size_t bytes) {
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    return unmapRange(reinterpret_cast<std::uint64_t>(address), bytes, false);
}

Status GuestVirtualProtect_nid_postfix(const void* pointer, std::size_t bytes, int protection) {
    const auto address = reinterpret_cast<std::uint64_t>(pointer);
    if (!validRange(address, bytes) || (protection & ~0x3f7) != 0) return Status::Invalid;
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    const auto last = address + bytes;
    if (!anyOverlap(address, last)) return Status::Invalid;
    GuestAllocations::GuestAllocationsRequireUnpinned_nid_postfix(nullptr, pointer, bytes);
    mappingGeneration.fetch_add(1, std::memory_order_acq_rel);
    split(address);
    split(last);
    // Gaps are skipped; reserved areas only record the protection.
    for (const auto begin : overlapping(address, last)) {
        auto& record = areas().at(begin);
        record.protection = protection;
        if (!committed(record.kind)) continue;
        Platform::Protect(begin, static_cast<std::size_t>(record.end - begin), hostProtection(protection));
        if (record.kind == Kind::Heap) continue;
        GuestAllocations::GuestAllocationsCarve_nid_postfix(nullptr, reinterpret_cast<const void*>(begin), static_cast<std::size_t>(record.end - begin), [] {});
        registerCommitted(begin, record.end, record);
    }
    return Status::Ok;
}

Status GuestVirtualReleasePhysical_nid_postfix(std::int64_t physical, std::size_t bytes) {
    if (physical < 0 || bytes == 0 || static_cast<std::uint64_t>(physical) >= kPhysicalBytes) return Status::Invalid;
    const auto first = static_cast<std::uint64_t>(physical);
    const auto last = first + bytes;
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> views;
    for (const auto& [begin, record] : areas()) {
        if (record.kind != Kind::Direct) continue;
        const auto viewFirst = record.offset;
        const auto viewLast = record.offset + (record.end - begin);
        const auto overlapFirst = std::max(first, viewFirst);
        const auto overlapLast = std::min(last, viewLast);
        if (overlapFirst < overlapLast) views.emplace_back(begin + (overlapFirst - viewFirst), begin + (overlapLast - viewFirst));
    }
    for (const auto& [begin, end] : views) {
        const auto status = unmapRange(begin, static_cast<std::size_t>(end - begin), false);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

bool GuestVirtualQuery_nid_postfix(const void* pointer, bool findNext, Area* area) {
    if (area == nullptr) return false;
    const auto address = reinterpret_cast<std::uint64_t>(pointer);
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    auto& map = areas();
    auto it = map.upper_bound(address);
    if (it != map.begin() && std::prev(it)->second.end > address) --it;
    else if (!findNext || it == map.end()) return false;
    // Neighbours merge like shadPS4's CanMergeWith: same kind and protection, contiguous physical
    // memory, and neither mapped with NO_COALESCE.
    const auto mergeable = [](std::uint64_t leftBegin, const Record& left, std::uint64_t rightBegin, const Record& right) {
        if (left.end != rightBegin || left.kind != right.kind || left.protection != right.protection || left.noCoalesce || right.noCoalesce) return false;
        return left.kind != Kind::Direct || left.offset + (left.end - leftBegin) == right.offset;
    };
    auto first = it;
    while (first != map.begin() && mergeable(std::prev(first)->first, std::prev(first)->second, first->first, first->second)) --first;
    auto last = it;
    while (std::next(last) != map.end() && mergeable(last->first, last->second, std::next(last)->first, std::next(last)->second)) ++last;
    *area = {first->first, last->second.end - first->first, first->second.kind, first->second.protection, first->second.kind == Kind::Direct ? static_cast<std::int64_t>(first->second.offset) : -1, first->second.noCoalesce};
    return true;
}

bool GuestVirtualAccessible_nid_postfix(std::uint64_t address, std::uint64_t bytes, bool writable) {
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return false;
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    auto& map = areas();
    auto it = map.upper_bound(address);
    if (it == map.begin()) return false;
    --it;
    const auto end = address + bytes;
    for (auto cursor = address; cursor < end; ++it) {
        if (it == map.end() || it->first > cursor || it->second.end <= cursor) return false;
        const auto& record = it->second;
        if (!committed(record.kind) || !hostReadable(record.protection) || (writable && !hostWritable(record.protection))) return false;
        cursor = record.end;
    }
    return true;
}

bool GuestVirtualTranslate_nid_postfix(std::uint64_t address, std::uint64_t bytes, Translation* translation) {
    if (translation == nullptr || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return false;
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    auto& map = areas();
    auto it = map.upper_bound(address);
    if (it == map.begin()) return false;
    --it;
    if (it->second.end <= address || !committed(it->second.kind)) return false;
    const auto& first = it->second;
    const auto offset = first.offset + (address - it->first);
    const auto last = address + bytes;
    auto covered = std::min(last, first.end);
    // Adjacent views of the next segment bytes extend the translation.
    for (auto next = std::next(it); covered < last && next != map.end() && next->first == covered && committed(next->second.kind) && next->second.segment == first.segment && next->second.offset == offset + (covered - address); ++next) {
        covered = std::min(last, next->second.end);
    }
    *translation = {first.segment->id, first.segment->segment.alias, first.segment->segment.bytes, offset, covered - address};
    return true;
}

bool GuestVirtualSingleView_nid_postfix(std::uint64_t address, std::uint64_t bytes) {
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return false;
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    auto& map = areas();
    const auto last = address + bytes;
    auto it = map.upper_bound(address);
    if (it == map.begin()) return false;
    --it;
    auto cursor = address;
    for (; cursor < last; ++it) {
        if (it == map.end() || it->first > cursor || it->second.end <= cursor || !committed(it->second.kind)) return false;
        const auto& record = it->second;
        const auto first = record.offset + (cursor - it->first);
        const auto end = record.offset + (std::min(last, record.end) - it->first);
        for (const auto& [otherBegin, other] : map) {
            if (other.segment != record.segment || otherBegin == it->first || !committed(other.kind)) continue;
            const auto otherFirst = other.offset;
            const auto otherEnd = other.offset + (other.end - otherBegin);
            if (otherFirst < end && first < otherEnd) return false;
        }
        cursor = std::min(last, record.end);
    }
    return true;
}

bool GuestWriteWatchAvailable_nid_postfix() {
    return Platform::WriteWatchAvailable();
}

bool GuestWriteWatchCollect_nid_postfix(std::uint64_t address, std::uint64_t bytes, void (*visit)(void* context, std::uint64_t begin, std::uint64_t end), void* context) {
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return false;
    // Direct memory shown at other guest addresses too: a write through any view changes every
    // view, but the watch sees it only in the view written through. The other views are scanned as
    // well and every write is reported at each view of its bytes, so it also stays visible to
    // whoever collects another view later.
    std::vector<DirectView> group;
    {
        std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
        group = aliasGroup(address, address + bytes);
    }
    // No lock while scanning: the kernel orders scans with mapping changes, and a newly mapped
    // view's pages are reported as written until they are first collected.
    if (group.empty()) return Platform::CollectWrites(address, static_cast<std::size_t>(bytes), visit, context);
    struct Fanout {
        const std::vector<DirectView>* group;
        void (*visit)(void*, std::uint64_t, std::uint64_t);
        void* context;
    } fanout{&group, visit, context};
    const auto report = [](void* opaque, std::uint64_t begin, std::uint64_t end) {
        auto& fan = *static_cast<Fanout*>(opaque);
        fan.visit(fan.context, begin, end);
        for (const auto& written : *fan.group) {
            const auto writtenEnd = written.address + (written.last - written.first);
            if (end <= written.address || writtenEnd <= begin) continue;
            const auto physicalFirst = written.first + (std::max(begin, written.address) - written.address);
            const auto physicalLast = written.first + (std::min(end, writtenEnd) - written.address);
            for (const auto& view : *fan.group) {
                const auto first = std::max(physicalFirst, view.first);
                const auto last = std::min(physicalLast, view.last);
                if (first < last) fan.visit(fan.context, view.address + (first - view.first), view.address + (last - view.first));
            }
        }
    };
    if (!Platform::CollectWrites(address, static_cast<std::size_t>(bytes), report, &fanout)) return false;
    const auto last = address + bytes;
    for (const auto& view : group) {
        const auto viewEnd = view.address + (view.last - view.first);
        if (view.address >= address && viewEnd <= last) continue;
        if (!Platform::CollectWrites(view.address, static_cast<std::size_t>(viewEnd - view.address), report, &fanout)) return false;
    }
    return true;
}

bool GuestWriteWatchAddHost_nid_postfix(std::uint64_t address, std::uint64_t bytes) {
    return bytes != 0 && Platform::WatchHost(address, static_cast<std::size_t>(bytes));
}

bool GuestSegmentAlive_nid_postfix(std::uint64_t segment) {
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    return liveSegments().contains(segment);
}

void* GuestMemoryBackingMap_nid_postfix(void* address, std::size_t bytes, std::size_t alignment, int protection) {
    void* result = address;
    const auto status = GuestVirtualMap_nid_postfix(&result, bytes, alignment, Kind::Heap, protection & 7, address != nullptr ? kMapFixed | kMapNoOverwrite : 0, -1);
    if (status != Status::Ok) throw std::runtime_error("guest heap mapping failed");
    return result;
}

void GuestMemoryBackingUnmap_nid_postfix(void* address, std::size_t bytes) {
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    if (unmapRange(reinterpret_cast<std::uint64_t>(address), bytes, true) != Status::Ok) throw std::invalid_argument("invalid guest heap unmap");
}

std::uint64_t GuestMemoryBackingGeneration_nid_postfix() {
    return mappingGeneration.load(std::memory_order_acquire);
}

void GuestMemoryBackingNoteChange_nid_postfix() {
    mappingGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void GuestMemoryBackingRequire_nid_postfix(std::uint64_t address, std::size_t bytes) {
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    forEachPiece(address, bytes, [](std::uint64_t, const Record&, std::uint64_t, std::uint64_t) {});
}

void GuestMemoryBackingWrite_nid_postfix(std::uint64_t address, const void* source, std::size_t bytes) {
    if (source == nullptr) throw std::invalid_argument("missing guest backing write source");
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    forEachPiece(address, bytes, [&](std::uint64_t areaBegin, const Record& record, std::uint64_t begin, std::uint64_t end) {
        auto* destination = static_cast<std::byte*>(record.segment->segment.alias) + record.offset + (begin - areaBegin);
        std::memcpy(destination, static_cast<const std::byte*>(source) + (begin - address), static_cast<std::size_t>(end - begin));
    });
}

}

}
