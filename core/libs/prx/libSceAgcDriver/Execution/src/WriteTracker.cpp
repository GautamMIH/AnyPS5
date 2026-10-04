#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unistd.h>

namespace AgcDriver::WriteTracker {
namespace {

// CPU writes are remembered per block of this size.

std::uint64_t pageSize() {
    static const auto size = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    return size;
}

// The last write of every written range: disjoint begin -> {end, stamp} intervals, a newer write
// replacing the parts it covers. "Written after stamp s?" visits only the intervals over the range,
// however many writes there were (logs of writes scanned back to s: 4-11 us per texture validation
// in heavy frames, and a bounded log answered "written" for anything older).
class LastWrites {
public:
    void Note(std::uint64_t begin, std::uint64_t end, std::uint64_t stamp) {
        auto it = writes.lower_bound(begin);
        if (it != writes.begin()) {
            const auto before = std::prev(it);
            const auto previous = before->second;
            if (previous.end > begin) {
                before->second.end = begin;
                if (previous.end > end) writes.emplace(end, previous);
            }
        }
        while (it != writes.end() && it->first < end) {
            const auto covered = it->second;
            it = writes.erase(it);
            if (covered.end > end) {
                writes.emplace(end, covered);
                break;
            }
        }
        writes.emplace(begin, Write{end, stamp});
    }

    bool Since(std::uint64_t begin, std::uint64_t end, std::uint64_t stamp) const {
        auto it = writes.upper_bound(begin);
        if (it != writes.begin() && std::prev(it)->second.end > begin) --it;
        for (; it != writes.end() && it->first < end; ++it)
            if (it->second.stamp > stamp) return true;
        return false;
    }

private:
    struct Write {
        std::uint64_t end;
        std::uint64_t stamp;
    };
    std::map<std::uint64_t, Write> writes;
};

struct State {
    bool available = false;

    std::mutex cpuMutex;
    // Pages written since the watch began, with the generation of the collection that saw the latest
    // write. Generations only grow, so older marks see newer writes.
    LastWrites cpuWrites;
    std::uint64_t cpuGeneration = 1;
    // Ranges collected in the current epoch, keyed by address and size (see NextEpoch).
    std::atomic<std::uint64_t> epoch{1};
    struct RangeHash {
        std::size_t operator()(const std::pair<std::uint64_t, std::uint64_t>& range) const noexcept {
            return static_cast<std::size_t>((range.first * 0x9e3779b97f4a7c15ull) ^ range.second);
        }
    };
    // Epoch and driver-write sequence of each range's last collection.
    struct Collection {
        std::uint64_t epoch;
        std::uint64_t driverSequence;
    };
    std::unordered_map<std::pair<std::uint64_t, std::uint64_t>, Collection, RangeHash> collected;
    // Driver writes (cpuMutex held).
    LastWrites driverWrites;
    std::atomic<std::uint64_t> driverSequence{0};

    std::mutex mutex;
    std::map<std::uint64_t, std::uint64_t> gpuRanges;  // begin -> end, merged
    std::atomic<std::uint64_t> gpuGeneration{0};
    LastWrites gpuWrites;
    std::atomic<std::uint64_t> gpuSequence{0};

    LastWrites aliasWrites;
    std::atomic<std::uint64_t> aliasGeneration{0};

    std::atomic<WriteListener> listener{nullptr};
    std::atomic<void*> listenerContext{nullptr};
};

void notify(State& tracker, std::uint64_t address, std::uint64_t bytes) {
    const auto listener = tracker.listener.load(std::memory_order_acquire);
    if (listener != nullptr && bytes != 0) listener(tracker.listenerContext.load(std::memory_order_acquire), address, bytes);
}

State& state() {
    static State* value = [] {
        auto* result = new State;
        result->available = std::getenv("ANYPS5_NO_WRITE_TRACKING") == nullptr && GuestMemoryBacking::GuestWriteWatchAvailable_nid_postfix();
        return result;
    }();
    return *value;
}

bool validRange(std::uint64_t address, std::uint64_t bytes) {
    return bytes != 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address;
}

// Records the range's writes since its last collection under a new generation (cpuMutex held).
// Returns false when they could not be collected.
bool collect(State& tracker, std::uint64_t address, std::uint64_t bytes) {
    struct Visit {
        State* tracker;
        std::uint64_t generation;
        bool written;
    } visit{&tracker, tracker.cpuGeneration + 1, false};
    const bool collected = GuestMemoryBacking::GuestWriteWatchCollect_nid_postfix(address, bytes, [](void* context, std::uint64_t begin, std::uint64_t end) {
        auto& visit = *static_cast<Visit*>(context);
        visit.written = true;
        notify(*visit.tracker, begin, end - begin);
        visit.tracker->cpuWrites.Note(begin, end, visit.generation);
    }, &visit);
    if (visit.written) tracker.cpuGeneration = visit.generation;
    return collected;
}

// Whether a driver write after `sequence` overlaps the range (cpuMutex held).
bool driverWrittenSince(const State& tracker, std::uint64_t address, std::uint64_t bytes, std::uint64_t sequence) {
    if (tracker.driverSequence.load(std::memory_order_relaxed) == sequence) return false;
    return tracker.driverWrites.Since(address, address + bytes, sequence);
}

// collect, at most once per epoch for a range unless the driver wrote it since (cpuMutex held).
bool collectInEpoch(State& tracker, std::uint64_t address, std::uint64_t bytes) {
    const auto epoch = tracker.epoch.load(std::memory_order_acquire);
    const auto range = std::make_pair(address, bytes);
    const auto sequence = tracker.driverSequence.load(std::memory_order_relaxed);
    if (const auto found = tracker.collected.find(range); found != tracker.collected.end() && found->second.epoch == epoch) {
        if (!driverWrittenSince(tracker, address, bytes, found->second.driverSequence)) {
            found->second.driverSequence = sequence;
            return true;
        }
    }
    if (!collect(tracker, address, bytes)) return false;
    if (tracker.collected.size() >= 65536) tracker.collected.clear();
    tracker.collected[range] = {epoch, sequence};
    return true;
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

std::uint64_t CpuMark(std::uint64_t address, std::uint64_t bytes) {
    auto& tracker = state();
    if (!tracker.available || !validRange(address, bytes)) return 0;
    std::lock_guard lock(tracker.cpuMutex);
    collectInEpoch(tracker, address, bytes);
    return tracker.cpuGeneration;
}

bool CpuWrittenSince(std::uint64_t address, std::uint64_t bytes, std::uint64_t generation) {
    auto& tracker = state();
    if (!tracker.available || !validRange(address, bytes) || generation == 0) return true;
    std::lock_guard lock(tracker.cpuMutex);
    if (!collectInEpoch(tracker, address, bytes)) return true;
    if (tracker.cpuGeneration == generation) return false;
    return tracker.cpuWrites.Since(address, address + bytes, generation);
}

bool CpuCollect(std::uint64_t address, std::uint64_t bytes) {
    auto& tracker = state();
    if (!tracker.available || !validRange(address, bytes)) return false;
    std::lock_guard lock(tracker.cpuMutex);
    return collectInEpoch(tracker, address, bytes);
}

void NextEpoch() {
    state().epoch.fetch_add(1, std::memory_order_acq_rel);
}

std::uint64_t Epoch() {
    return state().epoch.load(std::memory_order_acquire);
}

void NoteDriverWrite(std::uint64_t address, std::uint64_t bytes) {
    auto& tracker = state();
    if (!validRange(address, bytes)) return;
    std::lock_guard lock(tracker.cpuMutex);
    const auto sequence = tracker.driverSequence.load(std::memory_order_relaxed) + 1;
    tracker.driverWrites.Note(address, address + bytes, sequence);
    tracker.driverSequence.store(sequence, std::memory_order_release);
}

std::uint64_t DriverWriteSequence() {
    return state().driverSequence.load(std::memory_order_acquire);
}

bool DriverWrittenSince(std::uint64_t address, std::uint64_t bytes, std::uint64_t sequence) {
    auto& tracker = state();
    if (tracker.driverSequence.load(std::memory_order_acquire) == sequence) return false;
    std::lock_guard lock(tracker.cpuMutex);
    return driverWrittenSince(tracker, address, bytes, sequence);
}

void SetWriteListener(WriteListener listener, void* context) {
    auto& tracker = state();
    tracker.listenerContext.store(context, std::memory_order_release);
    tracker.listener.store(listener, std::memory_order_release);
}

void NoteGpuWrite(std::uint64_t address, std::uint64_t bytes) {
    if (bytes == 0) return;
    auto& tracker = state();
    notify(tracker, address, bytes);
    const auto size = pageSize();
    auto begin = address / size * size;
    auto end = (address + bytes + size - 1) / size * size;
    std::lock_guard lock(tracker.mutex);
    const auto sequence = tracker.gpuSequence.load(std::memory_order_relaxed) + 1;
    tracker.gpuWrites.Note(begin, end, sequence);
    tracker.gpuSequence.store(sequence, std::memory_order_release);
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

std::uint64_t GpuWriteSequence() {
    return state().gpuSequence.load(std::memory_order_acquire);
}

bool GpuWrittenSince(std::uint64_t address, std::uint64_t bytes, std::uint64_t sequence) {
    auto& tracker = state();
    if (tracker.gpuSequence.load(std::memory_order_acquire) == sequence) return false;
    std::lock_guard lock(tracker.mutex);
    return tracker.gpuWrites.Since(address, address + bytes, sequence);
}

namespace {
std::atomic<std::uint64_t> gpuIdleSequence{0};
std::atomic<std::uint64_t> gpuIdleCount{0};
std::atomic<std::uint64_t> deferredGpuWrites{0};
}

void NoteDeferredGpuWrite() {
    deferredGpuWrites.fetch_add(1, std::memory_order_acq_rel);
}

std::uint64_t DeferredGpuWrites() {
    return deferredGpuWrites.load(std::memory_order_acquire);
}

void NoteGpuIdle(std::uint64_t sequence) {
    auto previous = gpuIdleSequence.load(std::memory_order_relaxed);
    while (previous < sequence && !gpuIdleSequence.compare_exchange_weak(previous, sequence, std::memory_order_acq_rel)) {}
    gpuIdleCount.fetch_add(1, std::memory_order_acq_rel);
}

std::uint64_t GpuIdleSequence() {
    return gpuIdleSequence.load(std::memory_order_acquire);
}

std::uint64_t GpuIdleCount() {
    return gpuIdleCount.load(std::memory_order_acquire);
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
    notify(tracker, address, bytes);
    std::lock_guard lock(tracker.mutex);
    const auto generation = tracker.aliasGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    tracker.aliasWrites.Note(address, address + bytes, generation);
}

std::uint64_t AliasWriteGeneration() {
    return state().aliasGeneration.load(std::memory_order_acquire);
}

bool AliasWrittenSince(std::uint64_t address, std::uint64_t bytes, std::uint64_t generation) {
    auto& tracker = state();
    if (tracker.aliasGeneration.load(std::memory_order_acquire) == generation) return false;
    std::lock_guard lock(tracker.mutex);
    return tracker.aliasWrites.Since(address, address + bytes, generation);
}

}
