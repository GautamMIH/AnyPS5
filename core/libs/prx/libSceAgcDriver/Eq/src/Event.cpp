#include "prx/libSceAgcDriver/Eq/include/Event.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"

namespace {

struct Registration {
    KernelEqueue eq;
    int id;
};

std::mutex& registrationMutex() {
    static std::mutex instance;
    return instance;
}

std::vector<Registration>& registrations() {
    static std::vector<Registration> instance;
    return instance;
}

}

extern "C" {

int APS5_VABI sceAgcDriverAddEqEvent(KernelEqueue eq, int id, void* udata) {
    if (eq == 0 || id < 0) return EQUEUE_ERROR_EINVAL;
    static const bool trace = std::getenv("APS5_TRACE_AGC_RELEASE") != nullptr;
    if (trace) std::fprintf(stderr, "[agc-release] register eq=%p id=0x%x\n", reinterpret_cast<void*>(eq), static_cast<unsigned>(id));
    KernelEqueueEvent event{};
    event.event.ident = static_cast<uintptr_t>(id);
    event.event.filter = EVFILT_GRAPHICS_CORE;
    event.event.flags = EV_ADD | EV_CLEAR;
    event.event.udata = udata;
    event.filter.triggerFunc = [](KernelEqueueEvent* e, void* context) {
        e->event.data = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(context));
        e->triggered = true;
    };
    event.filter.resetFunc = [](KernelEqueueEvent* e) {
        e->triggered = false;
        e->event.data = 0;
    };
    const int result = EqueueAddEvent_nid_postfix(eq, event);
    if (result != EQUEUE_OK) return result;
    std::lock_guard lock(registrationMutex());
    auto& list = registrations();
    if (std::none_of(list.begin(), list.end(), [&](const auto& item) { return item.eq == eq && item.id == id; })) list.push_back({eq, id});
    return 0;
}

int APS5_VABI sceAgcDriverDeleteEqEvent(KernelEqueue eq, int id) {
    {
        std::lock_guard lock(registrationMutex());
        auto& list = registrations();
        const auto erased = std::erase_if(list, [&](const auto& item) { return item.eq == eq && item.id == id; });
        if (erased == 0) return EQUEUE_ERROR_ENOENT;
    }
    const int result = EqueueDeleteEvent_nid_postfix(eq, static_cast<uintptr_t>(id), EVFILT_GRAPHICS_CORE);
    return result == EQUEUE_ERROR_ENOENT ? 0 : result;
}

void AgcDriverTriggerEqEvent_nid_postfix(int id, std::uint32_t context) {
    std::vector<Registration> targets;
    {
        std::lock_guard lock(registrationMutex());
        for (const auto& item : registrations()) if (item.id == id) targets.push_back(item);
    }
    static const bool trace = std::getenv("APS5_TRACE_AGC_RELEASE") != nullptr;
    if (trace) std::fprintf(stderr, "[agc-release] deliver id=0x%x context=0x%x targets=%zu\n", static_cast<unsigned>(id), context, targets.size());
    for (const auto& item : targets) EqueueTriggerEvent_nid_postfix(item.eq, static_cast<uintptr_t>(id), EVFILT_GRAPHICS_CORE, reinterpret_cast<void*>(static_cast<uintptr_t>(context)));
}

}
