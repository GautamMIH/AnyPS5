#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DEVICETHREAD_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DEVICETHREAD_HPP

#include "prx/libSceAgcDriver/Execution/include/ThreadPlacement.hpp"
#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver {

// The second stage of the pipelined driver (the default; ANYPS5_PIPELINED_DRIVER=0 turns it off): device work (recording
// draws, GPU barriers) runs in order on its own thread, under the tracking mutex, while the worker
// decodes the next packets and captures their shaders. The worker drains it before any other device
// call. Its own guest reads ask Check what they need: nothing (no queued job, dirty surface or
// recent GPU write concerns the range), a resolve against the device's current state, a wait for
// the last queued job that writes the range, or (unknown writes) a full drain.
class DeviceThread {
public:
    using Ranges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

    // runJob runs one job on the device thread (it takes the locks and scopes the job needs) and
    // returns the device's resident ranges afterwards, published for MayAffect.
    explicit DeviceThread(std::function<Ranges(const std::function<void()>&)> runJob) : runJob(std::move(runJob)), thread([this] { run(); }) {}

    ~DeviceThread() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        thread.join();
    }

    DeviceThread(const DeviceThread&) = delete;
    DeviceThread& operator=(const DeviceThread&) = delete;

    // Queues a job. writes: the guest ranges it may write (render targets, writable buffers);
    // anyWrite: it may write elsewhere too (storage images, address-table stores).
    void Post(std::function<void()> job, Ranges writes, bool anyWrite) {
        // Sorted and merged: reads check them by binary search (address-table stores list every
        // writable registered range).
        std::sort(writes.begin(), writes.end());
        Ranges merged;
        for (const auto& range : writes) {
            if (!merged.empty() && range.first <= merged.back().second) merged.back().second = std::max(merged.back().second, range.second);
            else merged.push_back(range);
        }
        writes = std::move(merged);
        {
            std::lock_guard lock(mutex);
            rethrow();
            jobs.push_back({std::move(job), std::move(writes), anyWrite, ++posted});
        }
        wake.notify_all();
    }

    // Waits until the job with this serial (and every earlier one) ran.
    void WaitFor(std::uint64_t serial) {
        std::unique_lock lock(mutex);
        idle.wait(lock, [&] { return completed >= serial || failure != nullptr; });
        rethrow();
    }

    // Waits until every queued job ran; rethrows a job's failure. The caller must not hold the
    // tracking mutex (jobs take it).
    void Drain() {
        std::unique_lock lock(mutex);
        idle.wait(lock, [&] { return (jobs.empty() && !running) || failure != nullptr; });
        rethrow();
    }

    // What a read of [address, address + bytes) by the worker needs before the device's resolve:
    // None: no queued job writes it (or may write anywhere), no dirty surface covers it and the GPU
    // has not written it since it was last known idle, so no queued GPU write, surface or page
    // watch involves it (no resolve at all). Resolve: only state the device already holds does (the
    // resolve runs now, under the tracking mutex). Wait: queued job `serial` is the last that may
    // write it (wait for it, then resolve). Drain: unknown writes are pending.
    struct Need {
        enum Kind { None, Resolve, Wait, Drain } kind = None;
        std::uint64_t serial = 0;
    };
    Need Check(std::uint64_t address, std::size_t bytes) {
        const auto end = address + bytes;
        const auto overlaps = [&](const Ranges& ranges) {
            for (const auto& [first, last] : ranges)
                if (first < end && address < last) return true;
            return false;
        };
        // Job writes are sorted and disjoint.
        const auto overlapsSorted = [&](const Ranges& ranges) {
            auto it = std::upper_bound(ranges.begin(), ranges.end(), address, [](std::uint64_t value, const auto& range) { return value < range.second; });
            return it != ranges.end() && it->first < end;
        };
        std::lock_guard lock(mutex);
        // Debug aid: APS5_TRACE_PIPELINE=1 counts why reads take the slow path.
        static const bool trace = std::getenv("APS5_TRACE_PIPELINE") != nullptr;
        const auto because = [&](int reason) {
            if (trace) {
                static std::array<std::uint64_t, 6> counts{};
                ++counts[reason];
                if (++traceCalls % 200000 == 0) std::fprintf(stderr, "[pipeline] reads %llu: fast %llu unknown %llu queued %llu running %llu resident %llu gpu-written %llu\n", static_cast<unsigned long long>(traceCalls), static_cast<unsigned long long>(counts[0]), static_cast<unsigned long long>(counts[1]), static_cast<unsigned long long>(counts[2]), static_cast<unsigned long long>(counts[3]), static_cast<unsigned long long>(counts[4]), static_cast<unsigned long long>(counts[5]));
            }
            return reason != 0;
        };
        if (unknownGpuWrites && WriteTracker::GpuIdleCount() <= unknownIdleCount) {
            because(1);
            return {Need::Drain};
        }
        std::uint64_t last = 0;
        bool queued = false;
        if (running && (runningJob.anyWrite || overlapsSorted(runningJob.writes))) last = runningJob.serial;
        for (const auto& job : jobs) {
            if (job.anyWrite || overlapsSorted(job.writes)) {
                last = job.serial;
                queued = true;
                static int logged = 0;
                if (trace && logged < 30) {
                    ++logged;
                    std::fprintf(stderr, "[pipeline] wait read 0x%llx+0x%zx anyWrite %d ranges %zu\n", static_cast<unsigned long long>(address), bytes, job.anyWrite, job.writes.size());
                }
            }
        }
        if (last != 0) {
            because(queued ? 2 : 3);
            return {Need::Wait, last};
        }
        if (overlaps(resident)) {
            because(4);
            return {Need::Resolve};
        }
        if (WriteTracker::GpuWrittenSince(address, bytes, WriteTracker::GpuIdleSequence())) {
            because(5);
            return {Need::Resolve};
        }
        because(0);
        return {Need::None};
    }

    // Work that reports its GPU writes only once they landed (address-table stores) was recorded
    // (called by the job, under the tracking mutex): every range may concern the device until the GPU
    // is next idle (VulkanDevice::WaitIdle holds the mutex too, so a later idle point follows it).
    void NoteUnknownGpuWrites() {
        std::lock_guard lock(mutex);
        unknownGpuWrites = true;
        unknownIdleCount = WriteTracker::GpuIdleCount();
    }

private:
    struct Job {
        std::function<void()> run;
        Ranges writes;
        bool anyWrite = false;
        std::uint64_t serial = 0;
    };

    void rethrow() {
        if (failure) std::rethrow_exception(failure);
    }

    void run() {
#ifndef _WIN32
        pthread_setname_np(pthread_self(), "AgcDevice");
        PinToPerformanceCore(1);
#endif
        // Debug aid: APS5_TRACE_DEVICE=1 splits the thread's time every 10 s into idle (no job),
        // jobs' wall time and jobs' CPU time (the rest of a job's wall time it was off the CPU).
        static const bool trace = std::getenv("APS5_TRACE_DEVICE") != nullptr;
        const auto threadCpu = [] {
#ifndef _WIN32
            timespec now{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
            return static_cast<double>(now.tv_sec) * 1e3 + static_cast<double>(now.tv_nsec) / 1e6;
#else
            return 0.0;
#endif
        };
        auto window = std::chrono::steady_clock::now();
        double idleMs = 0, jobWallMs = 0, jobCpuMs = 0;
        std::uint64_t jobCount = 0;
        for (;;) {
            {
                std::unique_lock lock(mutex);
                const auto idleStart = std::chrono::steady_clock::now();
                wake.wait(lock, [&] { return stopping || !jobs.empty(); });
                if (trace) idleMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - idleStart).count();
                if (jobs.empty()) return;
                runningJob = std::move(jobs.front());
                jobs.pop_front();
                running = true;
            }
            Ranges published;
            std::exception_ptr error;
            const auto jobStart = std::chrono::steady_clock::now();
            const auto jobCpuStart = trace ? threadCpu() : 0.0;
            try {
                published = runJob(runningJob.run);
            } catch (...) {
                error = std::current_exception();
            }
            if (trace) {
                const auto now = std::chrono::steady_clock::now();
                jobWallMs += std::chrono::duration<double, std::milli>(now - jobStart).count();
                jobCpuMs += threadCpu() - jobCpuStart;
                ++jobCount;
                if (now - window >= std::chrono::seconds(10)) {
                    std::fprintf(stderr, "[device] %.0f ms window: idle %.0f ms, %llu jobs %.0f ms wall, %.0f ms on CPU\n", std::chrono::duration<double, std::milli>(now - window).count(), idleMs, static_cast<unsigned long long>(jobCount), jobWallMs, jobCpuMs);
                    window = now;
                    idleMs = jobWallMs = jobCpuMs = 0;
                    jobCount = 0;
                }
            }
            {
                std::lock_guard lock(mutex);
                // The job's effects are in the published state (and the write tracker) before it
                // stops counting as running.
                if (!error) resident = std::move(published);
                else if (!failure) {
                    failure = error;
                    jobs.clear();
                }
                completed = runningJob.serial;
                runningJob = {};
                running = false;
            }
            idle.notify_all();
        }
    }

    std::function<Ranges(const std::function<void()>&)> runJob;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Job> jobs;
    Job runningJob;
    bool running = false;
    bool stopping = false;
    std::uint64_t posted = 0;
    std::uint64_t completed = 0;
    std::exception_ptr failure;
    Ranges resident;
    bool unknownGpuWrites = false;
    std::uint64_t unknownIdleCount = 0;
    std::uint64_t traceCalls = 0;
    std::thread thread;
};

}

#endif
