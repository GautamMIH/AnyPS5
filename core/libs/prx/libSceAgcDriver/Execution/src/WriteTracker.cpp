#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <functional>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

namespace AgcDriver::WriteTracker {
namespace {

constexpr std::uint64_t kSoftDirtyBit = 1ull << 55u;
constexpr std::size_t kEntriesPerRead = 512;
// Alias writes are remembered this far back; older queries answer "written".
constexpr std::size_t kAliasHistory = 4096;

std::uint64_t pageSize() {
    static const auto size = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    return size;
}

struct State {
    int pagemap = -1;
    bool available = false;
    std::atomic<std::uint64_t> clears{0};

    std::mutex mutex;
    std::map<std::uint64_t, std::uint64_t> gpuRanges;  // begin -> end, merged
    std::atomic<std::uint64_t> gpuGeneration{0};

    struct AliasWrite {
        std::uint64_t generation;
        std::uint64_t begin;
        std::uint64_t end;
    };
    std::deque<AliasWrite> aliasWrites;
    std::uint64_t aliasFloor = 0;
    std::atomic<std::uint64_t> aliasGeneration{0};

    std::mutex listenerMutex;
    std::map<const void*, std::function<void()>> listeners;
};

bool writeClearRefs() {
    const int file = open("/proc/self/clear_refs", O_WRONLY | O_CLOEXEC);
    if (file < 0) return false;
    const bool written = write(file, "4", 1) == 1;
    close(file);
    return written;
}

bool softDirty(int pagemap, std::uint64_t address, std::uint64_t bytes, bool& dirty) {
    const auto size = pageSize();
    auto page = address / size;
    const auto last = (address + bytes - 1) / size;
    std::array<std::uint64_t, kEntriesPerRead> entries{};
    while (page <= last) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(last - page + 1, kEntriesPerRead));
        const auto wanted = static_cast<ssize_t>(count * sizeof(std::uint64_t));
        if (pread(pagemap, entries.data(), static_cast<std::size_t>(wanted), static_cast<off_t>(page * sizeof(std::uint64_t))) != wanted) return false;
        for (std::size_t index = 0; index < count; ++index) {
            if ((entries[index] & kSoftDirtyBit) != 0) {
                dirty = true;
                return true;
            }
        }
        page += count;
    }
    dirty = false;
    return true;
}

// Soft-dirty tracking needs CONFIG_MEM_SOFT_DIRTY and a readable pagemap: checked once by writing a
// probe page between two clears.
State& state() {
    static State* value = [] {
        auto* result = new State;
        if (std::getenv("ANYPS5_NO_WRITE_TRACKING") != nullptr) return result;
        result->pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        if (result->pagemap < 0) return result;
        const auto size = pageSize();
        auto* probe = static_cast<volatile unsigned char*>(mmap(nullptr, size * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (probe == MAP_FAILED) return result;
        probe[0] = 1;
        probe[size] = 1;
        bool dirtyAfterClear = true;
        if (writeClearRefs() && softDirty(result->pagemap, reinterpret_cast<std::uint64_t>(probe), size * 2, dirtyAfterClear) && !dirtyAfterClear) {
            probe[0] = 2;
            bool first = false;
            bool second = true;
            if (softDirty(result->pagemap, reinterpret_cast<std::uint64_t>(probe), size, first) && softDirty(result->pagemap, reinterpret_cast<std::uint64_t>(probe) + size, size, second))
                result->available = first && !second;
        }
        munmap(const_cast<unsigned char*>(probe), size * 2);
        if (!result->available) std::fprintf(stderr, "[agc] soft-dirty page tracking is unavailable; cached textures are compared on every use\n");
        return result;
    }();
    return *value;
}

bool overlaps(const std::map<std::uint64_t, std::uint64_t>& ranges, std::uint64_t begin, std::uint64_t end) {
    auto it = ranges.upper_bound(begin);
    if (it != ranges.begin() && std::prev(it)->second > begin) return true;
    return it != ranges.end() && it->first < end;
}

}

bool Available() {
    return state().available;
}

std::uint64_t Clears() {
    return state().clears.load(std::memory_order_acquire);
}

bool CpuWritten(std::uint64_t address, std::uint64_t bytes) {
    auto& tracker = state();
    if (!tracker.available || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return true;
    bool dirty = true;
    return !softDirty(tracker.pagemap, address, bytes, dirty) || dirty;
}

void Clear() {
    auto& tracker = state();
    if (!tracker.available) return;
    {
        std::lock_guard lock(tracker.listenerMutex);
        for (const auto& [owner, listener] : tracker.listeners) listener();
    }
    if (!writeClearRefs()) {
        tracker.available = false;
        return;
    }
    tracker.clears.fetch_add(1, std::memory_order_acq_rel);
}

void AddClearListener(const void* owner, std::function<void()> listener) {
    auto& tracker = state();
    std::lock_guard lock(tracker.listenerMutex);
    tracker.listeners[owner] = std::move(listener);
}

void RemoveClearListener(const void* owner) {
    auto& tracker = state();
    std::lock_guard lock(tracker.listenerMutex);
    tracker.listeners.erase(owner);
}

void NoteGpuWrite(std::uint64_t address, std::uint64_t bytes) {
    if (bytes == 0) return;
    auto& tracker = state();
    const auto size = pageSize();
    auto begin = address / size * size;
    auto end = (address + bytes + size - 1) / size * size;
    std::lock_guard lock(tracker.mutex);
    auto& ranges = tracker.gpuRanges;
    auto it = ranges.upper_bound(begin);
    if (it != ranges.begin() && std::prev(it)->second >= begin) --it;
    if (it != ranges.end() && it->first <= begin && it->second >= end) return;
    while (it != ranges.end() && it->first <= end) {
        begin = std::min(begin, it->first);
        end = std::max(end, it->second);
        it = ranges.erase(it);
    }
    ranges.emplace(begin, end);
    tracker.gpuGeneration.fetch_add(1, std::memory_order_acq_rel);
}

bool GpuWritten(std::uint64_t address, std::uint64_t bytes) {
    auto& tracker = state();
    std::lock_guard lock(tracker.mutex);
    return overlaps(tracker.gpuRanges, address, address + bytes);
}

std::uint64_t GpuWriteGeneration() {
    return state().gpuGeneration.load(std::memory_order_acquire);
}

void NoteAliasWrite(std::uint64_t address, std::uint64_t bytes) {
    if (bytes == 0) return;
    auto& tracker = state();
    std::lock_guard lock(tracker.mutex);
    const auto generation = tracker.aliasGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    tracker.aliasWrites.push_back({generation, address, address + bytes});
    if (tracker.aliasWrites.size() > kAliasHistory) {
        tracker.aliasFloor = tracker.aliasWrites.front().generation;
        tracker.aliasWrites.pop_front();
    }
}

std::uint64_t AliasWriteGeneration() {
    return state().aliasGeneration.load(std::memory_order_acquire);
}

bool AliasWrittenSince(std::uint64_t address, std::uint64_t bytes, std::uint64_t generation) {
    auto& tracker = state();
    if (tracker.aliasGeneration.load(std::memory_order_acquire) == generation) return false;
    std::lock_guard lock(tracker.mutex);
    if (generation < tracker.aliasFloor) return true;
    const auto end = address + bytes;
    for (auto it = tracker.aliasWrites.rbegin(); it != tracker.aliasWrites.rend() && it->generation > generation; ++it)
        if (it->begin < end && address < it->end) return true;
    return false;
}

}
