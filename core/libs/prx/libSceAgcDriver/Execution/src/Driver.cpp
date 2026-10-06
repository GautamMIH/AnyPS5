#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Eq/include/Event.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastClear.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libSceAgcDriver/Execution/include/DeviceThread.hpp"
#include "Optimization/include/Optimization/ShaderStageInputInfo.hpp"
#include "prx/libc/include/GuestMemoryTracking.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <cstdio>
#include <bit>
#include <algorithm>
#include <cstdlib>
#include <array>
#include <atomic>
#include <condition_variable>
#include <shared_mutex>
#include <cstring>
#include <chrono>
#include <deque>
#include <unordered_map>
#include <set>
#include <exception>
#include <limits>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <dlfcn.h>
#include <pthread.h>
#endif

namespace AgcDriver {

// Debug aid: APS5_TRACE_AGC_RELEASE=1 logs submissions, RELEASE_MEM labels and interrupts as they
// are queued and finished, and each interrupt's delivery: a guest waiting forever on a GPU label or
// interrupt shows whether the driver never got the work, never finished it, or finished it unseen.
bool TraceRelease() {
    static const bool enabled = std::getenv("APS5_TRACE_AGC_RELEASE") != nullptr;
    return enabled;
}
namespace {

void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(std::string("AGC driver: ") + reason);
    }
}

struct ShaderSnapshot {
    std::uint64_t codeAddress;
    std::uint64_t headerAddress;
    std::uint8_t type;
    std::vector<std::uint32_t> code;
    std::vector<std::byte> header;
};

struct Submission {
    std::uint64_t serial;
    std::uint32_t queue;
    std::vector<std::uint32_t> commands;
    std::map<std::uint64_t, std::shared_ptr<const ShaderSnapshot>> shaders;
    std::map<std::size_t, std::shared_ptr<IFlipRequest>> flips;
    std::map<std::size_t, std::shared_ptr<IRenderingWait>> renderingWaits;
    bool suspend = false;
    std::size_t cursor = 0;
    bool started = false;
    FrameTiming::Clock::time_point received;
    FrameTiming::Clock::time_point copied;
    FrameTiming::Clock::time_point validated;
    FrameTiming::Clock::time_point enqueued;
    FrameTiming::Clock::time_point dequeued;
};

// Names the calling thread for profilers and /proc (Linux limits names to 15 characters).
void NameThread(const char* name) {
#ifndef _WIN32
    pthread_setname_np(pthread_self(), name);
#else
    static_cast<void>(name);
#endif
}

std::uint32_t readRegister(const Registers& registers, std::uint32_t offset) {
    const auto it = registers.find(offset);
    if (it == registers.end()) {
        char message[96];
        std::snprintf(message, sizeof(message), "AGC driver: required shader register 0x%x has not been written", offset);
        throw std::runtime_error(message);
    }
    return it->second;
}

class Driver {
public:
    static Driver& Get() {
        static Driver driver;
        return driver;
    }

    ~Driver() {
        stop();
    }

    void Shutdown() {
        stop();
        CheckFailure();
    }

private:
    void stop() {
        require(std::this_thread::get_id() != worker.get_id(), "worker cannot stop itself");
        std::lock_guard shutdownLock(shutdownMutex);
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        if (worker.joinable()) worker.join();
        completerStop = true;
        wakeCompleter();
        if (completer.joinable()) completer.join();
    }

public:

    void Submit(const Packet* packet, std::uint32_t queue) {
        const auto received = FrameTiming::Clock::now();
        CheckFailure();
        require(queue == 0 || (queue >= 0x20 && queue < 0x58), "unsupported compute queue");
        GuestMemory::CheckRange(packet, sizeof(Packet), alignof(Packet));
        const auto descriptor = *packet;
        require(descriptor.flags == 0, "nonzero submission flags are not implemented");
        Submission submission{};
        submission.queue = queue;
        submission.received = received;
        if (descriptor.dw_num != 0) {
            require(descriptor.dw_num <= std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t), "command size overflow");
            GuestMemory::CheckRange(descriptor.addr, static_cast<std::size_t>(descriptor.dw_num) * sizeof(std::uint32_t), alignof(std::uint32_t));
            submission.commands.assign(descriptor.addr, descriptor.addr + descriptor.dw_num);
        }
        submission.copied = FrameTiming::Clock::now();
        validate(submission.commands, queue);
        submission.validated = FrameTiming::Clock::now();
        {
            std::lock_guard lock(mutex);
            rethrowFailure();
            require(!stopping, "submission during shutdown");
            require(accepted != std::numeric_limits<std::uint64_t>::max(), "submission serial overflow");
            registerDisplayPackets(submission, 0, submission.commands.size());
            submission.shaders = shaders;
            submission.serial = accepted + 1;
            submission.enqueued = FrameTiming::Clock::now();
            if (TraceRelease()) std::fprintf(stderr, "[agc-release] submit serial=%llu queue=0x%x dwords=%zu\n", static_cast<unsigned long long>(submission.serial), queue, submission.commands.size());
            pending.push_back(std::move(submission));
            ++accepted;
        }
        changed.notify_all();
    }

    void WaitIdle() {
        require(std::this_thread::get_id() != worker.get_id(), "worker cannot wait for itself");
        std::unique_lock lock(mutex);
        const auto target = accepted;
        changed.wait(lock, [&] { return failure != nullptr || completed >= target; });
        rethrowFailure();
    }

    void SuspendPoint() {
        const auto received = FrameTiming::Clock::now();
        require(std::this_thread::get_id() != worker.get_id(), "worker cannot suspend itself");
        std::unique_lock lock(mutex);
        rethrowFailure();
        require(!stopping, "suspend during shutdown");
        require(accepted != std::numeric_limits<std::uint64_t>::max(), "submission serial overflow");
        Submission boundary{};
        boundary.serial = accepted + 1;
        boundary.suspend = true;
        boundary.received = received;
        boundary.copied = received;
        boundary.validated = received;
        boundary.enqueued = FrameTiming::Clock::now();
        pending.push_back(std::move(boundary));
        const auto target = ++accepted;
        changed.notify_all();
        changed.wait(lock, [&] { return failure != nullptr || completed >= target; });
        rethrowFailure();
    }

    void RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
        require(output != nullptr, "null video output");
        std::lock_guard lock(mutex);
        rethrowFailure();
        require(!stopping, "video output registration during shutdown");
        require(outputs.emplace(handle, output).second, "video output already registered");
    }

    void UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
        std::lock_guard lock(mutex);
        const auto it = outputs.find(handle);
        require(it != outputs.end() && it->second == output, "video output registration mismatch");
        outputs.erase(it);
    }

    void CheckFailure() {
        std::lock_guard lock(mutex);
        rethrowFailure();
    }

    void ReportFailure(std::exception_ptr error) {
        require(error != nullptr, "null asynchronous failure");
        {
            std::lock_guard lock(mutex);
            if (!failure) {
                failure = error;
                // The guest sees the failure only at its next submission or flip; one waiting on the
                // failed work's labels never makes one, so the failure is reported here too.
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception& reported) {
                    std::fprintf(stderr, "[AgcDriver] GPU work failed: %s\n", reported.what());
                } catch (...) {
                    std::fprintf(stderr, "[AgcDriver] GPU work failed with an unknown exception\n");
                }
            }
            for (const auto& [handle, output] : outputs) output->Fail(failure);
            for (const auto& item : pending) {
                for (const auto& [offset, flip] : item.flips) flip->Fail(failure);
            }
            pending.clear();
        }
        changed.notify_all();
    }

    void Present(const PresentationWindow& window, const DisplayBuffer* buffer, bool opaque, void (*gpuReady)(void*), void* context) {
        PerformanceContext timingContext(window.timing.get());
        PerformanceTimer timing("Driver.Present");
        CheckFailure();
        require(gpuReady != nullptr && context != nullptr, "missing GPU completion callback");
        require(window.getDrawableSize != nullptr, "missing window drawable size query");
        std::shared_ptr<VulkanDevice> presenting;
        timing.Mark("validate");
        try {
            {
                std::lock_guard lock(gpuMutex);
                timing.Mark("gpu_mutex_wait");
                if (device == nullptr || device->Window() == nullptr) {
                    if (device) {
                        stashCompletions(pollCompletions(true));
                        device->WaitIdle();
                        // Cached textures and pipelines may still reference the old device's handles.
                        static std::vector<std::shared_ptr<VulkanDevice>> replacedDevices;
                        replacedDevices.push_back(device);
                    }
                    device = std::make_shared<VulkanDevice>(&window);
                    captureMemo.Clear();
                }
                require(device->Window() == window.context, "presentation window does not match device surface");
                presenting = device;
                timing.Mark("device_setup");
                std::uint32_t drawableWidth = 0;
                std::uint32_t drawableHeight = 0;
                window.getDrawableSize(window.context, &drawableWidth, &drawableHeight);
                presenting->Resize(drawableWidth, drawableHeight);
                timing.Mark("resize");
                if (buffer != nullptr) presenting->DumpFrame(*buffer);
                if (presenting->Presentable()) {
                    if (buffer != nullptr) {
                        require(buffer->width == window.width && buffer->height == window.height, "display buffer extent differs from output");
                        if (!VulkanDevice::AsyncFlip()) presenting->WaitDraws();
                        timing.Mark("draw_wait");
                        presenting->PresentDisplayBuffer(*buffer);
                        timing.Mark("present_display_buffer");
                    } else {
                        presenting->PresentClear(window.width, window.height, opaque);
                        timing.Mark("present_clear");
                    }
                }
            }
            publishCompletions();
            gpuReady(context);
            timing.Mark("release_and_callback");
            CheckFailure();
        } catch (...) {
            ReportFailure(std::current_exception());
            throw;
        }
    }

    void ReleaseWindow(void* window) {
        {
            const auto lock = lockDevice();
            if (device && device->Window() == window) {
                stashCompletions(pollCompletions(true));
                device.reset();
            }
        }
        publishCompletions();
    }

    void RegisterShader(const Shader* shader) {
        CheckFailure();
        // Shader binaries embedded in game data are only guaranteed 4-byte alignment, so the fixed
        // fields are read through a copy rather than through the guest pointer.
        GuestMemory::CheckRange(shader, sizeof(Shader), alignof(std::uint32_t));
        Shader fixed;
        std::memcpy(&fixed, shader, sizeof(Shader));
        require(fixed.file_header == 0x34333231u && fixed.version == 0x18u, "invalid shader header");
        require(fixed.header_size >= sizeof(Shader), "shader header is smaller than its fixed fields");
        require(fixed.shader_size != 0 && (fixed.shader_size & 3u) == 0, "invalid shader size");
        GuestMemory::CheckRange(shader, fixed.header_size, alignof(std::uint32_t));
        const auto* code = const_cast<const void*>(fixed.code);
        GuestMemory::CheckRange(code, fixed.shader_size, 256);
        ShaderSnapshot snapshot{reinterpret_cast<std::uintptr_t>(code), reinterpret_cast<std::uintptr_t>(shader), fixed.type, {}, {}};
        snapshot.code.resize(fixed.shader_size / sizeof(std::uint32_t));
        std::memcpy(snapshot.code.data(), code, fixed.shader_size);
        snapshot.header.resize(fixed.header_size);
        std::memcpy(snapshot.header.data(), shader, fixed.header_size);
        std::lock_guard lock(mutex);
        rethrowFailure();
        const auto address = snapshot.codeAddress;
        shaders.insert_or_assign(address, std::make_shared<const ShaderSnapshot>(std::move(snapshot)));
    }

private:
    std::mutex mutex;
    std::mutex shutdownMutex;
    std::condition_variable changed;
    std::deque<Submission> pending;
    std::map<std::uint64_t, std::shared_ptr<const ShaderSnapshot>> shaders;
    std::map<std::uint32_t, QueueState> queues;
    std::map<std::uint32_t, std::shared_ptr<IVideoOutput>> outputs;
    std::recursive_mutex& gpuMutex = GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix();
    std::shared_ptr<VulkanDevice> device;
    // Used and cleared under gpuMutex (cleared whenever the device, and so the target, changes).
    CaptureMemo captureMemo;
    std::uint64_t accepted = 0;
    std::uint64_t completed = 0;
    std::set<std::uint64_t> finishedOutOfOrder;
    std::deque<Submission> active;
    std::string blockedWait;
    std::exception_ptr failure;
    bool stopping = false;
    bool resetGraphics = false;
    std::shared_ptr<FrameTiming> frameTiming;
    std::uint64_t frameSerial = 0;
    // The pipelined driver's device thread (ANYPS5_PIPELINED_DRIVER=0 turns it off), or null: device work runs
    // on the worker.
    std::unique_ptr<DeviceThread> pipeline = makePipeline();
    std::thread worker;

    std::unique_ptr<DeviceThread> makePipeline() {
        const char* setting = std::getenv("ANYPS5_PIPELINED_DRIVER");
        static const bool pipelined = setting == nullptr || std::string(setting) != "0";
        if (!pipelined) return nullptr;
        return std::make_unique<DeviceThread>([this](const std::function<void()>& job) {
            std::lock_guard gpuLock(gpuMutex);
            job();
            refreshRegistered();
            return device != nullptr ? device->DirtyRanges() : DeviceThread::Ranges{};
        });
    }

    // Device work outside the device thread: what it queued runs first (the caller must not hold
    // gpuMutex: queued jobs take it).
    std::unique_lock<std::recursive_mutex> lockDevice(std::source_location where = std::source_location::current()) {
        if (pipeline) {
            PerformanceTimer timing("Driver.PipelineDrain");
            const auto started = std::chrono::steady_clock::now();
            pipeline->Drain();
            // Attributed to the calling line (APS5_TRACE_DRAINS).
            noteDrain(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(where.line()) << 4u), started);
            timing.Mark("device_call");
        }
        return std::unique_lock(gpuMutex);
    }

    // Debug aid: APS5_TRACE_DRAINS=1 totals the worker's waits for the device thread per caller
    // (module+offset, for addr2line) and prints them every 2000 frames' worth of calls.
    static void noteDrain(const void* caller, std::chrono::steady_clock::time_point started) {
        static const bool trace = std::getenv("APS5_TRACE_DRAINS") != nullptr;
        if (!trace) return;
        static std::mutex mutex;
        static std::map<const void*, std::pair<std::uint64_t, double>> callers;
        static std::uint64_t calls = 0;
        const auto waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        std::lock_guard lock(mutex);
        auto& total = callers[caller];
        ++total.first;
        total.second += waited;
        if (++calls % 50000 != 0) return;
        for (const auto& [site, value] : callers) {
            Dl_info info{};
            const auto found = dladdr(site, &info) != 0 && info.dli_fname != nullptr;
            std::fprintf(stderr, "[drains] %8llu calls %9.1f ms  +0x%llx\n", static_cast<unsigned long long>(value.first), value.second, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(site) - (found ? reinterpret_cast<std::uintptr_t>(info.dli_fbase) : 0)));
        }
        callers.clear();
    }

    static void resolveForDevice(void* context, std::uint64_t address, std::size_t bytes, bool writable) {
        if (context) static_cast<VulkanDevice*>(context)->ResolveMemory(address, bytes, writable);
    }

    // The pipelined worker's guest accesses (it holds no lock): a read nothing on the device thread
    // can concern needs no resolve; anything else drains the device thread and resolves as device
    // work does, page watches included (the scope resolves tracking).
    static void resolveForWorker(void* context, std::uint64_t address, std::size_t bytes, bool writable) {
        auto& self = *static_cast<Driver*>(context);
        // The worker's own writes wait for everything queued.
        const auto need = writable ? DeviceThread::Need{DeviceThread::Need::Drain} : self.pipeline->Check(address, bytes);
        if (need.kind == DeviceThread::Need::None) {
            // Debug aid: APS5_VERIFY_PIPELINE=1 checks every skipped resolve with the device thread
            // drained: no queued draw may write the range and no dirty resident surface may cover it.
            static const bool verify = std::getenv("APS5_VERIFY_PIPELINE") != nullptr;
            if (verify && self.device) {
                self.pipeline->Drain();
                std::lock_guard gpuLock(self.gpuMutex);
                static std::uint64_t checks = 0;
                static std::uint64_t misses = 0;
                ++checks;
                bool resident = false;
                for (const auto& [first, last] : self.device->DirtyRanges()) resident = resident || (first < address + bytes && address < last);
                if (resident || self.device->LastGpuWriter(address, bytes) != 0) {
                    ++misses;
                    std::fprintf(stderr, "[verify-pipeline] MISSED 0x%llx+0x%zx %s\n", static_cast<unsigned long long>(address), bytes, resident ? "resident surface" : "queued GPU write");
                }
                if (checks % 20000 == 0) std::fprintf(stderr, "[verify-pipeline] %llu skips checked, %llu missed\n", static_cast<unsigned long long>(checks), static_cast<unsigned long long>(misses));
            }
            return;
        }
        if (need.kind == DeviceThread::Need::Drain || need.kind == DeviceThread::Need::Wait) {
            PerformanceTimer timing("Driver.PipelineDrain");
            const auto started = std::chrono::steady_clock::now();
            if (need.kind == DeviceThread::Need::Drain) self.pipeline->Drain();
            else self.pipeline->WaitFor(need.serial);
            // Reads: attributed to two pseudo-callers (1: full drain, 2: wait for a writer).
            noteDrain(reinterpret_cast<const void*>(need.kind == DeviceThread::Need::Drain ? 1 : 2), started);
            timing.Mark(need.kind == DeviceThread::Need::Drain ? "read_drain" : "read_wait");
        }
        // Later queued jobs do not write the range: the device's present state is the one to resolve.
        std::lock_guard gpuLock(self.gpuMutex);
        if (self.device) self.device->ResolveMemory(address, bytes, writable);
        GuestMemoryTracking::GuestMemoryTrackingResolve_nid_postfix(address, bytes, writable);
    }

    // The memory scope for the worker's own guest accesses (shader captures, register packets,
    // indirect arguments): with gpuMutex held the device's resolver, pipelined the worker's.
    void* accessContext() { return pipeline ? static_cast<void*>(this) : static_cast<void*>(device.get()); }
    GuestMemory::MemoryAccessScope::Resolver accessResolver() const { return pipeline ? &Driver::resolveForWorker : &Driver::resolveForDevice; }

    // GPU work completes asynchronously, as on the console: a RELEASE_MEM's label and interrupt, and
    // a submission's completion, wait in order on a GPU marker (under gpuMutex) instead of the
    // worker idling the GPU. The worker drains them where later packets could observe them; the
    // completer thread finishes them while the worker has nothing to run.
    struct Completion {
        std::uint64_t marker = 0;
        std::vector<std::uint32_t> release;
        std::uint64_t serial = 0;
        int eventQueue = 0;
        std::optional<std::uint32_t> interrupt;
        // A flip whose frame is ready on the GPU once the completion is reached (AsyncFlip).
        std::shared_ptr<IFlipRequest> flip;
        std::shared_ptr<FrameTiming> flipTiming;
    };
    struct Completed {
        std::vector<std::pair<int, std::uint32_t>> interrupts;
        std::vector<std::uint64_t> serials;
        std::vector<std::pair<std::shared_ptr<IFlipRequest>, std::shared_ptr<FrameTiming>>> flips;
    };
    std::deque<Completion> completions;
    // The RELEASE_MEM packets the worker deferred, numbered in order, and how many of them have
    // completed (completions finish in order): the worker sees its pending releases without
    // draining the device thread, which may not have queued them yet. Worker only, but for the count.
    std::deque<std::pair<std::uint64_t, std::vector<std::uint32_t>>> deferredReleases;
    std::uint64_t releasesDeferred = 0;
    std::atomic<std::uint64_t> releasesCompleted{0};
    // Whether a deferred release still pending will satisfy the WAIT_REG_MEM. Worker only.
    bool pendingReleaseSatisfies(std::span<const std::uint32_t> packet) {
        const auto completed = releasesCompleted.load(std::memory_order_acquire);
        while (!deferredReleases.empty() && deferredReleases.front().first <= completed) deferredReleases.pop_front();
        return std::any_of(deferredReleases.begin(), deferredReleases.end(), [&](const auto& release) { return Pm4::ReleaseSatisfiesWait(packet, release.second); });
    }
    // Dwords that WRITE_DATA and small fills recorded on the GPU leave in guest memory, with the
    // draw-queue serial of the write: valid while that write is the range's last queued writer.
    struct PredictedDword {
        std::uint32_t value;
        std::uint64_t serial;
    };
    std::unordered_map<std::uint64_t, PredictedDword> predicted;
    void predictWrite(const Pm4::DmaCopy& dma, std::uint64_t serial) {
        if (dma.bytes > 64 || (!dma.immediate && dma.data.empty())) return;
        if (predicted.size() > 4096) predicted.clear();
        for (std::uint64_t offset = 0; offset < dma.bytes; offset += 4)
            predicted[dma.destination + offset] = {dma.data.empty() ? dma.value : dma.data[offset / 4], serial};
    }
    // Whether the queued GPU writes leave a value satisfying the WAIT_REG_MEM.
    bool predictedWaitSatisfied(std::span<const std::uint32_t> packet) {
        const auto wide = ((packet[0] >> 8u) & 0xffu) == 0x93;
        const auto target = packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u);
        const auto low = predicted.find(target);
        if (low == predicted.end()) return false;
        std::uint64_t value = low->second.value;
        if (wide) {
            const auto high = predicted.find(target + 4);
            if (high == predicted.end() || high->second.serial != low->second.serial) return false;
            value |= static_cast<std::uint64_t>(high->second.value) << 32u;
        }
        if (device->LastGpuWriter(target, wide ? 8 : 4) != low->second.serial) {
            predicted.erase(low);
            return false;
        }
        return Pm4::ValueSatisfiesWait(packet, value);
    }
    // Finished under gpuMutex, not yet published (see publishCompletions).
    Completed unpublished;
    std::atomic<bool> unpublishedPending{false};
    std::atomic<bool> completionsPending{false};
    std::atomic<bool> completerStop{false};
    std::mutex completerMutex;
    std::condition_variable completerWake;
    std::thread completer;

    Driver() : worker([this] { run(); }), completer([this] { complete(); }) {
        try {
            LibcRegisterShutdown_nid_postfix([] { Driver::Get().Shutdown(); });
        } catch (...) {
            stop();
            throw;
        }
    }

    void wakeCompleter() {
        { std::lock_guard lock(completerMutex); }
        completerWake.notify_all();
    }

    // Caller holds gpuMutex. The marker orders the completion after all GPU work submitted so far.
    void queueCompletion(Completion completion) {
        if (device != nullptr) completion.marker = device->SubmitMarker();
        completions.push_back(std::move(completion));
        completionsPending = true;
        wakeCompleter();
    }

    // A completion ordered after everything queued so far, without waiting for the device thread:
    // it queues the completion as its next job (jobs hold gpuMutex), behind the draws before it.
    // Every place that waits for all completions drains the device thread first. Caller does not
    // hold gpuMutex. ANYPS5_DRAIN_COMPLETIONS=1 drains instead.
    void deferCompletion(Completion completion, std::source_location where = std::source_location::current()) {
        static const bool drain = std::getenv("ANYPS5_DRAIN_COMPLETIONS") != nullptr;
        if (pipeline && !drain) {
            pipeline->Post([this, completion = std::move(completion)]() mutable { queueCompletion(std::move(completion)); }, {}, false);
        } else {
            const auto gpuLock = lockDevice(where);
            queueCompletion(std::move(completion));
        }
    }

    // Caller holds gpuMutex. Finishes the completions whose GPU work is done (all of them if wait),
    // in order: labels are written here; interrupts and completed serials go to finishCompletions.
    Completed pollCompletions(bool wait) {
        Completed done;
        while (!completions.empty()) {
            auto& next = completions.front();
            if (next.marker != 0 && device != nullptr && !device->MarkerReached(next.marker)) {
                if (!wait) break;
                device->WaitMarker(next.marker);
            }
            if (!next.release.empty()) {
                const GuestMemory::MemoryAccessScope memoryScope(device.get(), [](void* context, std::uint64_t address, std::size_t bytes, bool writable) {
                    if (context) static_cast<VulkanDevice*>(context)->ResolveMemory(address, bytes, writable);
                });
                const GuestMemory::AccessSite accessSite("completion_release");
                // Release packets only write memory: their register state is discarded. The scratch
                // state is reset from a pristine copy (copy-assignment reuses the register maps'
                // nodes; building a fresh state allocated a 350-entry map every release).
                static const QueueState pristine;
                static thread_local QueueState unused;
                unused = pristine;
                Pm4::Execute(next.release, unused);
                if (TraceRelease()) std::fprintf(stderr, "[agc-release] written label=0x%llx marker=%llu\n", static_cast<unsigned long long>(next.release[3] | (static_cast<std::uint64_t>(next.release[4]) << 32u)), static_cast<unsigned long long>(next.marker));
            }
            if (next.flip) done.flips.emplace_back(std::move(next.flip), std::move(next.flipTiming));
            if (next.interrupt) done.interrupts.emplace_back(next.eventQueue, *next.interrupt);
            if (next.serial != 0) done.serials.push_back(next.serial);
            if (!next.release.empty()) releasesCompleted.fetch_add(1, std::memory_order_release);
            completions.pop_front();
        }
        completionsPending = !completions.empty();
        return done;
    }

    // Caller holds gpuMutex: the completions are published later, without it.
    void stashCompletions(Completed done) {
        if (done.interrupts.empty() && done.serials.empty() && done.flips.empty()) return;
        unpublished.flips.insert(unpublished.flips.end(), done.flips.begin(), done.flips.end());
        unpublished.interrupts.insert(unpublished.interrupts.end(), done.interrupts.begin(), done.interrupts.end());
        unpublished.serials.insert(unpublished.serials.end(), done.serials.begin(), done.serials.end());
        unpublishedPending = true;
    }

    // Caller does not hold gpuMutex.
    void publishCompletions() {
        if (!unpublishedPending) return;
        Completed done;
        {
            std::lock_guard gpuLock(gpuMutex);
            std::swap(done, unpublished);
            unpublishedPending = false;
        }
        finishCompletions(done);
    }

    // Caller does not hold gpuMutex: raising an interrupt may wake guest threads that fault on
    // tracked memory, and serials take the queue mutex.
    void finishCompletions(const Completed& done) {
        // A flip's frame is done on the GPU: it may be presented (in completion order).
        for (const auto& [flip, timing] : done.flips) flip->GpuReady(timing);
        for (const auto& [queue, data] : done.interrupts) {
            if (TraceRelease()) std::fprintf(stderr, "[agc-release] interrupt queue=0x%x context=0x%x\n", static_cast<unsigned>(queue), data);
            AgcDriverTriggerEqEvent_nid_postfix(queue, data);
        }
        if (done.serials.empty()) return;
        {
            std::lock_guard lock(mutex);
            for (const auto serial : done.serials) markCompleted(serial);
        }
        changed.notify_all();
    }

    // The completer: finishes completions while nothing else does (the worker may be idle or
    // blocked), polling every 200 us while some are pending.
    void complete() noexcept {
        NameThread("AgcCompleter");
        try {
            while (!completerStop) {
                if (!completionsPending) {
                    std::unique_lock lock(completerMutex);
                    completerWake.wait_for(lock, std::chrono::milliseconds(50), [&] { return completerStop.load() || completionsPending.load(); });
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                {
                    std::lock_guard gpuLock(gpuMutex);
                    stashCompletions(pollCompletions(false));
                }
                publishCompletions();
            }
        } catch (...) {
            ReportFailure(std::current_exception());
        }
    }

    void rethrowFailure() const {
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
    }

    // INDIRECT_BUFFER: the nested command buffer is read when execution reaches it and replaces the
    // packet (call) or the rest of the submission (chain, bit 20).
    // Reserves the flips and captures the rendering waits in commands [first, last); the caller
    // holds mutex. Flips reach the video output's queue in the order they are reserved.
    void registerDisplayPackets(Submission& submission, std::size_t first, std::size_t last) {
        for (std::size_t cursor = first; cursor < last;) {
            const auto* words = submission.commands.data() + cursor;
            if (words[0] == RenderingWaitPacketHeader) {
                const auto output = outputs.find(words[1]);
                require(output != outputs.end(), "rendering wait references an unregistered video output");
                auto wait = output->second->CaptureRenderingWait(words[2]);
                require(wait != nullptr, "video output returned a null rendering wait");
                submission.renderingWaits.emplace(cursor, std::move(wait));
            }
            if (words[0] == FlipPacketHeader) {
                const auto output = outputs.find(words[1]);
                require(output != outputs.end(), "flip references an unregistered video output");
                const FlipInfo info{words[1], std::bit_cast<std::int32_t>(words[2]), words[3], std::bit_cast<std::int64_t>(static_cast<std::uint64_t>(words[4]) | (static_cast<std::uint64_t>(words[5]) << 32u))};
                auto request = output->second->Reserve(info);
                require(request != nullptr, "video output returned a null flip reservation");
                submission.flips.emplace(cursor, std::move(request));
            }
            cursor += static_cast<std::size_t>((words[0] >> 16u) & 0x3fffu) + 2;
        }
    }

    void spliceIndirectBuffer(Submission& submission, std::size_t cursor) {
        const auto* packet = submission.commands.data() + cursor;
        const auto address = (static_cast<std::uint64_t>(packet[2]) << 32u) | (packet[1] & ~3u);
        spliceCommands(submission, cursor, 4, address, packet[3] & 0xfffffu, (packet[3] & (1u << 20u)) != 0);
    }

    // Replaces the packet at cursor with the command buffer it calls (or, chained, with that buffer
    // and nothing after it).
    void spliceCommands(Submission& submission, std::size_t cursor, std::size_t packetDwords, std::uint64_t address, std::size_t dwords, bool chain) {
        constexpr std::size_t maximumDwords = 64u * 1024u * 1024u;
        std::vector<std::uint32_t> nested;
        if (dwords != 0) {
            require(address != 0, "nested command buffer has a null address");
            const auto* source = reinterpret_cast<const std::uint32_t*>(address);
            GuestMemory::CheckRange(source, dwords * sizeof(std::uint32_t), alignof(std::uint32_t));
            nested.assign(source, source + dwords);
            validate(nested, submission.queue);
        }
        require(submission.commands.size() - packetDwords + nested.size() <= maximumDwords, "nested command buffers exceed the submission limit");
        const auto after = cursor + packetDwords;
        const auto shift = [&](auto& byOffset) {
            std::remove_reference_t<decltype(byOffset)> moved;
            for (auto& [offset, value] : byOffset) {
                if (offset < after) moved.emplace(offset, std::move(value));
                else {
                    require(!chain, "packets after a chained command buffer are not executed");
                    moved.emplace(offset - packetDwords + nested.size(), std::move(value));
                }
            }
            byOffset = std::move(moved);
        };
        shift(submission.flips);
        shift(submission.renderingWaits);
        auto& commands = submission.commands;
        const auto tail = chain ? commands.end() : commands.begin() + static_cast<std::ptrdiff_t>(after);
        commands.erase(commands.begin() + static_cast<std::ptrdiff_t>(cursor), tail);
        commands.insert(commands.begin() + static_cast<std::ptrdiff_t>(cursor), nested.begin(), nested.end());
        // Flips in nested buffers are known only now, so they are reserved when reached.
        std::lock_guard lock(mutex);
        registerDisplayPackets(submission, cursor, cursor + nested.size());
    }

    static void validate(std::span<const std::uint32_t> commands, std::uint32_t queue) {
        for (std::size_t cursor = 0; cursor < commands.size();) {
            const auto header = commands[cursor];
            require((header & 0xc0000000u) == 0xc0000000u, "unsupported PM4 packet type");
            const auto count = static_cast<std::size_t>((header >> 16u) & 0x3fffu) + 2;
            require(count <= commands.size() - cursor, "truncated PM4 packet");
            try {
                Pm4::Validate(commands.subspan(cursor, count), queue);
                // A COND_EXEC range is skipped as a whole: it must end on a packet boundary of its own
                // command buffer and hold no flip (a skipped flip would leave its reservation pending).
                if (((header >> 8u) & 0xffu) == 0x22u) {
                    const auto end = cursor + count + Pm4::ConditionalWords(commands.subspan(cursor, count));
                    require(end <= commands.size(), "conditional execution range exceeds its command buffer");
                    std::size_t inner = cursor + count;
                    while (inner < end) {
                        require(commands[inner] != FlipPacketHeader, "a flip inside a conditional execution range is not implemented");
                        inner += static_cast<std::size_t>((commands[inner] >> 16u) & 0x3fffu) + 2;
                    }
                    require(inner == end, "conditional execution range ends inside a packet");
                }
            } catch (const std::exception& error) {
                throw std::runtime_error("AGC driver: " + Pm4::Name(header) + " at DWORD " + std::to_string(cursor) + ": " + error.what());
            }
            cursor += count;
        }
    }

    // EVENT_WRITE PIXEL_PIPE_STAT_DUMP (upstream 19521c2): the DB sample counters in the PS5 layout,
    // a begin and an end counter per DB (16 of them, 16 bytes apart; a query's begin dump writes the
    // first of each pair, its end dump the second). The count goes to the first DB, the others stay
    // zero, all with bit 63 marking the result ready. Counting starts at the first dump; the device
    // waits for the queued draws first. Kyty and SharpEmu write a fixed "visible" result instead.
    static void dumpSampleCounters(std::span<const std::uint32_t> packet, VulkanDevice* device) {
        const auto address = packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u);
        const std::uint64_t samples = device != nullptr ? device->CountSamples() : 0u;
        constexpr std::uint64_t ready = 1ull << 63u;
        for (std::uint64_t db = 0; db < 16; ++db) {
            const std::uint64_t value = ready | (db == 0 ? samples : 0u);
            GuestMemory::Write(address + db * 16u, std::as_bytes(std::span(&value, 1)), 8);
        }
    }

    void dispatch(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission) {
        PerformanceTimer timing("Driver.Dispatch");
        const auto gpuLock = lockDevice();
        timing.Mark("gpu_mutex_wait");
        const auto address = (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20c)) << 8u) | (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20d) & 0xffu) << 40u);
        auto it = submission.shaders.upper_bound(address);
        require(it != submission.shaders.begin(), "compute program does not belong to a registered shader");
        --it;
        const auto& snapshot = *it->second;
        require(address - snapshot.codeAddress < snapshot.code.size() * sizeof(std::uint32_t), "compute program is outside registered shader code");
        require(snapshot.type == 0, "compute program refers to a non-compute shader");
        const auto userCount = (readRegister(queue.shader, 0x213) >> 1u) & 0x1fu;
        std::vector<std::uint32_t> userData;
        for (std::uint32_t i = 0; i < userCount; ++i) {
            userData.push_back(readRegister(queue.shader, 0x240 + i));
        }
        auto compute = Graphics::DecodeComputeStageInfo(queue.shader);
        // USE_THREAD_DIMENSIONS (direct dispatches only, Pm4::Validate): the packet counts threads.
        // The host launches whole groups; a size that is no whole number of groups compiles the
        // partial-group variant, which retires the threads past it.
        std::array<std::uint32_t, 3> groups{packet[1], packet[2], packet[3]};
        if ((packet[4] & 0x20u) != 0) {
            const std::array<std::uint32_t, 3> threads{packet[1], packet[2], packet[3]};
            for (std::uint32_t axis = 0; axis < 3; ++axis) {
                const auto size = std::max(compute.numThreads[axis], 1u);
                if (threads[axis] % size != 0) compute.partialThreads = threads;
                groups[axis] = (threads[axis] + size - 1) / size;
            }
        }
        const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{{snapshot.codeAddress, std::as_bytes(std::span(snapshot.code))}, {snapshot.headerAddress, snapshot.header}}};
        if (device == nullptr) {
            device = std::make_shared<VulkanDevice>();
            captureMemo.Clear();
        }
        const GuestMemory::MemoryAccessScope memoryScope(device.get(), [](void* context, std::uint64_t address, std::size_t bytes, bool writable) {
            static_cast<VulkanDevice*>(context)->ResolveMemory(address, bytes, writable);
        });
        const GuestMemory::AccessSite accessSite("dispatch_capture");
        const auto codeOffset = static_cast<std::size_t>((address - snapshot.codeAddress) / sizeof(std::uint32_t));
        ShaderRecompiler::RecompileRequest request{
            {ShaderRecompiler::ShaderStage::Compute, address, std::span(snapshot.code).subspan(codeOffset), snapshot.headerAddress, snapshot.header},
            {(packet[4] & 0x8000u) != 0 ? 32u : 64u, 0, userData, compute, std::nullopt, std::nullopt, memory},
            device->Target(),
            {0, 0, 0, 128}
        };
        ShaderMemory shaderMemory(memory);
        timing.Mark("prepare");
        const auto capture = shaderMemory.Capture(request);
        timing.Mark("shader_memory_capture");
        const auto captured = shaderMemory.Regions();
        request.context.memory = captured;
        timing.Mark("request_memory");
        const auto compiled = [&] {
            try {
                return ShaderRecompiler::Recompile(request, *capture);
            } catch (const std::exception& error) {
                char context[160]{};
                std::snprintf(context, sizeof(context), "compute shader 0x%llx (header 0x%llx, PGM_RSRC2 0x%08x, %u user SGPRs): ",
                    static_cast<unsigned long long>(address), static_cast<unsigned long long>(snapshot.headerAddress), readRegister(queue.shader, 0x213), userCount);
                throw std::runtime_error(std::string(context) + error.what());
            }
        }();
        timing.Mark(compiled.cacheHit ? "shader_cache_hit" : "shader_compile");
        std::vector<Graphics::GuestMemorySnapshot> snapshots;
        for (const auto& region : captured) snapshots.push_back({region.guestAddress, region.bytes});
        timing.Mark("snapshots");
        device->Dispatch(compiled, groups[0], groups[1], groups[2], snapshots);
        timing.Mark("dispatch_and_resource_release");
        // A shader that stores to a buffer starting at a registered CMASK without XOR address
        // math is taken to clear it (shadPS4). The dispatch itself still runs.
        if (!compiled.usesBitwiseXor) {
            for (const auto& binding : compiled.bindings) {
                if (binding.kind != ShaderRecompiler::DescriptorKind::StorageBuffer || binding.readOnly) continue;
                for (std::uint32_t element = 0; element < binding.count && (element + 1u) * 4u <= binding.guestDescriptor.size(); ++element) {
                    if (element < binding.bufferWritten.size() && !binding.bufferWritten[element]) continue;
                    const auto* words = binding.guestDescriptor.data() + element * 4u;
                    Graphics::NoteMetadataClear(words[0] | (static_cast<std::uint64_t>(words[1] & 0xffffu) << 32u));
                }
            }
        }
    }

    void draw(QueueState& queue, Pm4::DrawParameters drawParameters, const Submission& submission) {
        PerformanceTimer timing("Driver.Draw");
        // Empty draws (common with GPU-generated indirect arguments) do nothing on hardware.
        if (drawParameters.indexCount == 0 || drawParameters.instanceCount == 0) return;
        const auto graphics = Graphics::DecodeState(queue);
        if (drawParameters.indexed && queue.userConfig.at(0x24b) != 0) {
            // Vulkan restarts on the all-ones index of the index type, for strips and fans always and
            // for lists with VK_EXT_primitive_topology_list_restart. The mesh path fetches indices in
            // the shader and does not restart.
            const bool list = graphics.topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST || graphics.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || graphics.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            if (!graphics.primitiveRestart || graphics.stages.mesh || (list && (device == nullptr || !device->PrimitiveListRestart())))
                throw std::runtime_error("AGC graphics: primitive restart is only supported for vertex-path strips and fans, and for lists with VK_EXT_primitive_topology_list_restart");
            const std::uint32_t allOnes = drawParameters.indexSize == 2 ? 0xffffu : 0xffffffffu;
            const auto resetIndex = queue.context.find(0x103);
            if (resetIndex == queue.context.end() || (resetIndex->second & allOnes) != allOnes)
                throw std::runtime_error("AGC graphics: primitive restart index other than all ones (VGT_MULTI_PRIM_IB_RESET_INDX) is unsupported");
        }
        if (graphics.eliminateFastClear) {
            const auto gpuLock = lockDevice();
            device->ResolveFastClears(graphics);
            return;
        }
        struct Program {
            ShaderRecompiler::ShaderBinary binary;
            std::uint32_t userDataBase;
            std::uint32_t firstUserSgpr = 8;
            std::vector<std::uint32_t> userData;
            std::array<ShaderRecompiler::MemoryRegion, 2> memory;
            // System SGPRs merged stages receive ahead of user data (see initializeMerged).
            std::uint32_t systemSgprs = 0;
            // The registered shader the code belongs to (identifies it for the capture memo).
            std::shared_ptr<const ShaderSnapshot> owner;
        };
        const auto programAddress = [&](std::uint32_t base) {
            const auto high = readRegister(queue.shader, base + 1);
            require((high & ~0xffu) == 0, "reserved graphics program address bits are set");
            return (static_cast<std::uint64_t>(readRegister(queue.shader, base)) << 8u) | (static_cast<std::uint64_t>(high) << 40u);
        };
        const auto prepare = [&](std::uint64_t address, std::uint8_t type, ShaderRecompiler::ShaderStage stage, std::uint32_t rsrc2, std::uint32_t userDataBase) {
            auto it = submission.shaders.upper_bound(address);
            require(it != submission.shaders.begin(), "graphics program does not belong to a registered shader");
            --it;
            const auto& snapshot = *it->second;
            require(address - snapshot.codeAddress < snapshot.code.size() * sizeof(std::uint32_t), "graphics program is outside registered shader code");
            require(snapshot.type == type, "graphics program refers to an incompatible shader binary type");
            const auto resources = readRegister(queue.shader, rsrc2);
            const auto userCount = ((resources >> 1u) & 0x1fu) | (((resources >> 27u) & 1u) << 5u);
            require(userCount <= 32, "graphics user SGPR count exceeds the register bank");
            const auto codeOffset = static_cast<std::size_t>((address - snapshot.codeAddress) / sizeof(std::uint32_t));
            Program result{
                {stage, address, std::span(snapshot.code).subspan(codeOffset), snapshot.headerAddress, snapshot.header},
                userDataBase,
                8,
                {},
                {{{snapshot.codeAddress, std::as_bytes(std::span(snapshot.code))}, {snapshot.headerAddress, snapshot.header}}}
            };
            for (std::uint32_t i = 0; i < userCount; ++i) result.userData.push_back(readRegister(queue.shader, userDataBase + i));
            result.owner = it->second;
            return result;
        };
        using Stage = ShaderRecompiler::ShaderStage;
        using Role = ShaderRecompiler::ProgramRole;
        std::vector<Program> programs;
        std::vector<Role> roles;
        programs.reserve(5);
        roles.reserve(5);
        const auto append = [&](std::uint32_t base, std::uint8_t type, Stage stage, std::uint32_t resources, std::uint32_t users, Role role) {
            programs.push_back(prepare(programAddress(base), type, stage, resources, users));
            roles.push_back(role);
        };
        const auto initializeMerged = [&](Program& program, std::uint32_t pointerBase, bool pointerRequired) {
            program.firstUserSgpr = 0;
            program.systemSgprs = 8;
            program.userData.insert(program.userData.begin(), 8, 0);
            if (pointerRequired) {
                const auto low = readRegister(queue.shader, pointerBase);
                const auto high = readRegister(queue.shader, pointerBase + 1);
                const auto address = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32u);
                require(address != 0, "merged shader user-data address is null");
                GuestMemory::CheckRange(reinterpret_cast<const void*>(address), 8, 4);
                program.userData[0] = low;
                program.userData[1] = high;
            }
        };
        if (graphics.stages.path == Graphics::ShaderPath::Tessellation) {
            append(0x148, 5, Stage::Local, 0x10b, 0x10c, Role::Local);
            append(0x108, 7, Stage::TessellationControl, 0x10b, 0x10c, Role::Hull);
            initializeMerged(programs.back(), 0x102, true);
            append(0x0c8, 2, Stage::TessellationEvaluation, 0x08b, 0x08c, Role::Domain);
        } else if (graphics.stages.path == Graphics::ShaderPath::Geometry) {
            const auto frontAddress = programAddress(0xc8);
            auto snapshot = submission.shaders.upper_bound(frontAddress);
            require(snapshot != submission.shaders.begin(), "geometry front program is not registered");
            --snapshot;
            const auto type = snapshot->second->type;
            require(type == 2 || type == 4, "invalid geometry front binary type");
            append(0xc8, type, Stage::Mesh, 0x8b, 0x8c, Role::Main);
            // Separately bound halves pass the front half's user-data table in s0:s1
            // (SPI_SHADER_USER_DATA_ADDR_LO/HI_GS). Halves fused with sceAgcUnknownFuseShaderHalves
            // share the fused shader's user data and leave the address unset.
            initializeMerged(programs.back(), 0x82, type == 4 && queue.shader.contains(0x82));
            if (type == 4) append(0x88, 6, Stage::Mesh, 0x8b, 0x8c, Role::GeometryBack);
            // The translated mesh program fetches indices through a V# in hidden user words 4-7.
            auto& words = programs.front().userData;
            require(programs.front().firstUserSgpr == 0 && words.size() >= ShaderRecompiler::MeshIndexBufferUserWord + 4, "mesh program lacks the hidden user words");
            const auto descriptor = Graphics::MeshIndexBufferDescriptor(drawParameters, programs.front().binary.codeAddress);
            std::copy(descriptor.begin(), descriptor.end(), words.begin() + ShaderRecompiler::MeshIndexBufferUserWord);
        } else {
            append(0xc8, 2, Stage::Vertex, 0x8b, 0x8c, Role::Main);
        }
        std::optional<ShaderRecompiler::ShaderPixelStageInfo> pixel;
        if (graphics.hasFragmentShader) {
            append(0x008, 1, Stage::Fragment, 0x00b, 0x00c, Role::Fragment);
            programs.back().firstUserSgpr = 0;
            std::array<std::uint8_t, 8> mappings{};
            for (std::uint32_t slot = 0; slot < mappings.size(); ++slot) mappings[slot] = (graphics.colorTargetMask & (1u << slot)) != 0 ? graphics.colors[slot].componentMapping : 0xe4u;
            pixel = Graphics::DecodePixelStageInfo(queue.context, mappings);
        }
        std::vector<ShaderRecompiler::MemoryRegion> memory;
        std::vector<ShaderRecompiler::LinkedProgram> linked;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& program = programs[i];
            memory.insert(memory.end(), program.memory.begin(), program.memory.end());
            linked.push_back({roles[i], program.binary, program.userDataBase, program.firstUserSgpr, program.userData});
        }
        timing.Mark("prepare");
        // Pipelined, the capture runs unlocked (resolveForWorker) and the device half is queued.
        std::unique_lock<std::recursive_mutex> gpuLock(gpuMutex, std::defer_lock);
        if (!pipeline) gpuLock.lock();
        timing.Mark("gpu_mutex_wait");
        if (device == nullptr) {
            const auto creation = lockDevice();
            device = std::make_shared<VulkanDevice>();
            captureMemo.Clear();
        }
        timing.Mark("device_setup");
        const GuestMemory::MemoryAccessScope memoryScope(accessContext(), accessResolver(), pipeline != nullptr);
        const GuestMemory::AccessSite accessSite("draw_capture");
        ShaderMemory shaderMemory(memory);
        std::vector<ShaderRecompiler::RecompileResult> results;
        std::vector<Graphics::CompiledShader> stages;
        results.reserve(programs.size() + (graphics.rectList ? 2u : 0u));
        stages.reserve(programs.size());
        // Mesh stages read the draw's parameters from the end of the push block (MeshDrawPushOffsetBytes,
        // pushed by Graphics::Draw); stage push data stays below them.
        std::uint32_t pushCursorBytes = 0;
        const std::uint32_t pushLimitBytes = graphics.stages.mesh.has_value() ? ShaderRecompiler::MeshDrawPushOffsetBytes : Graphics::PipelinePushConstantBytes;
        const auto vertexStageInfo = [&](const auto& program) {
            auto info = Graphics::DecodeVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData, program.systemSgprs);
            info.paClVsOutCntl = readRegister(queue.context, 0x207);
            return info;
        };
        // A geometry front half jumps into its back half; the two compile as one spliced program.
        std::vector<std::uint32_t> splicedGeometry;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (roles[i] == Role::GeometryBack) {
                require(i > 0 && roles[i - 1] == Role::Main, "geometry back half without its front half");
                splicedGeometry = ShaderRecompiler::SpliceGeometryHalves(programs[i - 1].binary.code, programs[i].binary.code);
            }
        }
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (roles[i] == Role::GeometryBack) continue;
            const auto& program = programs[i];
            const auto waveSize = program.binary.stage == Stage::Fragment ? graphics.stages.fragmentWaveSize : graphics.stages.vertexWaveSize;
            auto binary = program.binary;
            if (roles[i] == Role::Main && !splicedGeometry.empty()) binary.code = splicedGeometry;
            ShaderRecompiler::RecompileRequest request{
                binary,
                {waveSize, program.firstUserSgpr, program.userData, std::nullopt, program.binary.stage == Stage::Fragment ? pixel : std::nullopt, program.binary.stage == Stage::Fragment ? std::nullopt : std::optional(vertexStageInfo(program)), memory},
                device->Target(),
                {0, 0, pushCursorBytes, pushLimitBytes - pushCursorBytes},
                ShaderRecompiler::GraphicsCompileContext{program.firstUserSgpr, linked, graphics.stages.mesh, graphics.stages.tessellation, {drawParameters.indexAddress, drawParameters.indexCount, drawParameters.indexSize, drawParameters.instanceCount}}
            };
            PerformanceTimer shaderTiming("Driver.GraphicsShader");
            // Spliced geometry code belongs to no single registered shader.
            const bool spliced = roles[i] == Role::Main && !splicedGeometry.empty();
            const auto capture = spliced ? shaderMemory.Capture(request) : captureMemo.Capture(program.owner, request, shaderMemory);
            shaderTiming.Mark("memory_capture");
            memory = shaderMemory.Regions();
            request.context.memory = memory;
            shaderTiming.Mark("request_memory");
            results.push_back(ShaderRecompiler::Recompile(request, *capture));
            shaderTiming.Mark(results.back().cacheHit ? "cache_hit" : "compile");
            const auto& result = results.back();
            // Mesh draws read indices in-shader and take no indexed offsets.
            if (i == 0 && !(drawParameters.indexed && graphics.stages.mesh)) {
                const auto offsetValue = [&](std::int32_t sgpr) {
                    require(sgpr >= 0 && static_cast<std::uint32_t>(sgpr) >= program.firstUserSgpr, "invalid draw offset SGPR");
                    const auto index = static_cast<std::uint32_t>(sgpr) - program.firstUserSgpr;
                    require(index < program.userData.size(), "draw offset SGPR exceeds user data");
                    return program.userData[index];
                };
                if (drawParameters.firstVertex == 0 && result.vertexOffsetSgpr >= 0) drawParameters.firstVertex = offsetValue(result.vertexOffsetSgpr);
                if (result.instanceOffsetSgpr >= 0) drawParameters.firstInstance = offsetValue(result.instanceOffsetSgpr);
            }
            require(result.pushConstants.size() <= pushLimitBytes - pushCursorBytes, "stage push constants exceed the pipeline push constant block");
            stages.push_back({program.binary.stage, &result, result.pushConstants.empty() ? 0u : pushCursorBytes});
            pushCursorBytes += static_cast<std::uint32_t>(result.pushConstants.size());
        }
        timing.Mark("shaders");
        if (graphics.rectList) {
            require(stages.size() == (graphics.hasFragmentShader ? 2u : 1u), "rect-list requires a vertex program and at most one fragment program");
            // Without a pixel shader no parameters are interpolated.
            const ShaderRecompiler::RecompileResult noFragment{};
            auto rectangle = ShaderRecompiler::BuildRectListShaders(results[0], graphics.hasFragmentShader ? results[1] : noFragment, device->Target());
            results.push_back(std::move(rectangle.control));
            results.push_back(std::move(rectangle.evaluation));
            const auto control = results.size() - 2;
            stages.insert(stages.begin() + 1, {{Stage::TessellationControl, &results[control], 0}, {Stage::TessellationEvaluation, &results[control + 1], 0}});
        }
        std::vector<Graphics::GuestMemorySnapshot> snapshots;
        for (const auto& region : memory) snapshots.push_back({region.guestAddress, region.bytes});
        timing.Mark("post_compile_prepare");
        // Debug aid: ANYPS5_SYNC_DRAWS=1 completes each draw before the next packet, so a GPU
        // fault surfaces at the draw that caused it.
        static const bool syncDraws = std::getenv("ANYPS5_SYNC_DRAWS") != nullptr;
        if (syncDraws) {
            const auto syncLock = lockDevice();
            device->Draw(graphics, drawParameters, stages, snapshots);
            std::fprintf(stderr, "[pm4] draw completed\n");
        } else if (pipeline) {
            queueDraw(graphics, drawParameters, std::move(results), std::move(stages), std::move(shaderMemory), std::move(memory), std::move(snapshots));
        } else {
            device->EnqueueDraw(graphics, drawParameters, stages, snapshots);
        }
        timing.Mark("draw_and_resource_release");
    }

    // Queues a draw's device half (pipelined). The job owns what it reads: the compiled stages (the
    // stage list points into the results) and the captured shader memory (the snapshots view it).
    void queueDraw(const Graphics::State& graphics, const Pm4::DrawParameters& parameters, std::vector<ShaderRecompiler::RecompileResult> results, std::vector<Graphics::CompiledShader> stages, ShaderMemory shaderMemory, std::vector<ShaderRecompiler::MemoryRegion> memory, std::vector<Graphics::GuestMemorySnapshot> snapshots) {
        DeviceThread::Ranges writes;
        for (std::uint32_t slot = 0; slot < graphics.colors.size(); ++slot) {
            if ((graphics.colorTargetMask & (1u << slot)) == 0) continue;
            const auto& color = graphics.colors[slot];
            writes.emplace_back(color.address, color.address + color.bytes);
        }
        if (graphics.hasDepthTarget) {
            if (graphics.depth.depthBytes != 0) writes.emplace_back(graphics.depth.depthAddress, graphics.depth.depthAddress + graphics.depth.depthBytes);
            if (graphics.depth.stencilBytes != 0) writes.emplace_back(graphics.depth.stencilAddress, graphics.depth.stencilAddress + graphics.depth.stencilBytes);
        }
        // Shader stores (storage buffers and images, address-table stores) may reach anywhere.
        bool anyWrite = false;
        bool addressStores = false;
        for (const auto& result : results) {
            addressStores = addressStores || result.bdaWrites;
            for (const auto& binding : result.bindings) {
                if (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) anyWrite = true;
                if (binding.kind != ShaderRecompiler::DescriptorKind::StorageBuffer || binding.readOnly) continue;
                // Only guest buffers are guest memory: the address table and fault buffer are the
                // driver's, and shader data and flattened SRTs are copies of their words (their
                // "descriptors" are contents, not V#s) that are never written back.
                if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
                // Writable buffers write within their descriptors' ranges (as ShaderResources binds them).
                for (std::size_t element = 0; element < binding.count; ++element) {
                    if ((element + 1u) * 4u > binding.guestDescriptor.size()) {
                        anyWrite = true;
                        break;
                    }
                    // Elements the recompiler proved read-only are bound read-only.
                    if (element < binding.bufferWritten.size() && !binding.bufferWritten[element]) continue;
                    const auto* words = binding.guestDescriptor.data() + element * 4u;
                    const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1], words[2], words[3]}};
                    const auto base = descriptor.Base48();
                    const auto size = descriptor.GetSize();
                    if (base != 0 && size != 0) writes.emplace_back(base, base + size);
                }
            }
            for (const auto& binding : result.bindings) {
                if (binding.kind == ShaderRecompiler::DescriptorKind::StorageTexelBuffer && !binding.readOnly) anyWrite = true;
            }
        }
        // Address-table stores reach writable registered memory only.
        if (addressStores && !appendWritableRegistered(writes)) anyWrite = true;
        // Debug aid: APS5_TRACE_ANYWRITE=1 counts why draws may write anywhere.
        static const bool traceAny = std::getenv("APS5_TRACE_ANYWRITE") != nullptr;
        if (traceAny) {
            static std::uint64_t draws = 0, images = 0, texels = 0, stores = 0;
            bool image = false, texel = false;
            for (const auto& result : results)
                for (const auto& binding : result.bindings) {
                    image = image || binding.kind == ShaderRecompiler::DescriptorKind::StorageImage;
                    texel = texel || (binding.kind == ShaderRecompiler::DescriptorKind::StorageTexelBuffer && !binding.readOnly);
                }
            ++draws;
            images += image;
            texels += texel;
            stores += addressStores;
            if (draws % 100000 == 0) std::fprintf(stderr, "[anywrite] draws %llu: storage images %llu, writable texel buffers %llu, address-table stores %llu\n", static_cast<unsigned long long>(draws), static_cast<unsigned long long>(images), static_cast<unsigned long long>(texels), static_cast<unsigned long long>(stores));
        }
        struct Job {
            std::shared_ptr<VulkanDevice> device;
            Graphics::State graphics;
            Pm4::DrawParameters parameters;
            std::vector<ShaderRecompiler::RecompileResult> results;
            std::vector<Graphics::CompiledShader> stages;
            ShaderMemory shaderMemory;
            std::vector<ShaderRecompiler::MemoryRegion> memory;
            std::vector<Graphics::GuestMemorySnapshot> snapshots;
            std::shared_ptr<FrameTiming> timing;
        };
        auto job = std::make_shared<Job>(Job{device, graphics, parameters, std::move(results), std::move(stages), std::move(shaderMemory), std::move(memory), std::move(snapshots), frameTiming});
        pipeline->Post([this, job] {
            PerformanceContext timingContext(job->timing.get());
            const auto deferred = WriteTracker::DeferredGpuWrites();
            job->device->EnqueueDraw(job->graphics, job->parameters, job->stages, job->snapshots);
            // Writes noted at recording are in the write tracker; ones that land at completion are not.
            if (WriteTracker::DeferredGpuWrites() != deferred) pipeline->NoteUnknownGpuWrites();
        }, std::move(writes), anyWrite);
    }

    // The writable registered guest ranges (what address-table stores can reach). The registry is
    // read under the tracking mutex only (some of its changes rely on that mutex), so the device
    // thread snapshots it after a job when the registry or the guest mappings changed; the worker
    // uses the snapshot while neither generation moved since (false: no current snapshot).
    struct RegisteredSnapshot {
        DeviceThread::Ranges writable;
        std::uint64_t allocations = ~0ull;
        std::uint64_t mappings = ~0ull;
    };
    std::mutex registeredMutex;
    RegisteredSnapshot registered;

    // Device thread, tracking mutex held.
    void refreshRegistered() {
        const auto allocations = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        const auto mappings = GuestMemoryBacking::GuestMemoryBackingGeneration_nid_postfix();
        {
            std::lock_guard lock(registeredMutex);
            if (registered.allocations == allocations && registered.mappings == mappings) return;
        }
        RegisteredSnapshot snapshot{{}, allocations, mappings};
        for (const auto& range : GuestAllocations::GuestAllocationsAcquire_nid_postfix()) {
            if (range->writable) snapshot.writable.emplace_back(range->address, range->address + range->bytes);
        }
        std::lock_guard lock(registeredMutex);
        registered = std::move(snapshot);
    }

    bool appendWritableRegistered(DeviceThread::Ranges& writes) {
        const auto allocations = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        const auto mappings = GuestMemoryBacking::GuestMemoryBackingGeneration_nid_postfix();
        std::lock_guard lock(registeredMutex);
        if (registered.allocations != allocations || registered.mappings != mappings) return false;
        writes.insert(writes.end(), registered.writable.begin(), registered.writable.end());
        return true;
    }

    void includeSubmission(const Submission& submission, bool firstSegment) {
        if (frameTiming == nullptr) {
            require(frameSerial != std::numeric_limits<std::uint64_t>::max(), "frame serial overflow");
            frameTiming = std::make_shared<FrameTiming>(++frameSerial);
        }
        const auto dequeued = firstSegment ? submission.dequeued : FrameTiming::Clock::now();
        frameTiming->IncludeSubmission(submission.serial, submission.received, submission.enqueued, dequeued, firstSegment);
        if (firstSegment) {
            frameTiming->Add(frameTiming->Get("Submission", "copy"), submission.copied - submission.received, submission.commands.size() * sizeof(std::uint32_t));
            frameTiming->Add(frameTiming->Get("Submission", "validate"), submission.validated - submission.copied);
            frameTiming->Add(frameTiming->Get("Submission", "reserve_enqueue"), submission.enqueued - submission.validated);
        }
    }

    // Runs the submission from its cursor. Returns false when a WAIT_REG_MEM is not yet satisfied; the
    // cursor then points at that packet so the worker can resume it after other queues make progress.
    bool execute(Submission& submission) {
        const auto resumed = submission.started;
        if (!resumed) {
            submission.started = true;
            includeSubmission(submission, true);
            // Guest CPU writes made before the submission are ordered before its work.
            WriteTracker::NextEpoch();
        }
        if (submission.suspend) {
            PerformanceContext timingContext(frameTiming.get());
            PerformanceTimer timing("Driver.Suspend");
            const auto gpuLock = lockDevice();
            timing.Mark("gpu_mutex_wait");
            if (device != nullptr) device->WaitIdle();
            timing.Mark("device_idle_wait");
            stashCompletions(pollCompletions(true));
            resetGraphics = true;
            return true;
        }
        if (!resumed && submission.queue == 0 && resetGraphics) {
            queues.erase(0);
            resetGraphics = false;
        }
        auto& queue = queues[submission.queue];
        for (auto& cursor = submission.cursor; cursor < submission.commands.size();) {
            if (frameTiming == nullptr) includeSubmission(submission, false);
            const auto header = submission.commands[cursor];
            const auto count = static_cast<std::size_t>((header >> 16u) & 0x3fffu) + 2;
            const auto packet = std::span(submission.commands).subspan(cursor, count);
            const auto opcode = (header >> 8u) & 0xffu;
            if ((header & 1u) != 0 && queue.predicateSkip) {
                cursor += count;
                continue;
            }
            // Debug aid: ANYPS5_TRACE_PM4=1 logs every executed packet; DMA_DATA also logs its
            // destination and size.
            static const bool tracePm4 = std::getenv("ANYPS5_TRACE_PM4") != nullptr;
            if (tracePm4) {
                if (opcode == 0x15 && queue.shader.contains(0x20c) && queue.shader.contains(0x20d)) std::fprintf(stderr, "[pm4] q%u op=0x15 dispatch %ux%ux%u program=0x%llx\n", static_cast<unsigned>(submission.queue), packet[1], packet[2], packet[3], static_cast<unsigned long long>((static_cast<std::uint64_t>(queue.shader.at(0x20c)) << 8u) | (static_cast<std::uint64_t>(queue.shader.at(0x20d) & 0xffu) << 40u)));
                else if (opcode == 0x50 && count >= 7) std::fprintf(stderr, "[pm4] q%u op=0x%02x dst=0x%llx bytes=0x%x control=0x%08x src=0x%08x%08x\n", static_cast<unsigned>(submission.queue), static_cast<unsigned>(opcode), static_cast<unsigned long long>(packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u)), packet[6] & 0x3ffffffu, packet[1], packet[3], packet[2]);
                else if (opcode == 0x10 || opcode == 0x46) {
                    std::string words;
                    for (std::size_t i = 0; i < std::min<std::size_t>(count, 12); ++i) words += " " + std::to_string(packet[i]);
                    std::fprintf(stderr, "[pm4] q%u op=0x%02x dwords=%zu:%s\n", static_cast<unsigned>(submission.queue), static_cast<unsigned>(opcode), count, words.c_str());
                }
                else std::fprintf(stderr, "[pm4] q%u op=0x%02x dwords=%zu\n", static_cast<unsigned>(submission.queue), static_cast<unsigned>(opcode), count);
            }
            if (opcode == 0x22) {
                // COND_EXEC: the dword is read once earlier packets and their GPU writes are done (as
                // for a conditional branch); zero skips the guarded dwords of this buffer, which are
                // still unspliced here, so a guarded INDIRECT_BUFFER is skipped whole.
                std::uint32_t condition = 0;
                {
                    const auto gpuLock = lockDevice();
                    if (device != nullptr) device->WaitIdle();
                    stashCompletions(pollCompletions(true));
                    const GuestMemory::MemoryAccessScope memoryScope(device.get(), [](void* context, std::uint64_t address, std::size_t bytes, bool writable) {
                        if (context) static_cast<VulkanDevice*>(context)->ResolveMemory(address, bytes, writable);
                    });
                    const GuestMemory::AccessSite accessSite("cond_exec");
                    condition = Pm4::ReadCondition(packet);
                }
                publishCompletions();
                cursor += count + (condition == 0 ? Pm4::ConditionalWords(packet) : 0u);
                continue;
            }
            if (opcode == 0x3f && count == 14) {
                // The compare value may be written by earlier GPU work.
                std::optional<Pm4::BranchTarget> target;
                {
                    const auto gpuLock = lockDevice();
                    if (device != nullptr) device->WaitIdle();
                    stashCompletions(pollCompletions(true));
                    const GuestMemory::MemoryAccessScope memoryScope(device.get(), [](void* context, std::uint64_t address, std::size_t bytes, bool writable) {
                        if (context) static_cast<VulkanDevice*>(context)->ResolveMemory(address, bytes, writable);
                    });
                    const GuestMemory::AccessSite accessSite("cond_branch");
                    target = Pm4::ResolveBranch(packet);
                }
                if (target) spliceCommands(submission, cursor, count, target->address, target->dwords, true);
                else spliceCommands(submission, cursor, count, 0, 0, false);
                continue;
            }
            if (opcode == 0x3f) {
                spliceIndirectBuffer(submission, cursor);
                continue;
            }
            // A DMA_DATA fill (immediate source) of a registered CMASK clears it (shadPS4 FillBuffer).
            if (opcode == 0x50 && count >= 7 && (((packet[1] >> 29u) & 3u) | ((packet[6] >> 24u) & 4u) | ((packet[6] >> 25u) & 8u)) == 2u) {
                // The metadata registry belongs to device work.
                if (pipeline) pipeline->Drain();
                Graphics::NoteMetadataClear(packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u));
            }
            {
                PerformanceContext timingContext(frameTiming.get());
                PerformanceTimer timing("Driver.Packet");
                CheckFailure();
                timing.Mark("failure_check");
                if (opcode == 0x3c || opcode == 0x93) {
                    // Opt-in (ANYPS5_QUEUE_WAIT_REG_MEM=1): a wait on a label a pending release
                    // writes is a GPU barrier (see below), queued in order like one, with no drain.
                    // Zorro gained ~0.5 ms a frame but Hellboy lost ~1 ms, so waits drain by default.
                    static const bool drainWaits = std::getenv("ANYPS5_QUEUE_WAIT_REG_MEM") == nullptr;
                    static const bool cpuWaits = std::getenv("ANYPS5_CPU_WAIT_REG_MEM") != nullptr;
                    if (pipeline && device != nullptr && !drainWaits && !cpuWaits && pendingReleaseSatisfies(packet)) {
                        pipeline->Post([device = device] { device->AcquireGpuMemory(); }, {}, false);
                        timing.Mark("gpu_wait_queued");
                        cursor += count;
                        continue;
                    }
                    // A label already in memory is read like any worker read (waiting only for a
                    // queued job that writes it); a wait it does not satisfy yet drains below.
                    if (pipeline && device != nullptr && !drainWaits && !cpuWaits) {
                        bool satisfied = false;
                        {
                            const GuestMemory::MemoryAccessScope memoryScope(accessContext(), accessResolver(), true);
                            const GuestMemory::AccessSite accessSite("wait_reg_mem");
                            satisfied = Pm4::WaitSatisfied(packet);
                        }
                        if (satisfied) {
                            timing.Mark("memory_wait");
                            WriteTracker::NextEpoch();
                            cursor += count;
                            continue;
                        }
                    }
                    const auto gpuLock = lockDevice();
                    const GuestMemory::MemoryAccessScope memoryScope(device.get(), [](void* context, std::uint64_t address, std::size_t bytes, bool writable) {
                        if (context) static_cast<VulkanDevice*>(context)->ResolveMemory(address, bytes, writable);
                    });
                    const GuestMemory::AccessSite accessSite("wait_reg_mem");
                    // The packet only waits until memory satisfies its condition: when it already
                    // does (labels are usually written by then), the GPU need not go idle first.
                    static const bool traceDma = std::getenv("APS5_TRACE_DMA") != nullptr;
                    if (traceDma) std::fprintf(stderr, "[wait] q%u 0x%llx func %u ref 0x%x mask 0x%x\n", static_cast<unsigned>(submission.queue), static_cast<unsigned long long>(packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u)), packet[1] & 7u, packet[4], packet[5]);
                    // A wait on a label an earlier RELEASE_MEM writes (still pending: its GPU work is
                    // in flight) is a GPU-side drain on the console, not a CPU stall: all GPU work
                    // runs in one queue in submission order, so a full GPU barrier orders what
                    // follows after the released work. The label lands with its completion, and
                    // packets the CPU executes drain completions first. Reading the label here would
                    // wait for the GPU. ANYPS5_CPU_WAIT_REG_MEM=1 waits on the CPU.
                    static const bool cpuWait = std::getenv("ANYPS5_CPU_WAIT_REG_MEM") != nullptr;
                    if (!cpuWait && device != nullptr && (predictedWaitSatisfied(packet) || std::any_of(completions.begin(), completions.end(), [&](const Completion& pending) { return Pm4::ReleaseSatisfiesWait(packet, pending.release); }))) {
                        device->AcquireGpuMemory();
                        timing.Mark("gpu_wait");
                        cursor += count;
                        continue;
                    }
                    bool satisfied = Pm4::WaitSatisfied(packet);
                    timing.Mark("memory_check");
                    if (!satisfied && !completions.empty()) {
                        stashCompletions(pollCompletions(true));
                        satisfied = Pm4::WaitSatisfied(packet);
                        timing.Mark("completion_wait");
                    }
                    if (!satisfied && device != nullptr) {
                        device->WaitIdle();
                        satisfied = Pm4::WaitSatisfied(packet);
                    }
                    if (!satisfied) {
                        blockedWait = Pm4::DescribeWait(packet);
                        return false;
                    }
                    timing.Mark("memory_wait");
                    // Whatever the guest wrote before releasing the wait is ordered before what follows.
                    WriteTracker::NextEpoch();
                    cursor += count;
                    continue;
                }
                if (opcode == 0x49) {
                    // RELEASE_MEM writes its label (and raises its interrupt) once the GPU work
                    // before it completed; the worker goes on meanwhile.
                    Completion completion;
                    completion.release.assign(packet.begin(), packet.end());
                    completion.eventQueue = static_cast<int>(submission.queue);
                    if (((packet[2] >> 24u) & 7u) != 0) completion.interrupt = packet[7];
                    if (TraceRelease()) std::fprintf(stderr, "[agc-release] queued serial=%llu queue=0x%x label=0x%llx data=0x%08x%08x select=%u interrupt=%u context=0x%x\n", static_cast<unsigned long long>(submission.serial), static_cast<unsigned>(submission.queue), static_cast<unsigned long long>(packet[3] | (static_cast<std::uint64_t>(packet[4]) << 32u)), packet[6], packet[5], packet[2] >> 29u, (packet[2] >> 24u) & 7u, packet[7]);
                    deferredReleases.emplace_back(++releasesDeferred, completion.release);
                    deferCompletion(std::move(completion));
                    timing.Mark("release_deferred");
                    cursor += count;
                    continue;
                }
                if (pipeline && device != nullptr && ((opcode == 0x46 && (packet[1] & 0x3fu) != 0x39u) || opcode == 0x58)) {
                    // A GPU barrier is device work like a draw: queued in order, no drain.
                    pipeline->Post([device = device] { device->AcquireGpuMemory(); }, {}, false);
                    timing.Mark("gpu_barrier_queued");
                    publishCompletions();
                    cursor += count;
                    continue;
                }
                if (opcode == 0x37 || opcode == 0x40 || opcode == 0x50 || opcode == 0x42 || opcode == 0x46 || opcode == 0x58 || header == FlipPacketHeader) {
                    const auto gpuLock = lockDevice();
                    timing.Mark("gpu_mutex_wait");
                    // Cache and partial-flush events and ACQUIRE_MEM order GPU work against GPU work:
                    // a barrier. Later CPU accesses are ordered by the packets that make them (they
                    // wait for draws and pending labels below) and by guest range checks.
                    // PIXEL_PIPE_STAT_DUMP: drained like a label (every draw before it completed), then
                    // the sample count is written as the DB counters.
                    // DMA_DATA between imported guest ranges runs on the GPU in queue order: no drain.
                    // So does WRITE_DATA to memory (vkCmdUpdateBuffer).
                    if ((opcode == 0x50 || opcode == 0x37) && device != nullptr) {
                        std::uint64_t serial = 0;
                        if (const auto dma = opcode == 0x50 ? Pm4::DecodeDmaCopy(packet) : Pm4::DecodeWriteData(packet); dma && device->RecordDmaData(*dma, &serial)) {
                            predictWrite(*dma, serial);
                            timing.Mark("dma_gpu");
                            cursor += count;
                            continue;
                        }
                    }
                    const auto sampleDump = opcode == 0x46 && (packet[1] & 0x3fu) == 0x39u;
                    const auto gpuBarrier = (opcode == 0x46 && !sampleDump) || opcode == 0x58;
                    const auto waitDraws = opcode == 0x37 || opcode == 0x40 || opcode == 0x50 || opcode == 0x42 || sampleDump;
                    if (sampleDump) {
                        dumpSampleCounters(packet, device.get());
                    } else if (device != nullptr) {
                        if (gpuBarrier) device->AcquireGpuMemory();
                        else if (waitDraws) {
                            // Which packet types make the worker drain the GPU (Driver.DrainFor.<packet>).
                            PerformanceTimer drainTiming(opcode == 0x37 ? "Driver.DrainFor.WriteData" : opcode == 0x40 ? "Driver.DrainFor.CopyData" : opcode == 0x50 ? "Driver.DrainFor.DmaData" : "Driver.DrainFor.PfpSyncMe");
                            device->WaitDraws();
                        }
                        else if (!VulkanDevice::AsyncFlip()) {
                            PerformanceTimer waitTiming("Driver.FlipWait");
                            device->WaitIdle();
                        }
                    }
                    // The CPU executes these packets (or presents): labels released before them land
                    // first. An asynchronous flip is itself a completion queued behind them.
                    const bool asyncFlip = header == FlipPacketHeader && device != nullptr && VulkanDevice::AsyncFlip();
                    if (!gpuBarrier) stashCompletions(pollCompletions(!asyncFlip));
                    timing.Mark(gpuBarrier ? "gpu_barrier" : waitDraws ? "draw_wait" : "device_idle_wait");
                }
                // Errors name the packet that raised them: the worker's failure is reported on
                // another thread, far from the throw.
                const auto withContext = [&](auto&& run) {
                    try {
                        run();
                    } catch (const std::exception& error) {
                        char context[80];
                        std::snprintf(context, sizeof(context), " (PM4 opcode 0x%02x, queue %u, dword %zu)", static_cast<unsigned>(opcode), static_cast<unsigned>(submission.queue), cursor);
                        throw std::runtime_error(std::string(error.what()) + context);
                    }
                };
                if (header == RenderingWaitPacketHeader) {
                    submission.renderingWaits.at(cursor)->Wait();
                    timing.Mark("rendering_wait");
                } else if (header == FlipPacketHeader) {
                    CheckFailure();
                    timing.Mark("flip_prepare");
                } else if (opcode == 0x15) {
                    withContext([&] { dispatch(queue, packet, submission); });
                } else if (opcode == 0x16) {
                    std::array<std::uint32_t, 5> direct;
                    {
                        std::unique_lock<std::recursive_mutex> gpuLock(gpuMutex, std::defer_lock);
                        if (!pipeline) gpuLock.lock();
                        const GuestMemory::MemoryAccessScope memoryScope(accessContext(), accessResolver(), pipeline != nullptr);
                        const GuestMemory::AccessSite accessSite("indirect_dispatch_args");
                        direct = Pm4::ResolveDispatch(packet, queue);
                    }
                    withContext([&] { dispatch(queue, direct, submission); });
                } else if (opcode == 0x35 || opcode == 0x27 || opcode == 0x2d) {
                    Pm4::DrawParameters parameters;
                    {
                        // The draw packet's checks are the worker's own guest accesses (unlocked when
                        // pipelined); without the pipeline they resolve as before.
                        std::optional<GuestMemory::MemoryAccessScope> memoryScope;
                        if (pipeline) memoryScope.emplace(accessContext(), accessResolver(), true);
                        withContext([&] { parameters = Pm4::ResolveDraw(packet, queue); });
                    }
                    withContext([&] { draw(queue, parameters, submission); });
                } else if (opcode == 0x24 || opcode == 0x25 || opcode == 0x2c || opcode == 0x38) {
                    std::vector<Pm4::IndirectDraw> draws;
                    {
                        std::unique_lock<std::recursive_mutex> gpuLock(gpuMutex, std::defer_lock);
                        if (!pipeline) gpuLock.lock();
                        const GuestMemory::MemoryAccessScope memoryScope(accessContext(), accessResolver(), pipeline != nullptr);
                        const GuestMemory::AccessSite accessSite("indirect_draw_args");
                        withContext([&] { draws = Pm4::ResolveIndirectDraws(packet, queue); });
                    }
                    for (const auto& indirect : draws) {
                        // The command processor writes the arguments into the SH registers before
                        // each draw; they persist like any register write.
                        for (const auto& [location, value] : indirect.registers) queue.shader[location] = value;
                        withContext([&] { draw(queue, indirect.parameters, submission); });
                    }
                } else if (opcode != 0x42 && opcode != 0x46 && opcode != 0x58) {
                    std::unique_lock<std::recursive_mutex> gpuLock(gpuMutex, std::defer_lock);
                    if (!pipeline) gpuLock.lock();
                    const GuestMemory::MemoryAccessScope memoryScope(accessContext(), accessResolver(), pipeline != nullptr);
                    const GuestMemory::AccessSite accessSite("pm4_execute");
                    withContext([&] { Pm4::Execute(packet, queue); });
                    timing.Mark("pm4_execute");
                }
            }
            publishCompletions();
            if (header == FlipPacketHeader) {
                WriteTracker::NextEpoch();
                frameTiming->SetFlip(submission.serial, cursor, submission.received, FrameTiming::Clock::now());
                {
                    const auto gpuLock = lockDevice();
                    if (device) device->ReportGpuTime(*frameTiming);
                }
                const auto completedFrame = std::exchange(frameTiming, nullptr);
                bool queued = false;
                if (VulkanDevice::AsyncFlip()) {
                    const auto gpuLock = lockDevice();
                    if (device != nullptr) {
                        Completion completion;
                        completion.flip = submission.flips.at(cursor);
                        completion.flipTiming = completedFrame;
                        queueCompletion(std::move(completion));
                        queued = true;
                    }
                }
                if (!queued) submission.flips.at(cursor)->GpuReady(completedFrame);
            }
            cursor += count;
        }
        return true;
    }

    // Serials can finish out of order once queues interleave; waiters see the contiguous prefix.
    void markCompleted(std::uint64_t serial) {
        finishedOutOfOrder.insert(serial);
        while (!finishedOutOfOrder.empty() && *finishedOutOfOrder.begin() == completed + 1) {
            completed = *finishedOutOfOrder.begin();
            finishedOutOfOrder.erase(finishedOutOfOrder.begin());
        }
    }

    // A submission may run once every earlier active submission on its queue has finished; suspend
    // boundaries order against everything.
    bool runnable(std::size_t index) const {
        const auto& candidate = active[index];
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            const auto& other = active[earlier];
            if (other.suspend || candidate.suspend || other.queue == candidate.queue) return false;
        }
        return true;
    }

    // Debug aid: APS5_TRACE_WORKER=1 accounts the worker's time every 10 s: idle (no submission),
    // blocked (every active submission waits on memory) and running, with the commonest blocking waits.
    struct WorkerTrace {
        bool enabled = std::getenv("APS5_TRACE_WORKER") != nullptr;
        std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
        double idleMs = 0, blockedMs = 0;
        std::map<std::string, std::pair<std::uint64_t, double>> blockers;
        void add(double& total, std::chrono::steady_clock::time_point started, const std::string* blocker = nullptr) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            total += ms;
            if (blocker != nullptr) {
                auto& entry = blockers[blocker->substr(0, 96)];
                ++entry.first;
                entry.second += ms;
            }
            const double window = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - windowStart).count();
            if (window < 10000.0) return;
            std::fprintf(stderr, "[worker] %.0f ms window: idle %.0f ms, blocked %.0f ms, running %.0f ms\n", window, idleMs, blockedMs, window - idleMs - blockedMs);
            std::vector<std::pair<double, std::string>> top;
            for (const auto& [name, value] : blockers) top.emplace_back(value.second, name + " x" + std::to_string(value.first));
            std::sort(top.rbegin(), top.rend());
            for (std::size_t i = 0; i < top.size() && i < 6; ++i) std::fprintf(stderr, "[worker]   blocked %.0f ms on %s\n", top[i].first, top[i].second.c_str());
            windowStart = std::chrono::steady_clock::now();
            idleMs = blockedMs = 0;
            blockers.clear();
        }
    } workerTrace;

    void run() noexcept {
        NameThread("AgcWorker");
        // Memory waits normally resolve within microseconds; one that stays blocked this long with no
        // other work able to run is a synchronization the emulation cannot satisfy.
        constexpr auto stallLimit = std::chrono::seconds(10);
        auto lastProgress = std::chrono::steady_clock::now();
        try {
            for (;;) {
                {
                    PerformanceContext timingContext(frameTiming.get());
                    PerformanceTimer timing("Driver.Worker");
                    std::unique_lock lock(mutex);
                    timing.Mark("queue_mutex_wait");
                    if (active.empty()) {
                        const auto idleStart = std::chrono::steady_clock::now();
                        changed.wait(lock, [&] { return failure || stopping || !pending.empty(); });
                        if (workerTrace.enabled) workerTrace.add(workerTrace.idleMs, idleStart);
                    }
                    timing.Mark("wait_for_submission");
                    rethrowFailure();
                    while (!pending.empty()) {
                        active.push_back(std::move(pending.front()));
                        pending.pop_front();
                        active.back().dequeued = FrameTiming::Clock::now();
                    }
                    if (active.empty()) {
                        break;
                    }
                }
                bool progressed = false;
                for (std::size_t index = 0; index < active.size();) {
                    if (!runnable(index)) {
                        ++index;
                        continue;
                    }
                    const auto startCursor = active[index].cursor;
                    const auto wasStarted = active[index].started;
                    const auto finished = execute(active[index]);
                    publishCompletions();
                    if (!finished) {
                        if (active[index].cursor != startCursor || !wasStarted) progressed = true;
                        ++index;
                        continue;
                    }
                    {
                        PerformanceContext timingContext(frameTiming.get());
                        PerformanceTimer timing("Driver.SubmissionCompletion");
                        {
                            Completion completion;
                            completion.serial = active[index].serial;
                            deferCompletion(std::move(completion));
                            // Finished ones are published now if the device is free, else by the completer.
                            std::unique_lock gpuLock(gpuMutex, std::try_to_lock);
                            if (gpuLock.owns_lock()) stashCompletions(pollCompletions(false));
                        }
                        publishCompletions();
                    }
                    {
                        std::lock_guard lock(mutex);
                        rethrowFailure();
                    }
                    active.erase(active.begin() + static_cast<std::ptrdiff_t>(index));
                    progressed = true;
                    changed.notify_all();
                }
                if (progressed) {
                    lastProgress = std::chrono::steady_clock::now();
                    continue;
                }
                require(std::chrono::steady_clock::now() - lastProgress < stallLimit, ("GPU memory wait never satisfied: " + blockedWait).c_str());
                std::unique_lock lock(mutex);
                const auto blockedStart = std::chrono::steady_clock::now();
                changed.wait_for(lock, std::chrono::microseconds(200), [&] { return failure || !pending.empty(); });
                if (workerTrace.enabled) workerTrace.add(workerTrace.blockedMs, blockedStart, &blockedWait);
            }
            const auto gpuLock = lockDevice();
            device.reset();
        } catch (...) {
            const auto error = std::current_exception();
            for (const auto& submission : active)
                for (const auto& [offset, flip] : submission.flips) flip->Fail(error);
            ReportFailure(error);
            {
                std::lock_guard gpuLock(gpuMutex);
                device.reset();
            }
        }
    }
};

}

void Submit(const Packet* packet, std::uint32_t queue) {
    Driver::Get().Submit(packet, queue);
}

void WaitIdle() {
    Driver::Get().WaitIdle();
}

void RegisterShader(const Shader* shader) {
    Driver::Get().RegisterShader(shader);
}

void SuspendPoint() {
    Driver::Get().SuspendPoint();
}

void RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    Driver::Get().RegisterVideoOutput(handle, output);
}

void UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    Driver::Get().UnregisterVideoOutput(handle, output);
}

void PresentClear(const PresentationWindow& window, bool opaque, void (*gpuReady)(void*), void* context) {
    Driver::Get().Present(window, nullptr, opaque, gpuReady, context);
}

void PresentBuffer(const PresentationWindow& window, const DisplayBuffer& buffer, void (*gpuReady)(void*), void* context) {
    Driver::Get().Present(window, &buffer, true, gpuReady, context);
}

void ReleaseWindow(void* window) {
    Driver::Get().ReleaseWindow(window);
}

void ReportFailure(std::exception_ptr error) {
    Driver::Get().ReportFailure(error);
}

}

extern "C" void AgcDriverWaitIdle_nid_postfix() {
    AgcDriver::WaitIdle();
}

extern "C" void AgcDriverRegisterShader_nid_postfix(const Shader* shader) {
    AgcDriver::RegisterShader(shader);
}

extern "C" void AgcDriverSuspendPoint_nid_postfix() {
    AgcDriver::SuspendPoint();
}

extern "C" void AgcDriverRegisterVideoOutput_nid_postfix(std::uint32_t handle, const std::shared_ptr<AgcDriver::IVideoOutput>& output) {
    AgcDriver::RegisterVideoOutput(handle, output);
}

extern "C" void AgcDriverUnregisterVideoOutput_nid_postfix(std::uint32_t handle, const std::shared_ptr<AgcDriver::IVideoOutput>& output) {
    AgcDriver::UnregisterVideoOutput(handle, output);
}

extern "C" void AgcDriverPresentClear_nid_postfix(const AgcDriver::PresentationWindow& window, bool opaque, void (*gpuReady)(void*), void* context) {
    AgcDriver::PresentClear(window, opaque, gpuReady, context);
}

extern "C" void AgcDriverPresentBuffer_nid_postfix(const AgcDriver::PresentationWindow& window, const AgcDriver::DisplayBuffer& buffer, void (*gpuReady)(void*), void* context) {
    AgcDriver::PresentBuffer(window, buffer, gpuReady, context);
}

extern "C" void AgcDriverReleaseWindow_nid_postfix(void* window) {
    AgcDriver::ReleaseWindow(window);
}

extern "C" void AgcDriverReportFailure_nid_postfix(std::exception_ptr error) {
    AgcDriver::ReportFailure(error);
}
