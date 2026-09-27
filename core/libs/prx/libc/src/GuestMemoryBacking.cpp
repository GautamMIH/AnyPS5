#include "prx/libc/include/GuestMemoryBacking.hpp"
#include "prx/libc/include/MemoryBackingPlatform.hpp"
#include "prx/libc/include/GuestMemoryTracking.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <limits>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace GuestMemoryBacking {
namespace {

struct Allocation {
    Platform::Mapping mapping;
    std::map<std::uint64_t, std::uint64_t> ranges;
    std::map<std::uint64_t, std::uint64_t> owned;
};

std::map<std::uint64_t, std::uint64_t> removeInterval(const std::map<std::uint64_t, std::uint64_t>& segments, std::uint64_t first, std::uint64_t last, std::map<std::uint64_t, std::uint64_t>* removed) {
    std::map<std::uint64_t, std::uint64_t> result;
    for (const auto& [begin, end] : segments) {
        if (end <= first || begin >= last) {
            result.emplace(begin, end);
            continue;
        }
        if (begin < first) result.emplace(begin, first);
        if (last < end) result.emplace(last, end);
        if (removed != nullptr) removed->emplace(std::max(begin, first), std::min(end, last));
    }
    return result;
}

void releaseAllocation(const Allocation& allocation) {
    if (allocation.owned.size() == 1 && allocation.owned.begin()->first == allocation.mapping.address && allocation.owned.begin()->second == allocation.mapping.address + allocation.mapping.bytes) {
        Platform::Unmap(allocation.mapping);
        return;
    }
    for (const auto& [begin, end] : allocation.owned)
        Platform::Release(begin, static_cast<std::size_t>(end - begin));
    Platform::ReleaseAlias(allocation.mapping);
}

std::map<std::uint64_t, Allocation>& allocations() {
    static auto* value = new std::map<std::uint64_t, Allocation>;
    return *value;
}

std::atomic<std::uint64_t> mappingGeneration{0};

// Mapped pieces by start address, with their end and owning allocation. Allocations are keyed by
// their first owned address, and a fixed mapping carved into the middle of one leaves its tail
// after the new allocation's key, so the owner of an address is found through its piece, not by
// the nearest allocation key. Rebuilt whenever the mapping generation changes; map nodes keep
// their addresses across re-keying.
struct PieceIndex {
    std::uint64_t generation = ~0ull;
    std::map<std::uint64_t, std::pair<std::uint64_t, Allocation*>> pieces;
};

// Visits the mapped pieces covering [address, address + bytes) in order, as (allocation, begin,
// end) clipped to the range. Contiguous guest memory may span several mappings (the PS5 lets a
// game treat adjacent mappings as one object), so pieces of different allocations are joined.
template<typename TVisit>
void forEachPiece(std::uint64_t address, std::size_t bytes, TVisit&& visit) {
    if (address == 0 || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) throw std::invalid_argument("invalid guest backing range");
    static PieceIndex index;
    const auto generation = mappingGeneration.load(std::memory_order_acquire);
    if (index.generation != generation) {
        index.pieces.clear();
        for (auto& [key, allocation] : allocations()) {
            for (const auto& [begin, end] : allocation.ranges) index.pieces[begin] = {end, &allocation};
        }
        index.generation = generation;
    }
    const auto last = address + bytes;
    const auto unmapped = [&] {
        char message[96];
        std::snprintf(message, sizeof(message), "guest memory backing range 0x%llx+0x%zx is unmapped", static_cast<unsigned long long>(address), bytes);
        throw std::runtime_error(message);
    };
    auto piece = index.pieces.upper_bound(address);
    if (piece == index.pieces.begin()) unmapped();
    --piece;
    for (auto cursor = address; cursor < last; ++piece) {
        if (piece == index.pieces.end() || piece->first > cursor || piece->second.first <= cursor) unmapped();
        const auto end = std::min(last, piece->second.first);
        visit(*piece->second.second, cursor, end);
        cursor = end;
    }
}


}

std::uint64_t GuestMemoryBackingGeneration_nid_postfix() {
    return mappingGeneration.load(std::memory_order_acquire);
}

void GuestMemoryBackingNoteChange_nid_postfix() {
    mappingGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void* GuestMemoryBackingMap_nid_postfix(void* address, std::size_t bytes, std::size_t alignment, int protection) {
    const auto pageSize = GuestMemoryTracking::GuestMemoryTrackingPageSize_nid_postfix();
    if (bytes == 0 || bytes % pageSize != 0 || alignment < pageSize || (alignment & (alignment - 1)) != 0 || (protection & ~7) != 0) throw std::invalid_argument("invalid shared guest memory mapping");
    if (address != nullptr && reinterpret_cast<std::uintptr_t>(address) % alignment != 0) throw std::invalid_argument("misaligned fixed guest memory mapping");
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    GuestMemoryBackingNoteChange_nid_postfix();
    const auto mapping = Platform::Map(address, bytes, alignment, protection);
    try {
        if (mapping.bytes > std::numeric_limits<std::uint64_t>::max() - mapping.address) throw std::overflow_error("guest backing mapping overflow");
        const auto mappingEnd = mapping.address + bytes;
        for (const auto& [base, existing] : allocations()) {
            if (base >= mappingEnd)
                break;
            for (const auto& [begin, end] : existing.owned)
                if (begin < mappingEnd && end > mapping.address) throw std::runtime_error("overlapping guest backing mappings");
        }
        Allocation allocation{mapping, {{mapping.address, mapping.address + bytes}}, {{mapping.address, mapping.address + bytes}}};
        if (!allocations().emplace(mapping.address, std::move(allocation)).second) throw std::runtime_error("duplicate guest backing mapping");
    } catch (...) {
        Platform::Unmap(mapping);
        throw;
    }
    return reinterpret_cast<void*>(mapping.address);
}

void GuestMemoryBackingUnmap_nid_postfix(void* pointer, std::size_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto pageSize = GuestMemoryTracking::GuestMemoryTrackingPageSize_nid_postfix();
    if (address == 0 || bytes == 0 || address % pageSize != 0 || bytes % pageSize != 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) throw std::invalid_argument("misaligned guest backing unmap");
    const auto last = address + bytes;
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    GuestMemoryBackingNoteChange_nid_postfix();
    // Like munmap, the range may cover several allocations and already-unmapped gaps; every mapped
    // piece inside it is removed, and an allocation with no mapped pieces left is released.
    bool unmapped = false;
    GuestMemoryTracking::GuestMemoryTrackingInvalidate_nid_postfix(address, bytes);
    for (auto it = allocations().begin(); it != allocations().end();) {
        auto& allocation = it->second;
        std::map<std::uint64_t, std::uint64_t> removed;
        auto remaining = removeInterval(allocation.ranges, address, last, &removed);
        if (removed.empty()) {
            ++it;
            continue;
        }
        unmapped = true;
        if (remaining.empty()) {
            releaseAllocation(allocation);
            it = allocations().erase(it);
            continue;
        }
        for (const auto& [begin, end] : removed)
            Platform::Deactivate(begin, static_cast<std::size_t>(end - begin));
        allocation.ranges.swap(remaining);
        ++it;
    }
    if (!unmapped) throw std::runtime_error("guest memory backing range is unmapped");
}

void GuestMemoryBackingCarve_nid_postfix(void* pointer, std::size_t bytes) {
    const auto first = reinterpret_cast<std::uintptr_t>(pointer);
    const auto pageSize = GuestMemoryTracking::GuestMemoryTrackingPageSize_nid_postfix();
    if (first == 0 || bytes == 0 || first % pageSize != 0 || bytes % pageSize != 0 || bytes > std::numeric_limits<std::uint64_t>::max() - first) throw std::invalid_argument("invalid guest backing carve range");
    const auto last = first + bytes;
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    GuestMemoryBackingNoteChange_nid_postfix();
    GuestMemoryTracking::GuestMemoryTrackingInvalidate_nid_postfix(first, bytes);
    for (auto it = allocations().begin(); it != allocations().end();) {
        auto& allocation = it->second;
        std::map<std::uint64_t, std::uint64_t> released;
        auto owned = removeInterval(allocation.owned, first, last, &released);
        if (released.empty()) {
            ++it;
            continue;
        }
        for (const auto& [begin, end] : released)
            Platform::Release(begin, static_cast<std::size_t>(end - begin));
        allocation.owned.swap(owned);
        allocation.ranges = removeInterval(allocation.ranges, first, last, nullptr);
        if (allocation.owned.empty()) {
            Platform::ReleaseAlias(allocation.mapping);
            it = allocations().erase(it);
        } else if (allocation.owned.begin()->first != it->first) {
            auto node = allocations().extract(it++);
            node.key() = node.mapped().owned.begin()->first;
            allocations().insert(std::move(node));
        } else {
            ++it;
        }
    }
}

void GuestMemoryBackingRequire_nid_postfix(std::uint64_t address, std::size_t bytes) {
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    forEachPiece(address, bytes, [](Allocation&, std::uint64_t, std::uint64_t) {});
}

void GuestMemoryBackingWrite_nid_postfix(std::uint64_t address, const void* source, std::size_t bytes) {
    if (source == nullptr) throw std::invalid_argument("missing guest backing write source");
    std::lock_guard lock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    forEachPiece(address, bytes, [&](Allocation& allocation, std::uint64_t begin, std::uint64_t end) {
        auto* destination = static_cast<std::byte*>(allocation.mapping.alias) + (begin - allocation.mapping.address);
        std::memcpy(destination, static_cast<const std::byte*>(source) + (begin - address), static_cast<std::size_t>(end - begin));
    });
}

}
