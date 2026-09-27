#include "prx/libSceAgc/DcbFlow/include/Dispatch.hpp"

#include "prx/libSceAgc/Command/include/Memory.hpp"
#include "prx/libSceAgc/Command/include/Packet.hpp"
#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

// The arguments are read at the dispatch indirect base plus the offset.
std::uint32_t* APS5_VABI sceAgcDcbDispatchIndirect(CommandBuffer* buf, std::uint32_t dataOffsetInBytes, std::uint32_t flags) {
    Agc::Command::CheckBits(flags, 0xa079u, __func__);
    Agc::Command::Require((dataOffsetInBytes & 3u) == 0, __func__, "misaligned indirect argument offset");
    return Agc::Command::Emit(buf, 0x16u, {dataOffsetInBytes, flags | 0x41u}, __func__);
}

std::uint32_t APS5_VABI sceAgcDcbDispatchIndirectGetSize() {
    return 12;
}

}
