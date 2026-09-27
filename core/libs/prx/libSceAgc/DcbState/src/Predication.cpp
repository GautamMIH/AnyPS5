#include "prx/libSceAgc/DcbState/include/Predication.hpp"

#include "prx/libSceAgc/Command/include/Packet.hpp"
#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

// SET_PREDICATION: condition (bit 8), wait (bit 12) and operation (bits 16-18), then the
// 16-byte-aligned address of the predicate (layout as in KytyPS5).
uint32_t* APS5_VABI sceAgcDcbSetPredication(CommandBuffer* buf, uint8_t condition, uint8_t op, uint8_t wait_op, const volatile void* address, uint32_t count_in_dwords) {
    static_cast<void>(count_in_dwords);
    Agc::Command::Require(condition <= 1 && wait_op <= 1 && op <= 7, __func__, "invalid predication fields");
    const auto value = reinterpret_cast<std::uint64_t>(address);
    const auto flags = (static_cast<std::uint32_t>(condition) << 8u) | (static_cast<std::uint32_t>(wait_op) << 12u) | (static_cast<std::uint32_t>(op) << 16u);
    return Agc::Command::Emit(buf, 0x20u, {flags, static_cast<std::uint32_t>(value) & ~0xfu, static_cast<std::uint32_t>(value >> 32u)}, __func__);
}

std::uint32_t APS5_VABI sceAgcDcbSetZPassPredicationEnableGetSize() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

std::uint32_t APS5_VABI sceAgcDcbSetPredicationDisableGetSize() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

std::uint32_t APS5_VABI sceAgcDcbSetBoolPredicationEnableGetSize() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
