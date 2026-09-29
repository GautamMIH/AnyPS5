#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PERFORMANCETIMER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PERFORMANCETIMER_HPP

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace AgcDriver {

class FrameTiming {
public:
    using Clock = std::chrono::steady_clock;

    struct Metric {
        Clock::duration total{};
        Clock::duration maximum{};
        std::uint64_t count = 0;
        std::uint64_t bytes = 0;
    };

    explicit FrameTiming(std::uint64_t id) : id(id) {}

    // Scope and stage names are static strings, looked up by address: timers run on hot paths
    // (hundreds of thousands of times a frame), so a lookup must not compare names. The same name
    // at two addresses (string literals of different modules) is merged when the frame is printed.
    Metric* Get(const char* scope, const char* stage) {
        std::lock_guard lock(mutex);
        return &metrics[{scope, stage}];
    }

    void Add(Metric* metric, Clock::duration elapsed, std::uint64_t bytes = 0) {
        std::lock_guard lock(mutex);
        add(*metric, elapsed, bytes);
    }

    void Add(const char* scope, const char* stage, Clock::duration elapsed, std::uint64_t bytes = 0) {
        std::lock_guard lock(mutex);
        add(metrics[{scope, stage}], elapsed, bytes);
    }

    void IncludeSubmission(std::uint64_t serial, Clock::time_point received, Clock::time_point enqueued, Clock::time_point dequeued, bool firstSegment) {
        if (serial == 0 || received > enqueued || enqueued > dequeued) throw std::runtime_error("Frame timing: invalid submission timestamps");
        if (firstSerial == 0) {
            firstSerial = serial;
            start = received;
            executionStart = dequeued;
        }
        start = std::min(start, received);
        lastSerial = serial;
        if (firstSegment) {
            Add(Get("Submission", "accept"), enqueued - received);
            Add(Get("Submission", "queue"), dequeued - enqueued);
        }
    }

    void SetFlip(std::uint64_t serial, std::size_t offset, Clock::time_point received, Clock::time_point reached) {
        if (serial != lastSerial || received > reached || executionStart > reached) throw std::runtime_error("Frame timing: invalid flip lineage");
        flipSerial = serial;
        flipOffset = offset;
        flipReceived = received;
        flipReached = reached;
    }

    void Print(std::uint32_t outputHandle, std::int32_t buffer, std::int64_t argument, Clock::time_point finished, Clock::duration interval) {
        std::lock_guard lock(mutex);
        std::map<std::pair<std::string_view, std::string_view>, Metric> merged;
        for (const auto& [key, metric] : metrics) {
            auto& target = merged[{key.first, key.second}];
            target.total += metric.total;
            target.maximum = std::max(target.maximum, metric.maximum);
            target.count += metric.count;
            target.bytes += metric.bytes;
        }
        if (firstSerial == 0 || flipSerial == 0) throw std::runtime_error("Frame timing: incomplete submission lineage");
        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << std::fixed << std::setprecision(3);
        output << "[FrameTiming] frame=" << id << " submissions=" << firstSerial << ':' << lastSerial;
        output << " output=" << outputHandle;
        output << " flip=" << flipSerial << ':' << flipOffset << " buffer=" << buffer << " argument=" << argument;
        output << " endpoint=flip_complete display_confirmed=0";
        output << " first_submit_to_flip_ms=" << milliseconds(finished - start);
        output << " flip_submit_to_complete_ms=" << milliseconds(finished - flipReceived);
        output << " first_submit_to_execute_ms=" << milliseconds(executionStart - start);
        output << " execute_to_flip_packet_ms=" << milliseconds(flipReached - executionStart);
        output << " flip_packet_to_complete_ms=" << milliseconds(finished - flipReached);
        auto accounted = Clock::duration::zero();
        for (const auto scope : {"Driver.Packet", "Driver.Suspend", "Driver.Worker", "Driver.Completion"}) {
            const auto it = merged.find({scope, "total"});
            if (it != merged.end()) accounted += it->second.total;
        }
        output << " worker_unattributed_ms=" << milliseconds(flipReached - executionStart - accounted);
        if (interval != Clock::duration::zero()) output << " flip_interval_ms=" << milliseconds(interval);
        output << " metrics=inclusive(count,sum_ms,max_ms[,bytes])";
        for (const auto& [key, metric] : merged) {
            if (metric.count == 0) continue;
            output << ' ' << key.first << '.' << key.second << "=(" << metric.count << ',' << milliseconds(metric.total) << ',' << milliseconds(metric.maximum);
            if (metric.bytes != 0) output << ',' << metric.bytes;
            output << ')';
        }
        output << '\n';
        const auto text = output.str();
        if (std::fwrite(text.data(), 1, text.size(), stdout) != text.size() || std::fflush(stdout) != 0) throw std::runtime_error("Frame timing: report write failed");
    }

private:
    static void add(Metric& metric, Clock::duration elapsed, std::uint64_t bytes) {
        metric.total += elapsed;
        metric.maximum = std::max(metric.maximum, elapsed);
        ++metric.count;
        metric.bytes += bytes;
    }

    struct KeyHash {
        std::size_t operator()(const std::pair<const char*, const char*>& key) const noexcept {
            const auto scope = reinterpret_cast<std::uintptr_t>(key.first);
            const auto stage = reinterpret_cast<std::uintptr_t>(key.second);
            return static_cast<std::size_t>((scope * 0x9e3779b97f4a7c15ull) ^ (stage + 0x7f4a7c15u + (scope >> 6u)));
        }
    };

    static double milliseconds(Clock::duration elapsed) {
        return std::chrono::duration<double, std::milli>(elapsed).count();
    }

    std::mutex mutex;
    // Node-based: Metric pointers handed out by Get stay valid as the map grows.
    std::unordered_map<std::pair<const char*, const char*>, Metric, KeyHash> metrics;
    std::uint64_t id;
    std::uint64_t firstSerial = 0;
    std::uint64_t lastSerial = 0;
    std::uint64_t flipSerial = 0;
    std::size_t flipOffset = 0;
    Clock::time_point start{};
    Clock::time_point executionStart{};
    Clock::time_point flipReceived{};
    Clock::time_point flipReached{};
};

class PerformanceContext {
public:
    explicit PerformanceContext(FrameTiming* frame) : previous(current) {
        current = frame;
    }

    PerformanceContext(const PerformanceContext&) = delete;
    PerformanceContext& operator=(const PerformanceContext&) = delete;

    ~PerformanceContext() {
        current = previous;
    }

    static FrameTiming* Current() {
        return current;
    }

private:
    inline static thread_local FrameTiming* current = nullptr;
    FrameTiming* previous;
};

class PerformanceTimer {
public:
    explicit PerformanceTimer(const char* scope) : frame(PerformanceContext::Current()), scope(scope) {
        if (frame != nullptr) {
            total = frame->Get(scope, "total");
        }
        start = Clock::now();
        previous = start;
    }

    PerformanceTimer(const PerformanceTimer&) = delete;
    PerformanceTimer& operator=(const PerformanceTimer&) = delete;

    ~PerformanceTimer() {
        if (frame == nullptr) return;
        const auto end = Clock::now();
        if (marked) frame->Add(scope, "tail", end - previous);
        frame->Add(total, end - start);
    }

    void Mark(const char* stage, std::uint64_t bytes = 0) {
        if (frame == nullptr) return;
        const auto now = Clock::now();
        frame->Add(scope, stage, now - previous, bytes);
        marked = true;
        previous = now;
    }

private:
    using Clock = FrameTiming::Clock;

    FrameTiming* frame;
    const char* scope;
    FrameTiming::Metric* total = nullptr;
    bool marked = false;
    Clock::time_point start;
    Clock::time_point previous;
};

}

#endif
