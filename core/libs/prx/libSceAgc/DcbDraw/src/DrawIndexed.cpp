#include "prx/libSceAgc/Command/include/Draw.hpp"
#include "prx/libSceAgc/DcbDraw/include/DrawIndexed.hpp"

#include "prx/libSceAgc/Command/include/Packet.hpp"
#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

// DRAW_INDEX_2: max index count, index buffer address, index count, initiator (as in KytyPS5).
uint32_t* APS5_VABI sceAgcDcbDrawIndex(CommandBuffer* buf, uint32_t index_count, const volatile void* index_addr, uint64_t modifier) {
    const auto address = reinterpret_cast<std::uint64_t>(index_addr);
    Agc::Command::Require(address != 0 && (address & 1u) == 0, __func__, "null or misaligned index buffer");
    return Agc::Command::Emit(buf, 0x27u, {index_count == 0 ? 1u : index_count, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), index_count, Agc::Command::DrawInitiator(modifier, true, __func__)}, __func__);
}

std::uint32_t APS5_VABI sceAgcDcbDrawIndexGetSize() {
    return 24;
}

std::uint32_t* APS5_VABI sceAgcDcbDrawIndexAuto(CommandBuffer* buf, std::uint32_t indexCount, std::uint64_t modifier) {
    return Agc::Command::Emit(buf, 0x2du, {indexCount, Agc::Command::DrawInitiator(modifier, false, __func__)}, __func__);
}

std::uint32_t APS5_VABI sceAgcDcbDrawIndexAutoGetSize() {
    return 12;
}

std::uint32_t* APS5_VABI sceAgcDcbDrawIndexOffset(CommandBuffer* buf, std::uint32_t indexOffset, std::uint32_t indexCount, std::uint64_t modifier) {
    return Agc::Command::Emit(buf, 0x35u, {indexCount == 0 ? 1u : indexCount, indexOffset, indexCount, Agc::Command::DrawInitiator(modifier, true, __func__)}, __func__);
}

uint32_t APS5_VABI sceAgcDcbDrawIndexOffsetGetSize(void) {
    return 20;
}

std::uint32_t* APS5_VABI sceAgcDcbDrawIndexIndirect(CommandBuffer* buf, std::uint32_t dataOffsetInBytes, std::uint64_t modifier) {
    Agc::Command::Require((dataOffsetInBytes & 3u) == 0, __func__, "misaligned indirect argument offset");
    const auto offsets = Agc::Command::DrawIndexedPatchOffsets(modifier, __func__);
    return Agc::Command::Emit(buf, 0x25u, {dataOffsetInBytes, static_cast<std::uint32_t>(offsets), static_cast<std::uint32_t>(offsets >> 32u), Agc::Command::DrawInitiator(modifier, true, __func__)}, __func__);
}

std::uint32_t APS5_VABI sceAgcDcbDrawIndexIndirectGetSize() {
    return 20;
}

// DW4 holds the count-from-memory flag (bit 30) and the draw-index SGPR location (0x280: none).
std::uint32_t* APS5_VABI sceAgcDcbDrawIndexIndirectMulti(CommandBuffer* buf, std::uint32_t dataOffsetInBytes, std::uint32_t countIndirect, std::uint32_t maxCountOrCount, const volatile void* countAddress, std::uint32_t strideInBytes, std::uint64_t modifier) {
    Agc::Command::CheckBits(countIndirect, 1, __func__);
    Agc::Command::Require((dataOffsetInBytes & 3u) == 0 && (strideInBytes & 3u) == 0 && strideInBytes >= 20, __func__, "invalid indirect draw offset or stride");
    const auto address = reinterpret_cast<std::uintptr_t>(countAddress);
    if (countIndirect != 0) {
        Agc::Command::CheckGpuAddress(address, 4, __func__);
    } else {
        Agc::Command::Require(address == 0, __func__, "count address supplied for a direct draw count");
    }
    const auto offsets = Agc::Command::DrawIndexedPatchOffsets(modifier, __func__);
    return Agc::Command::Emit(buf, 0x38u, {dataOffsetInBytes, static_cast<std::uint32_t>(offsets), static_cast<std::uint32_t>(offsets >> 32u), (countIndirect << 30u) | Agc::Command::DrawIndexLocation(modifier), maxCountOrCount, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), strideInBytes, Agc::Command::DrawInitiator(modifier, true, __func__)}, __func__);
}

std::uint32_t APS5_VABI sceAgcDcbDrawIndexIndirectMultiGetSize() {
    return 40;
}

uint32_t* APS5_VABI sceAgcDcbDrawIndexMultiInstanced(CommandBuffer* buf, uint32_t index_count, const volatile void* index_addr, const volatile void* object_ids, uint32_t instance_count, uint64_t modifier) {
 (void)buf;
 (void)index_count;
 (void)index_addr;
 (void)object_ids;
 (void)instance_count;
 (void)modifier;
 NotImplemented_nid_no_patch(__func__);
 return nullptr;
}

uint32_t APS5_VABI sceAgcDcbDrawIndexMultiInstancedGetSize(void) {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

}
