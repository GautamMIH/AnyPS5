#include "prx/libSceAgc/DcbDraw/include/Instancing.hpp"

#include "prx/libSceAgc/Command/include/Packet.hpp"
#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

std::uint32_t* APS5_VABI sceAgcDcbSetNumInstances(CommandBuffer* buf, std::uint32_t numInstances) {
    return Agc::Command::Emit(buf, 0x2fu, {numInstances}, __func__);
}

std::uint32_t APS5_VABI sceAgcDcbSetNumInstancesGetSize() {
    return 8;
}

// SET_BASE with base index 1; header bit 1 holds the shader type, selecting the draw (graphics)
// or dispatch (compute) indirect-argument base (KytyPS5).
std::uint32_t* APS5_VABI sceAgcDcbSetBaseIndirectArgs(CommandBuffer* buf, std::uint32_t shaderType, const volatile void* indirectBaseAddress) {
    Agc::Command::CheckBits(shaderType, 1, __func__);
    const auto address = reinterpret_cast<std::uintptr_t>(indirectBaseAddress);
    Agc::Command::CheckGpuAddress(address, 8, __func__);
    auto* packet = Agc::Command::Allocate(buf, 4, __func__);
    packet[0] = Agc::Command::Header(0x11u, 4, shaderType << 1u);
    packet[1] = 1;
    packet[2] = static_cast<std::uint32_t>(address);
    packet[3] = static_cast<std::uint32_t>(address >> 32u);
    return packet;
}

}
