#include "prx/libc/include/GuestMemoryTracking.hpp"
#include "prx/libc/include/MemoryTrackingPlatform.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace GuestMemoryTracking {
namespace {

// Watches may share pages (neighbouring render targets, mip levels): a page's protection is the
// strictest of the watches covering it, and its native protection returns when none protects it.
struct Entry {
    std::uint64_t address;
    std::size_t bytes;
    void* context;
    Resolver resolver;
    Protection protection = Protection::ReadWrite;
    // The native protection of the entry's pages, recorded when it was created.
    std::vector<Platform::Region> original;
    bool resolving = false;
    bool active = false;
};

struct Registry {
    std::recursive_mutex mutex;
    std::multimap<std::uint64_t, std::shared_ptr<Entry>> entries;
    // The longest entry: entries overlapping an address start at most this far before it.
    std::size_t largest = 0;
    bool installed = false;
};

Registry& registry() {
    static auto* value = new Registry;
    return *value;
}

std::uint64_t checkedEnd(std::uint64_t address, std::size_t bytes) {
    if (address == 0 || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) throw std::invalid_argument("invalid tracked guest memory range");
    return address + bytes;
}

void resolve(const std::shared_ptr<Entry>& entry, Access access) {
    if (entry->resolving) throw std::runtime_error("recursive guest memory ownership resolution");
    entry->resolving = true;
    try {
        entry->resolver(entry->context, access);
        if (entry->protection == Protection::None || (access != Access::Read && entry->protection != Protection::ReadWrite)) throw std::runtime_error("guest memory resolver did not release CPU access");
        entry->resolving = false;
    } catch (...) {
        entry->resolving = false;
        throw;
    }
}

std::multimap<std::uint64_t, std::shared_ptr<Entry>>::const_iterator firstCandidate(std::uint64_t address) {
    const auto& registryValue = registry();
    return registryValue.entries.lower_bound(address > registryValue.largest ? address - registryValue.largest : 0);
}

std::vector<std::shared_ptr<Entry>> overlapping(std::uint64_t address, std::size_t bytes) {
    const auto end = checkedEnd(address, bytes);
    const auto& entries = registry().entries;
    std::vector<std::shared_ptr<Entry>> result;
    for (auto it = firstCandidate(address); it != entries.end() && it->first < end; ++it) {
        if (it->first + it->second->bytes > address) result.push_back(it->second);
    }
    return result;
}

// The regions' parts within [first, last).
std::vector<Platform::Region> clip(const std::vector<Platform::Region>& regions, std::uint64_t first, std::uint64_t last) {
    std::vector<Platform::Region> result;
    for (const auto& region : regions) {
        const auto from = std::max(first, region.address);
        const auto to = std::min<std::uint64_t>(last, region.address + region.bytes);
        if (from < to) result.push_back({from, static_cast<std::size_t>(to - from), region.protection});
    }
    return result;
}

// Sets the pages of [first, last) to the strictest protection of the entries covering them, or to
// their native protection (from native, or any covering entry) where no entry protects them.
void apply(std::uint64_t first, std::uint64_t last, const std::vector<Platform::Region>& native) {
    const auto entries = overlapping(first, static_cast<std::size_t>(last - first));
    std::vector<std::uint64_t> cuts{first, last};
    for (const auto& entry : entries) {
        if (entry->address > first) cuts.push_back(entry->address);
        if (entry->address + entry->bytes < last) cuts.push_back(entry->address + entry->bytes);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    for (std::size_t index = 0; index + 1 < cuts.size(); ++index) {
        const auto from = cuts[index];
        const auto to = cuts[index + 1];
        auto protection = Protection::ReadWrite;
        const std::vector<Platform::Region>* source = &native;
        for (const auto& entry : entries) {
            if (entry->address > from || entry->address + entry->bytes < to) continue;
            protection = std::min(protection, entry->protection);
            source = &entry->original;
        }
        if (protection == Protection::ReadWrite) Platform::Restore(clip(*source, from, to));
        else Platform::Protect(from, static_cast<std::size_t>(to - from), protection);
    }
}

bool fault(std::uint64_t address, bool writable) {
    std::lock_guard lock(registry().mutex);
    const auto entries = overlapping(address, 1);
    if (entries.empty()) return false;
    bool handled = false;
    for (const auto& entry : entries) {
        if (!entry->active) continue;
        if (entry->protection == Protection::None || (writable && entry->protection == Protection::Read)) resolve(entry, writable ? Access::Write : Access::Read);
        handled = true;
    }
    return handled;
}

}

std::recursive_mutex& GuestMemoryTrackingMutex_nid_postfix() {
    return registry().mutex;
}

std::size_t GuestMemoryTrackingPageSize_nid_postfix() {
    return Platform::PageSize();
}

void* GuestMemoryTrackingCreate_nid_postfix(std::uint64_t address, std::size_t bytes, void* context, Resolver resolver) {
    const auto end = checkedEnd(address, bytes);
    const auto pageSize = Platform::PageSize();
    if (context == nullptr || resolver == nullptr) throw std::invalid_argument("missing guest memory ownership resolver");
    if (end > std::numeric_limits<std::uint64_t>::max() - (pageSize - 1)) throw std::overflow_error("tracked guest memory page range overflow");
    const auto first = address - address % pageSize;
    const auto last = (end + pageSize - 1) / pageSize * pageSize;
    std::lock_guard lock(registry().mutex);
    GuestMemoryBacking::GuestMemoryBackingRequire_nid_postfix(first, static_cast<std::size_t>(last - first));
    // Native protection: queried where no entry watches the pages, taken from the entries that do
    // (their pages may be protected now).
    std::vector<Platform::Region> original;
    auto cursor = first;
    for (const auto& other : overlapping(first, static_cast<std::size_t>(last - first))) {
        const auto from = std::max(cursor, other->address);
        const auto to = std::min<std::uint64_t>(last, other->address + other->bytes);
        if (to <= cursor) continue;
        if (cursor < from) {
            auto queried = Platform::Query(cursor, static_cast<std::size_t>(from - cursor));
            original.insert(original.end(), queried.begin(), queried.end());
        }
        auto shared = clip(other->original, from, to);
        original.insert(original.end(), shared.begin(), shared.end());
        cursor = to;
    }
    if (cursor < last) {
        auto queried = Platform::Query(cursor, static_cast<std::size_t>(last - cursor));
        original.insert(original.end(), queried.begin(), queried.end());
    }
    if (!registry().installed) {
        Platform::Install(fault);
        registry().installed = true;
    }
    auto entry = std::make_shared<Entry>();
    entry->address = first;
    entry->bytes = static_cast<std::size_t>(last - first);
    entry->context = context;
    entry->resolver = resolver;
    entry->original = std::move(original);
    auto handle = std::make_unique<std::shared_ptr<Entry>>(entry);
    registry().largest = std::max(registry().largest, entry->bytes);
    registry().entries.emplace(first, std::move(entry));
    return handle.release();
}

void GuestMemoryTrackingDestroy_nid_postfix(void* handle) noexcept {
    if (handle == nullptr) std::terminate();
    std::lock_guard lock(registry().mutex);
    std::unique_ptr<std::shared_ptr<Entry>> owner(static_cast<std::shared_ptr<Entry>*>(handle));
    const auto& entry = **owner;
    auto& entries = registry().entries;
    for (auto [it, end] = entries.equal_range(entry.address); it != end; ++it) {
        if (it->second.get() != &entry) continue;
        entries.erase(it);
        break;
    }
    if (entry.protection != Protection::ReadWrite) apply(entry.address, entry.address + entry.bytes, entry.original);
}

void GuestMemoryTrackingProtect_nid_postfix(void* handle, Protection protection) {
    if (handle == nullptr) throw std::invalid_argument("missing guest memory watch");
    std::lock_guard lock(registry().mutex);
    auto& entry = **static_cast<std::shared_ptr<Entry>*>(handle);
    if (entry.protection == protection) return;
    if (protection != Protection::ReadWrite) entry.active = true;
    entry.protection = protection;
    apply(entry.address, entry.address + entry.bytes, entry.original);
}

void GuestMemoryTrackingResolve_nid_postfix(std::uint64_t address, std::size_t bytes, bool writable) {
    if (bytes == 0) return;
    const auto end = checkedEnd(address, bytes);
    std::lock_guard lock(registry().mutex);
    // Checked thousands of times a frame: find whether a watch needs resolving before copying the
    // overlapping entries (resolving may change the registry).
    const auto& entries = registry().entries;
    bool needed = false;
    for (auto it = firstCandidate(address); it != entries.end() && it->first < end && !needed; ++it) {
        const auto& entry = *it->second;
        needed = it->first + entry.bytes > address && (entry.protection == Protection::None || (writable && entry.protection == Protection::Read));
    }
    if (!needed) return;
    for (const auto& entry : overlapping(address, bytes)) {
        if (entry->protection == Protection::None || (writable && entry->protection == Protection::Read)) resolve(entry, writable ? Access::Write : Access::Read);
    }
}

void GuestMemoryTrackingInvalidate_nid_postfix(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0) return;
    std::lock_guard lock(registry().mutex);
    for (const auto& entry : overlapping(address, bytes)) {
        resolve(entry, Access::Invalidate);
        entry->active = false;
    }
}

void GuestMemoryTrackingValidate_nid_postfix(std::uint64_t address, std::size_t bytes, const std::function<void(std::uint64_t, std::size_t)>& validate) {
    if (bytes == 0) return;
    if (!validate) throw std::invalid_argument("missing native memory range validator");
    const auto end = checkedEnd(address, bytes);
    std::lock_guard lock(registry().mutex);
    for (const auto& entry : overlapping(address, bytes)) {
        const auto last = std::min(end, entry->address + entry->bytes);
        if (last <= address) continue;
        if (address < entry->address) validate(address, static_cast<std::size_t>(entry->address - address));
        const auto first = std::max(address, entry->address);
        if (entry->protection == Protection::ReadWrite) validate(first, static_cast<std::size_t>(last - first));
        else if (entry->original.empty()) throw std::runtime_error("protected guest memory has no native permission record");
        address = last;
    }
    if (address < end) validate(address, static_cast<std::size_t>(end - address));
}

}
