#include "prx/libSceAgc/Acb/include/Dispatch.hpp"

#include "prx/libSceAgc/Command/include/Memory.hpp"
#include "prx/libSceAgc/Command/include/Packet.hpp"
#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

// Compute queues address the arguments directly (KytyPS5).
std::uint32_t* APS5_VABI sceAgcAcbDispatchIndirect(CommandBuffer* buf, const volatile void* indirectArgs, std::uint32_t modifier) {
    Agc::Command::CheckBits(modifier, 0xa079u, __func__);
    const auto address = reinterpret_cast<std::uintptr_t>(indirectArgs);
    Agc::Command::CheckGpuAddress(address, 4, __func__);
    return Agc::Command::Emit(buf, 0x16u, {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), modifier | 0x41u}, __func__);
}

std::uint32_t APS5_VABI sceAgcAcbDispatchIndirectGetSize() {
    return 16;
}

}
