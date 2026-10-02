#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EQ_INCLUDE_EVENT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EQ_INCLUDE_EVENT_HPP

#include "SceTypes.hpp"
#include <cstdint>

extern "C" int APS5_VABI sceAgcDriverAddEqEvent(KernelEqueue eq, int id, void* udata);
extern "C" int APS5_VABI sceAgcDriverDeleteEqEvent(KernelEqueue eq, int id);

inline constexpr std::int16_t EVFILT_GRAPHICS_CORE = -14;

// Delivers GPU event `id` (the submitting queue: 0 for graphics, 0x20 + pipe * 8 + queue for compute)
// to every event queue registered through sceAgcDriverAddEqEvent; `context` becomes the event data.
extern "C" void AgcDriverTriggerEqEvent_nid_postfix(int id, std::uint32_t context);

#endif
