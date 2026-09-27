#include "../include/PthreadStacks.hpp"
#include <map>
#include <mutex>
#include <stdexcept>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace PthreadStacks {

namespace {

std::mutex& registryMutex() {
    static std::mutex instance;
    return instance;
}

std::map<std::uintptr_t, std::uintptr_t>& registry() {
    static std::map<std::uintptr_t, std::uintptr_t> instance;
    return instance;
}

// Thread-local destructors also run when a thread leaves through pthread_exit, so a registration
// never outlives its stack.
struct CurrentRegistration {
    std::uintptr_t start = 0;
    ~CurrentRegistration() {
        if (start == 0)
            return;
        const std::lock_guard lock(registryMutex());
        registry().erase(start);
    }
};

thread_local CurrentRegistration current;

bool currentBounds(std::uintptr_t& start, std::uintptr_t& end) {
#if defined(__linux__)
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0)
        return false;
    void* base = nullptr;
    std::size_t size = 0;
    const int result = pthread_attr_getstack(&attributes, &base, &size);
    pthread_attr_destroy(&attributes);
    if (result != 0 || base == nullptr || size == 0)
        return false;
    start = reinterpret_cast<std::uintptr_t>(base);
    end = start + size;
    return true;
#else
    (void)start;
    (void)end;
    return false;
#endif
}

}

void CurrentBounds(void** lowest, std::size_t* size) {
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
    if (!currentBounds(start, end))
        throw std::runtime_error("cannot determine the current thread stack");
    *lowest = reinterpret_cast<void*>(start);
    *size = end - start;
}

void RegisterCurrent() {
    if (current.start != 0)
        return;
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
    if (!currentBounds(start, end))
        throw std::runtime_error("cannot determine the current thread stack");
    const std::lock_guard lock(registryMutex());
    registry()[start] = end;
    current.start = start;
}

void UnregisterCurrent() {
    if (current.start == 0)
        return;
    const std::lock_guard lock(registryMutex());
    registry().erase(current.start);
    current.start = 0;
}

bool Find(std::uintptr_t address, void** start, void** end) {
    RegisterCurrent();
    const std::lock_guard lock(registryMutex());
    auto it = registry().upper_bound(address);
    if (it == registry().begin())
        return false;
    --it;
    if (address >= it->second)
        return false;
    if (start) *start = reinterpret_cast<void*>(it->first);
    if (end) *end = reinterpret_cast<void*>(it->second);
    return true;
}

}
