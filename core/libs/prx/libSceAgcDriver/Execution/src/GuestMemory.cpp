#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestMemoryTracking.hpp"
#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <dlfcn.h>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>
#endif

namespace AgcDriver::GuestMemory {
namespace {
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("AGC driver: ") + reason);
}

#ifndef _WIN32
// /proc/self/maps is too slow to parse for every check (draws make hundreds of thousands per
// frame), so a snapshot is reused until guest mappings change or a range is not covered by it.
struct MappingEntry {
    std::uintptr_t first;
    std::uintptr_t last;
    std::string permissions;
};

struct MappingCache {
    std::mutex mutex;
    std::uint64_t generation = std::numeric_limits<std::uint64_t>::max();
    std::vector<MappingEntry> entries;
    std::vector<std::tuple<std::uintptr_t, std::uintptr_t, bool>> failed;
};

MappingCache& mappingCache() {
    static MappingCache cache;
    return cache;
}

void loadMappings(std::vector<MappingEntry>& entries) {
    entries.clear();
    std::ifstream maps("/proc/self/maps");
    require(maps.is_open(), "cannot query guest memory maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream fields(line);
        MappingEntry entry{};
        char separator = 0;
        require(static_cast<bool>(fields >> std::hex >> entry.first >> separator >> entry.last >> entry.permissions) && separator == '-' && entry.first < entry.last && !entry.permissions.empty(), "invalid guest memory map entry");
        entries.push_back(std::move(entry));
    }
}

// Empty when [address, end) is mapped readable (and writable if requested); otherwise the failure.
std::string checkMappings(const std::vector<MappingEntry>& entries, std::uintptr_t address, std::uintptr_t end, bool writable) {
    auto it = std::upper_bound(entries.begin(), entries.end(), address, [](std::uintptr_t value, const MappingEntry& entry) { return value < entry.last; });
    auto cursor = address;
    for (; cursor < end && it != entries.end(); ++it) {
        if (!(it->first <= cursor && it->permissions[0] == 'r')) {
            char detail[160]{};
            std::snprintf(detail, sizeof(detail), "guest memory is not readable (range 0x%llx+0x%llx, at 0x%llx: mapping 0x%llx-0x%llx %s)", static_cast<unsigned long long>(address), static_cast<unsigned long long>(end - address), static_cast<unsigned long long>(cursor), static_cast<unsigned long long>(it->first), static_cast<unsigned long long>(it->last), it->permissions.c_str());
            return detail;
        }
        if (writable && !(it->permissions.size() > 1 && it->permissions[1] == 'w')) return "guest memory has no write permission";
        cursor = std::min(end, it->last);
    }
    return cursor == end ? std::string() : std::string("guest address range is not mapped");
}
#endif
}

void CheckRange(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable) {
    require(alignment != 0, "zero guest memory alignment");
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (address == 0 || address % alignment != 0) {
        char detail[96]{};
        std::snprintf(detail, sizeof(detail), " (address 0x%llx, %zu bytes, alignment %zu)", static_cast<unsigned long long>(address), bytes, alignment);
        require(false, (std::string("null or misaligned address") + detail).c_str());
    }
    require(bytes <= std::numeric_limits<std::uintptr_t>::max() - address, "address range overflow");
    PerformanceTimer timing("GuestMemory.CheckRange");
    // A GPU consumer (GpuAccessScope) is served by the render cache's resolve: it queues the
    // surfaces' write-backs ahead of the consumer on the draw queue. The page watches (owned by the
    // same resident surfaces) serve CPU accesses; resolving them here would wait for the queue.
    const bool gpuConsumer = GpuAccessScope::Active() && MemoryAccessScope::HasResolver();
    MemoryAccessScope::Resolve(address, bytes, writable);
    timing.Mark("scope_resolve");
    if (!gpuConsumer) {
        GuestMemoryTracking::GuestMemoryTrackingResolve_nid_postfix(address, bytes, writable);
        timing.Mark("tracking_resolve");
    }
    auto cursor = address;
    const auto end = address + bytes;
#ifdef _WIN32
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        require(VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == sizeof(memory), "cannot query guest memory");
        require(memory.State == MEM_COMMIT && (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0, "guest memory is not readable");
        const auto protection = memory.Protect & 0xffu;
        require(protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY, "guest memory has no read permission");
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        require(memory.RegionSize <= std::numeric_limits<std::uintptr_t>::max() - base && base + memory.RegionSize > cursor, "invalid guest memory mapping");
        require(!writable || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY, "guest memory has no write permission");
        cursor = std::min(end, base + memory.RegionSize);
    }
#else
    // Guest memory is answered from libc's area map. /proc/self/maps serves other memory; its
    // snapshot goes stale whenever watches change page protection, and a reload costs ~20 ms.
    if (GuestMemoryBacking::GuestVirtualAccessible_nid_postfix(address, bytes, writable)) {
        timing.Mark("area_map");
        return;
    }
    auto& cache = mappingCache();
    const std::lock_guard lock(cache.mutex);
    const auto generation = GuestMemoryBacking::GuestMemoryBackingGeneration_nid_postfix();
    if (cache.generation == generation && checkMappings(cache.entries, cursor, end, writable).empty()) return;
    // Ranges that failed against a fresh snapshot fail again without a reload while guest mappings
    // are unchanged: games probe the same unmapped descriptors every frame (guest memory never
    // reaches here, the area map answers it first).
    const auto failedBefore = std::find(cache.failed.begin(), cache.failed.end(), std::make_tuple(cursor, end, writable));
    if (cache.generation == generation && failedBefore != cache.failed.end()) {
        const auto failure = checkMappings(cache.entries, cursor, end, writable);
        require(false, failure.empty() ? "guest address range is not mapped" : failure.c_str());
    }
    loadMappings(cache.entries);
    timing.Mark(cache.generation == generation ? "reload_uncovered" : "reload_changed");
    if (cache.generation != generation) cache.failed.clear();
    cache.generation = generation;
    const auto failure = checkMappings(cache.entries, cursor, end, writable);
    if (!failure.empty() && cache.failed.size() < 256) cache.failed.emplace_back(cursor, end, writable);
    require(failure.empty(), failure.c_str());
#endif
}

#ifndef _WIN32
// Debug aid: APS5_TRACE_READS=1 totals the bytes Read copies per caller (module+offset, for
// addr2line) and prints the heaviest callers every 5000 reads.
__attribute__((noinline)) void traceRead(const void* caller, std::size_t bytes) {
    static std::mutex mutex;
    static std::map<const void*, std::pair<std::uint64_t, std::uint64_t>> callers;
    static std::uint64_t reads = 0;
    std::lock_guard lock(mutex);
    auto& total = callers[caller];
    total.first += bytes;
    ++total.second;
    if (++reads % 5000 != 0) return;
    std::vector<std::pair<const void*, std::pair<std::uint64_t, std::uint64_t>>> sorted(callers.begin(), callers.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    for (std::size_t index = 0; index < sorted.size() && index < 12; ++index) {
        Dl_info info{};
        const auto found = dladdr(sorted[index].first, &info) != 0 && info.dli_fname != nullptr;
        std::fprintf(stderr, "[reads] %10.1f MB %8llu calls  %s+0x%llx\n", static_cast<double>(sorted[index].second.first) / 1048576.0, static_cast<unsigned long long>(sorted[index].second.second), found ? info.dli_fname : "?", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(sorted[index].first) - (found ? reinterpret_cast<std::uintptr_t>(info.dli_fbase) : 0)));
    }
    callers.clear();
}
#endif

void Read(std::uint64_t address, std::span<std::byte> destination, std::size_t alignment) {
    PerformanceTimer timing("GuestMemory.Read");
    if (destination.empty()) return;
#ifndef _WIN32
    static const bool traceReads = std::getenv("APS5_TRACE_READS") != nullptr;
    if (traceReads) traceRead(__builtin_return_address(0), destination.size());
#endif
    const auto* source = reinterpret_cast<const void*>(address);
    CheckRange(source, destination.size(), alignment);
    timing.Mark("range_check");
    std::memcpy(destination.data(), source, destination.size());
    timing.Mark("copy", destination.size());
}

void Write(std::uint64_t address, std::span<const std::byte> source, std::size_t alignment) {
    PerformanceTimer timing("GuestMemory.Write");
    if (source.empty()) return;
    auto* destination = reinterpret_cast<void*>(address);
    CheckRange(destination, source.size(), alignment, true);
    timing.Mark("range_check");
    std::memcpy(destination, source.data(), source.size());
    // The driver's own CPU write: later texture validations must collect it.
    WriteTracker::NextEpoch();
    timing.Mark("copy", source.size());
}

void WriteThroughAlias(std::uint64_t address, const void* source, std::size_t bytes) {
    GuestMemoryBacking::GuestMemoryBackingWrite_nid_postfix(address, source, bytes);
    WriteTracker::NoteAliasWrite(address, bytes);
}

}

extern "C" void AgcDriverCheckGuestMemory_nid_postfix(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable) {
    AgcDriver::GuestMemory::CheckRange(pointer, bytes, alignment, writable);
}
