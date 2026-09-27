#ifndef CORE_LIBS_PRX_LIBSCEAGC_DCBSTATE_INCLUDE_MARKER_HPP
#define CORE_LIBS_PRX_LIBSCEAGC_DCBSTATE_INCLUDE_MARKER_HPP

#include <cstdint>
#include "SceTypes.hpp"

extern "C" {

std::uint32_t* APS5_VABI sceAgcDcbSetMarker(CommandBuffer* buf, const char* str, std::uint32_t color);
std::uint32_t* APS5_VABI sceAgcDcbPopMarker(CommandBuffer* buf);
std::uint32_t* APS5_VABI sceAgcDcbPushMarker(CommandBuffer* buf, const char* str, std::uint32_t color);

}

#endif
