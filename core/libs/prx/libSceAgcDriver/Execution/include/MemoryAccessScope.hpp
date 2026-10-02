#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MEMORYACCESSSCOPE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MEMORYACCESSSCOPE_HPP

#include <cstddef>
#include <cstdint>

namespace AgcDriver::GuestMemory {

class MemoryAccessScope {
public:
    using Resolver = void (*)(void*, std::uint64_t, std::size_t, bool);
    MemoryAccessScope(void* context, Resolver resolver) : previousContext(currentContext), previousResolver(currentResolver) {
        currentContext = context;
        currentResolver = resolver;
    }
    ~MemoryAccessScope() {
        currentContext = previousContext;
        currentResolver = previousResolver;
    }
    MemoryAccessScope(const MemoryAccessScope&) = delete;
    MemoryAccessScope& operator=(const MemoryAccessScope&) = delete;
    // Whether a resolver is installed (the device's render cache and draw queue).
    static bool HasResolver() { return currentResolver != nullptr; }
    static void Resolve(std::uint64_t address, std::size_t bytes, bool writable) {
        const auto resolver = currentResolver;
        const auto context = currentContext;
        if (resolver == nullptr || bytes == 0) return;
        const MemoryAccessScope suspended(nullptr, nullptr);
        resolver(context, address, bytes, writable);
    }

private:
    inline static thread_local void* currentContext = nullptr;
    inline static thread_local Resolver currentResolver = nullptr;
    void* previousContext;
    Resolver previousResolver;
};

// Profiling: names the access a guest memory check is made for (a static timing mark name).
class AccessSite {
public:
    explicit AccessSite(const char* name) : previous(current) { current = name; }
    ~AccessSite() { current = previous; }
    AccessSite(const AccessSite&) = delete;
    AccessSite& operator=(const AccessSite&) = delete;
    static const char* Current() { return current != nullptr ? current : "cpu_wait_other"; }

private:
    inline static thread_local const char* current = nullptr;
    const char* previous;
};

// Marks guest memory checks made for accesses the GPU performs itself, through imported guest
// memory: earlier GPU writes to the range then need a GPU memory barrier, not a CPU wait. Checks
// for CPU reads (snapshots, uploads) are made outside such a scope.
class GpuAccessScope {
public:
    GpuAccessScope() : previous(active) { active = true; }
    ~GpuAccessScope() { active = previous; }
    GpuAccessScope(const GpuAccessScope&) = delete;
    GpuAccessScope& operator=(const GpuAccessScope&) = delete;
    static bool Active() { return active; }

private:
    inline static thread_local bool active = false;
    bool previous;
};

}

#endif
