#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_THREADPLACEMENT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_THREADPLACEMENT_HPP

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#endif

namespace AgcDriver {

// Pins the calling driver thread to one physical performance core, its own: rank 0 takes the last
// such core, rank 1 the one before (ANYPS5_PIN_DRIVER_THREADS=0 leaves placement to the scheduler).
// Zorro gameplay (3 alternating runs): 4179-4227 frames in 120 s against 3972-4020, heavy-frame
// median 36.0-36.3 ms against 39.1-39.9; Hellboy unchanged or better (fast runs 16.3 against 17.0 ms). On hybrid CPUs Linux lists
// the performance cores' logical CPUs in /sys/devices/cpu_core/cpus; elsewhere every CPU counts. The
// driver's worker and device thread are the frame's critical path, and an efficiency core or a
// hyperthread sibling shared with a spinning game thread slows them. Linux only.
inline void PinToPerformanceCore(unsigned rank) {
#ifndef _WIN32
    static const bool enabled = [] {
        const char* value = std::getenv("ANYPS5_PIN_DRIVER_THREADS");
        return value == nullptr || std::string(value) != "0";
    }();
    if (!enabled) return;
    // Logical CPUs from a list such as "0-7,9".
    const auto parseList = [](const std::string& text) {
        std::set<int> cpus;
        std::stringstream input(text);
        std::string part;
        while (std::getline(input, part, ',')) {
            if (part.empty()) continue;
            const auto dash = part.find('-');
            const int first = std::atoi(part.substr(0, dash).c_str());
            const int last = dash == std::string::npos ? first : std::atoi(part.substr(dash + 1).c_str());
            for (int cpu = first; cpu <= last; ++cpu) cpus.insert(cpu);
        }
        return cpus;
    };
    const auto readLine = [](const std::string& path) {
        std::ifstream file(path);
        std::string line;
        std::getline(file, line);
        return line;
    };
    auto performance = parseList(readLine("/sys/devices/cpu_core/cpus"));
    if (performance.empty()) performance = parseList(readLine("/sys/devices/system/cpu/online"));
    // Physical cores, by their lowest logical CPU.
    std::map<int, std::set<int>> cores;
    for (const auto cpu : performance) {
        auto siblings = parseList(readLine("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list"));
        if (siblings.empty()) siblings = {cpu};
        cores[*siblings.begin()] = siblings;
    }
    if (cores.size() <= rank) return;
    auto it = cores.rbegin();
    std::advance(it, rank);
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const auto cpu : it->second) CPU_SET(cpu, &set);
    const int result = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    static const bool trace = std::getenv("APS5_TRACE_PIN") != nullptr;
    if (trace) std::fprintf(stderr, "[pin] rank %u -> core of CPU %d (%zu logical CPUs): %s\n", rank, it->first, it->second.size(), result == 0 ? "ok" : "failed");
#else
    static_cast<void>(rank);
#endif
}

}

#endif
