#include "prx/libSceAgc/Misc/include/ShaderFusion.hpp"

#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libSceAgc/Shader/include/ShaderConstants.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

struct SizeAlign {
 uint64_t m_size;
 size_t m_align;
};

namespace {

using ShaderRegs::ShaderBinaryType;

constexpr int GRAPHICS5_ERROR_INVALID_SHADER_HALVES = static_cast<int>(0x8a6c0008u);

// SH register offsets (gfx10) of the halves' checksums and resource descriptors.
constexpr std::uint32_t SPI_SHADER_PGM_CHKSUM_GS = 0x080u;
constexpr std::uint32_t SPI_SHADER_PGM_RSRC1_GS = 0x08Au;
constexpr std::uint32_t SPI_SHADER_PGM_RSRC2_GS = 0x08Bu;
constexpr std::uint32_t SPI_SHADER_PGM_CHKSUM_HS = 0x100u;
constexpr std::uint32_t SPI_SHADER_PGM_RSRC1_HS = 0x10Au;
constexpr std::uint32_t SPI_SHADER_PGM_RSRC2_HS = 0x10Bu;

bool isHalfPair(const Shader& front, const Shader& back) {
    const auto frontType = static_cast<ShaderBinaryType>(front.type);
    const auto backType = static_cast<ShaderBinaryType>(back.type);
    return (frontType == ShaderBinaryType::GsFront && backType == ShaderBinaryType::GsBack) || (frontType == ShaderBinaryType::HsFront && backType == ShaderBinaryType::HsBack);
}

ShaderRegister* findRegister(ShaderRegister* regs, std::uint32_t count, std::uint32_t offset, std::uint32_t occurrence = 0) {
    if (regs == nullptr) return nullptr;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (regs[i].offset != offset) continue;
        if (occurrence == 0) return regs + i;
        --occurrence;
    }
    return nullptr;
}

void mergeMaxField(ShaderRegister& dst, const ShaderRegister& src, std::uint32_t shift, std::uint32_t mask) {
    const auto field = std::max((dst.value >> shift) & mask, (src.value >> shift) & mask);
    dst.value = (dst.value & ~(mask << shift)) | (field << shift);
}

// Fuses the front (ES or LS) and back (GS or HS) halves of a merged shader stage into one
// shader, as KytyPS5 does: the back half's registers with the front half's checksums and code
// address, and resource fields large enough for both halves.
int fuseShaderHalves(Shader* fused, const Shader* front, const Shader* back, void* scratch, bool recomputeSharedVgprs) {
    if (fused == nullptr || front == nullptr || back == nullptr) throw std::runtime_error("sceAgcFuseShaderHalves: null argument");
    if (!isHalfPair(*front, *back)) return GRAPHICS5_ERROR_INVALID_SHADER_HALVES;
    const auto isGs = static_cast<ShaderBinaryType>(front->type) == ShaderBinaryType::GsFront;
    // VGT_SHADER_STAGES_EN must agree on the stage the halves form (GS_EN or HS_EN).
    const auto stageBit = isGs ? (1u << 22u) : (1u << 21u);
    if (front->specials == nullptr || back->specials == nullptr || ((front->specials->vgt_shader_stages_en.value ^ back->specials->vgt_shader_stages_en.value) & stageBit) != 0) {
        return GRAPHICS5_ERROR_INVALID_SHADER_HALVES;
    }

    *fused = *back;
    fused->type = static_cast<std::uint8_t>(isGs ? ShaderBinaryType::Gs : ShaderBinaryType::Hs);
    if (scratch != nullptr) {
        std::memcpy(scratch, back->sh_registers, static_cast<std::size_t>(back->num_sh_registers) * sizeof(ShaderRegister));
        fused->sh_registers = static_cast<ShaderRegister*>(scratch);
    }
    auto* regs = fused->sh_registers;
    const std::uint32_t count = fused->num_sh_registers;
    const std::uint32_t frontCount = front->num_sh_registers;

    const auto checksum = isGs ? SPI_SHADER_PGM_CHKSUM_GS : SPI_SHADER_PGM_CHKSUM_HS;
    for (std::uint32_t occurrence = 0; occurrence < 2; ++occurrence) {
        const auto* source = findRegister(front->sh_registers, frontCount, checksum, occurrence);
        auto* target = findRegister(regs, count, checksum, occurrence);
        if (source != nullptr && target != nullptr) target->value = source->value;
    }

    const auto* frontRsrc1 = findRegister(front->sh_registers, frontCount, isGs ? SPI_SHADER_PGM_RSRC1_GS : SPI_SHADER_PGM_RSRC1_HS);
    const auto* frontRsrc2 = findRegister(front->sh_registers, frontCount, isGs ? SPI_SHADER_PGM_RSRC2_GS : SPI_SHADER_PGM_RSRC2_HS);
    auto* rsrc1 = findRegister(regs, count, isGs ? SPI_SHADER_PGM_RSRC1_GS : SPI_SHADER_PGM_RSRC1_HS);
    auto* rsrc2 = findRegister(regs, count, isGs ? SPI_SHADER_PGM_RSRC2_GS : SPI_SHADER_PGM_RSRC2_HS);
    if (frontRsrc1 == nullptr || frontRsrc2 == nullptr || rsrc1 == nullptr || rsrc2 == nullptr) {
        throw std::runtime_error("sceAgcFuseShaderHalves: shader half lacks SPI_SHADER_PGM_RSRC1/2");
    }

    if (recomputeSharedVgprs) {
        // Shared VGPRs (RSRC2 bits 28-31, in blocks of 8) are reallocated for the larger half.
        const auto frontVgprs = ((frontRsrc1->value & 0x3fu) + 1u) * 4u;
        const auto backVgprs = ((rsrc1->value & 0x3fu) + 1u) * 4u;
        const auto frontTotal = frontVgprs + (frontRsrc2->value >> 28u) * 8u;
        const auto backTotal = backVgprs + (rsrc2->value >> 28u) * 8u;
        const auto maxTotal = std::max(frontTotal, backTotal);
        const auto shared = std::max(frontVgprs, backVgprs) >= maxTotal ? 0u : (maxTotal - std::min(frontTotal, backTotal) + 7u) / 64u;
        rsrc2->value = (rsrc2->value & 0x0fffffffu) | ((shared & 0xfu) << 28u);
    } else {
        mergeMaxField(*rsrc2, *frontRsrc2, 28, 0x0fu);
    }
    mergeMaxField(*rsrc1, *frontRsrc1, 0, 0x3fu);
    if (isGs) {
        mergeMaxField(*rsrc1, *frontRsrc1, 29, 0x03u);
        mergeMaxField(*rsrc2, *frontRsrc2, 16, 0x03u);
        rsrc2->value = (rsrc2->value & 0xfffbffffu) | (frontRsrc2->value & 0x00040000u);
    } else {
        mergeMaxField(*rsrc1, *frontRsrc1, 28, 0x03u);
    }
    rsrc2->value = (rsrc2->value & 0xf7ffffc1u) | (frontRsrc2->value & 0x0800003eu);

    // The fused stage starts at the front half's code.
    auto* lo = findRegister(regs, count, isGs ? ShaderRegs::SPI_SHADER_PGM_LO_ES : ShaderRegs::SPI_SHADER_PGM_LO_LS);
    if (lo != nullptr && lo + 1 < regs + count && (lo + 1)->offset == lo->offset + 1u) {
        const auto address = reinterpret_cast<std::uint64_t>(front->code);
        lo->value = static_cast<std::uint32_t>(address >> 8u);
        (lo + 1)->value = ((lo + 1)->value & 0xffffff00u) | static_cast<std::uint32_t>((address >> 40u) & 0xffu);
    }
    fused->user_data = recomputeSharedVgprs ? nullptr : front->user_data;
    return 0;
}

}

extern "C" {

APS5_EXPORT("fd5Bp5tGTgo", sceAgcUnknownFuseShaderHalves);
int APS5_VABI sceAgcUnknownFuseShaderHalves(Shader* fused_result, const Shader* front, const Shader* back, void* scratch_mem) {
    return fuseShaderHalves(fused_result, front, back, scratch_mem, true);
}

// Older export of the same operation: shared VGPRs take the larger half's count.
int APS5_VABI AgcFuseShaderHalvesKeepSharedVgprs(Shader* fused_result, const Shader* front, const Shader* back, void* scratch_mem) {
    return fuseShaderHalves(fused_result, front, back, scratch_mem, false);
}
APS5_EXPORT("NApJjpKNBl4", AgcFuseShaderHalvesKeepSharedVgprs);

APS5_EXPORT("dolOmWH+huQ", sceAgcUnknownGetFusedShaderSize);
int APS5_VABI sceAgcUnknownGetFusedShaderSize(SizeAlign* dst, const Shader* front, const Shader* back) {
    if (dst == nullptr || front == nullptr || back == nullptr) throw std::runtime_error("sceAgcUnknownGetFusedShaderSize: null argument");
    if (!isHalfPair(*front, *back)) return GRAPHICS5_ERROR_INVALID_SHADER_HALVES;
    // Scratch for the fused shader's copy of the back half's SH registers.
    dst->m_size = static_cast<std::uint64_t>(back->num_sh_registers) * sizeof(ShaderRegister);
    dst->m_align = 4;
    return 0;
}

APS5_EXPORT("k0E7vkgqAuE", sceAgcCreateInterpolantMappingVsPs);
int APS5_VABI sceAgcCreateInterpolantMappingVsPs(ShaderRegister* regs, const Shader* vs, const Shader* ps) {
    (void)regs;
    (void)vs;
    (void)ps;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
