#include "prx/libSceAgc/Command/include/Packet.hpp"
#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

// Rewrites the destination (dwords 2-3) of a WRITE_DATA packet built earlier.
int APS5_VABI sceAgcWriteDataPatchSetAddressOrOffset(std::uint32_t* cmd, std::uint64_t addressOrOffset) {
    Agc::Command::Require(cmd != nullptr, __func__, "null packet");
    Agc::Command::Require(((cmd[0] >> 8u) & 0xffu) == 0x37u, __func__, "packet is not WRITE_DATA");
    cmd[2] = static_cast<std::uint32_t>(addressOrOffset);
    cmd[3] = static_cast<std::uint32_t>(addressOrOffset >> 32u);
    return 0;
}

// AnyPS5 presents a base PS5, so PS5 Pro ("Trinity") paths stay disabled (see sceKernelIsTrinityMode).
void APS5_VABI sceAgcGetIsTrinityMode(std::uint8_t* result) {
    Agc::Command::Require(result != nullptr, __func__, "null result");
    *result = 0;
}

}
