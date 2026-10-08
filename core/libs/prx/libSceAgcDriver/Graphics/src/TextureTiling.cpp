#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace AgcDriver::Graphics {

namespace {

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1u) / alignment * alignment;
}

std::uint32_t ShiftCeil(std::uint32_t value, std::uint32_t shift) {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(value) + (1ull << shift) - 1ull) >> shift);
}

std::uint32_t CalcLinearBlockWidth(std::uint32_t bytesPerElement) {
    return 256u / bytesPerElement;
}

struct BlockLayout {
    std::uint32_t blockSize;
    std::uint32_t blockWidth;
    std::uint32_t blockHeight;
};

struct Log2BlockDimensions {
    std::uint8_t width;
    std::uint8_t height;
};

constexpr Log2BlockDimensions kLog2BlockThin256B[] = {{4, 4}, {4, 3}, {3, 3}, {3, 2}, {2, 2}};
constexpr Log2BlockDimensions kLog2BlockThin4KB[] = {{6, 6}, {6, 5}, {5, 5}, {5, 4}, {4, 4}};
constexpr Log2BlockDimensions kLog2BlockThin64KB[] = {{8, 8}, {8, 7}, {7, 7}, {7, 6}, {6, 6}};

BlockLayout GetBlockLayout(TextureTileMode tileMode, std::uint32_t bytesPerElement) {
    Require(std::has_single_bit(bytesPerElement) && bytesPerElement <= 16u, "unsupported bytes per element for tiled texture geometry");
    const auto index = static_cast<std::size_t>(std::countr_zero(bytesPerElement));
    switch (tileMode) {
        case TextureTileMode::kLinear: throw std::runtime_error("AGC graphics: GetBlockLayout does not apply to linear tiling");
        case TextureTileMode::kStandard256B:
        case TextureTileMode::kD256B: return {256u, 1u << kLog2BlockThin256B[index].width, 1u << kLog2BlockThin256B[index].height};
        case TextureTileMode::kStandard4KB:
        case TextureTileMode::kD4KB:
        case TextureTileMode::kS4KBX:
        case TextureTileMode::kD4KBX: return {4096u, 1u << kLog2BlockThin4KB[index].width, 1u << kLog2BlockThin4KB[index].height};
        case TextureTileMode::RenderTarget64KB:
        case TextureTileMode::Depth64KB:
        case TextureTileMode::kStandard64KB:
        case TextureTileMode::kD64KB:
        case TextureTileMode::kS64KBT:
        case TextureTileMode::kD64KBT:
        case TextureTileMode::kS64KBX:
        case TextureTileMode::kD64KBX: return {65536u, 1u << kLog2BlockThin64KB[index].width, 1u << kLog2BlockThin64KB[index].height};
    }
    throw std::runtime_error("AGC graphics: GetBlockLayout encountered an unknown tile mode");
}

struct MipTailLocation {
    std::uint32_t x;
    std::uint32_t y;
};

constexpr MipTailLocation kMipTailThin4KB[5][8] = {
    {{32, 0}, {16, 32}, {0, 48}, {0, 32}, {16, 16}, {16, 0}, {0, 16}, {0, 0}},
    {{32, 0}, {16, 16}, {0, 24}, {0, 16}, {16, 8}, {16, 0}, {0, 8}, {0, 0}},
    {{16, 0}, {8, 16}, {0, 24}, {0, 16}, {8, 8}, {8, 0}, {0, 8}, {0, 0}},
    {{16, 0}, {8, 8}, {0, 12}, {0, 8}, {8, 4}, {8, 0}, {0, 4}, {0, 0}},
    {{8, 0}, {4, 8}, {0, 12}, {0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}},
};

constexpr MipTailLocation kMipTailThin64KB[5][12] = {
    {{128, 0}, {0, 128}, {64, 0}, {0, 64}, {32, 0}, {16, 32}, {0, 48}, {0, 32}, {16, 16}, {16, 0}, {0, 16}, {0, 0}},
    {{128, 0}, {0, 64}, {64, 0}, {0, 32}, {32, 0}, {16, 16}, {0, 24}, {0, 16}, {16, 8}, {16, 0}, {0, 8}, {0, 0}},
    {{64, 0}, {0, 64}, {32, 0}, {0, 32}, {16, 0}, {8, 16}, {0, 24}, {0, 16}, {8, 8}, {8, 0}, {0, 8}, {0, 0}},
    {{64, 0}, {0, 32}, {32, 0}, {0, 16}, {16, 0}, {8, 8}, {0, 12}, {0, 8}, {8, 4}, {8, 0}, {0, 4}, {0, 0}},
    {{32, 0}, {0, 32}, {16, 0}, {0, 16}, {8, 0}, {4, 8}, {0, 12}, {0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}},
};

struct MipTailLayout {
    const MipTailLocation* locations;
    std::uint32_t maxLevels;
    std::uint32_t widthLimit;
    std::uint32_t heightLimit;
};

template<std::size_t TLevels>
MipTailLayout MakeMipTailLayout(const MipTailLocation (&locations)[TLevels], std::uint32_t widthLimit, std::uint32_t heightLimit) {
    return {locations, static_cast<std::uint32_t>(TLevels), widthLimit, heightLimit};
}

bool GetMipTailLayout(TextureTileMode tileMode, const BlockLayout& block, std::uint32_t bytesPerElement, MipTailLayout& out) {
    const auto index = static_cast<std::size_t>(std::countr_zero(bytesPerElement));
    switch (tileMode) {
        case TextureTileMode::kLinear:
        case TextureTileMode::kStandard256B:
        case TextureTileMode::kD256B:
        // Depth surfaces are padded to whole blocks with no mip tail (single level, as the DB writes them).
        case TextureTileMode::Depth64KB: return false;
        case TextureTileMode::kStandard4KB:
        case TextureTileMode::kD4KB:
        case TextureTileMode::kS4KBX:
        case TextureTileMode::kD4KBX:
            out = MakeMipTailLayout(kMipTailThin4KB[index], block.blockWidth >> 1u, block.blockHeight);
            return true;
        case TextureTileMode::RenderTarget64KB:
        case TextureTileMode::kStandard64KB:
        case TextureTileMode::kD64KB:
        case TextureTileMode::kS64KBT:
        case TextureTileMode::kD64KBT:
        case TextureTileMode::kS64KBX:
        case TextureTileMode::kD64KBX:
            out = MakeMipTailLayout(kMipTailThin64KB[index], block.blockWidth >> 1u, block.blockHeight);
            return true;
    }
    throw std::runtime_error("AGC graphics: GetMipTailLayout encountered an unknown tile mode");
}

    constexpr TextureSwizzleEquation kTextureSwizzleEquations[] = {
        {24u, 1u, {0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00000004u, 0x00004000u, 0x00000008u, 0x00010000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
        {24u, 2u, {0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00000004u, 0x00004000u, 0x00000008u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00010000u, 0x00000040u, 0x00040000u, 0x00000080u}},
        {24u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00000004u, 0x00004000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00008000u, 0x00000010u, 0x00040000u, 0x00000040u}},
        {24u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00000004u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00004000u, 0x00000008u, 0x00010000u, 0x00000040u}},
        {24u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00004000u, 0x00000004u, 0x00008000u, 0x00000010u}},
        {25u, 1u, {0x00000001u, 0x00000002u, 0x00000004u, 0x00000008u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00008000u, 0x08010080u, 0x04080010u, 0x02020040u, 0x01040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
        {25u, 2u, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x08008080u, 0x04040010u, 0x02010040u, 0x01020020u, 0x00020000u, 0x00000040u, 0x00040000u, 0x00000080u}},
        {25u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x08008040u, 0x04040008u, 0x02010020u, 0x01020010u, 0x00020000u, 0x00000020u, 0x00040000u, 0x00000040u}},
        {25u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00002000u, 0x00000002u, 0x00000004u, 0x08004040u, 0x04020008u, 0x02008020u, 0x01010010u, 0x00010000u, 0x00000020u, 0x00020000u, 0x00000040u}},
        {25u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00001000u, 0x00002000u, 0x00000001u, 0x00000002u, 0x08004020u, 0x04020004u, 0x02008010u, 0x01010008u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u}},
        {26u, 1u, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x08010080u, 0x04080010u, 0x02020040u, 0x01040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
        {26u, 2u, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x08008080u, 0x04040010u, 0x02010040u, 0x01020020u, 0x00020000u, 0x00000040u, 0x00040000u, 0x00000080u}},
        {26u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x08008040u, 0x04040008u, 0x02010020u, 0x01020010u, 0x00020000u, 0x00000020u, 0x00040000u, 0x00000040u}},
        {26u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x08004040u, 0x04020008u, 0x02008020u, 0x01010010u, 0x00010000u, 0x00000020u, 0x00020000u, 0x00000040u}},
        {26u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x08004020u, 0x04020004u, 0x02008010u, 0x01010008u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u}},
        {27u, 1u, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00010000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
        {27u, 2u, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00010000u, 0x00000040u, 0x00040000u, 0x00000080u}},
        {27u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00008000u, 0x00000010u, 0x00040000u, 0x00000040u}},
        {27u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00004000u, 0x00000008u, 0x00010000u, 0x00000040u}},
        {27u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00004000u, 0x00000004u, 0x00008000u, 0x00000010u}},
        {2u, 1u, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {2u, 2u, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {2u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {2u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {2u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {6u, 1u, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {6u, 2u, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00000010u, 0x00010000u, 0x00000020u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {6u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x00008000u, 0x00000008u, 0x00010000u, 0x00000010u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {6u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00000010u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {6u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00004000u, 0x00000004u, 0x00008000u, 0x00000008u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {10u, 1u, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
        {10u, 2u, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00000010u, 0x00010000u, 0x00000020u, 0x00020000u, 0x00000040u, 0x00040000u, 0x00000080u}},
        {10u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x00008000u, 0x00000008u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u, 0x00040000u, 0x00000040u}},
        {10u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00000010u, 0x00010000u, 0x00000020u, 0x00020000u, 0x00000040u}},
        {10u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00004000u, 0x00000004u, 0x00008000u, 0x00000008u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u}},
        {17u, 1u, {0x00000001u, 0x00000002u, 0x00000004u, 0x00000008u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00008000u, 0x00010080u, 0x00080010u, 0x00020040u, 0x00040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
        {17u, 2u, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x00008080u, 0x00040010u, 0x00010040u, 0x00020020u, 0x00020000u, 0x00000040u, 0x00040000u, 0x00000080u}},
        {17u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x00008040u, 0x00040008u, 0x00010020u, 0x00020010u, 0x00020000u, 0x00000020u, 0x00040000u, 0x00000040u}},
        {17u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00002000u, 0x00000002u, 0x00000004u, 0x00004040u, 0x00020008u, 0x00008020u, 0x00010010u, 0x00010000u, 0x00000020u, 0x00020000u, 0x00000040u}},
        {17u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00001000u, 0x00002000u, 0x00000001u, 0x00000002u, 0x00004020u, 0x00020004u, 0x00008010u, 0x00010008u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u}},
        {18u, 1u, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00010080u, 0x00080010u, 0x00020040u, 0x00040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
        {18u, 2u, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x00008080u, 0x00040010u, 0x00010040u, 0x00020020u, 0x00020000u, 0x00000040u, 0x00040000u, 0x00000080u}},
        {18u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x00008040u, 0x00040008u, 0x00010020u, 0x00020010u, 0x00020000u, 0x00000020u, 0x00040000u, 0x00000040u}},
        {18u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00004040u, 0x00020008u, 0x00008020u, 0x00010010u, 0x00010000u, 0x00000020u, 0x00020000u, 0x00000040u}},
        {18u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00004020u, 0x00020004u, 0x00008010u, 0x00010008u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u}},
        {21u, 1u, {0x00000001u, 0x00000002u, 0x00000004u, 0x00000008u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00008000u, 0x08010080u, 0x04080010u, 0x02020040u, 0x01040020u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {21u, 2u, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x08008080u, 0x04040010u, 0x02010040u, 0x01020020u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {21u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x08008040u, 0x04040008u, 0x02010020u, 0x01020010u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {21u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00002000u, 0x00000002u, 0x00000004u, 0x08004040u, 0x04020008u, 0x02008020u, 0x01010010u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {21u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00001000u, 0x00002000u, 0x00000001u, 0x00000002u, 0x08004020u, 0x04020004u, 0x02008010u, 0x01010008u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {22u, 1u, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x08010080u, 0x04080010u, 0x02020040u, 0x01040020u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {22u, 2u, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x08008080u, 0x04040010u, 0x02010040u, 0x01020020u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {22u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x08008040u, 0x04040008u, 0x02010020u, 0x01020010u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {22u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x08004040u, 0x04020008u, 0x02008020u, 0x01010010u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {22u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x08004020u, 0x04020004u, 0x02008010u, 0x01010008u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        // Thick (3D) standard swizzles SW_4KB_S, SW_64KB_S and SW_64KB_S_X use the S3 patterns, keyed 0x100 | SW_MODE.
        {0x105u, 1u, {0x00000001u, 0x00000002u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00002000u, 0x00000004u, 0x04000000u, 0x00004000u, 0x00000008u, 0x08000000u, 0x00008000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {0x105u, 2u, {0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00002000u, 0x00000002u, 0x04000000u, 0x00004000u, 0x00000004u, 0x08000000u, 0x00008000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {0x105u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x01000000u, 0x00002000u, 0x00000002u, 0x02000000u, 0x00004000u, 0x00000004u, 0x04000000u, 0x00008000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {0x105u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x00000002u, 0x02000000u, 0x00002000u, 0x00000004u, 0x04000000u, 0x00004000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {0x105u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01000000u, 0x00001000u, 0x00000001u, 0x02000000u, 0x00002000u, 0x00000002u, 0x04000000u, 0x00004000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {0x109u, 1u, {0x00000001u, 0x00000002u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00002000u, 0x00000004u, 0x04000000u, 0x00004000u, 0x00000008u, 0x08000000u, 0x00008000u, 0x00000010u, 0x10000000u, 0x00010000u, 0x00000020u}},
        {0x109u, 2u, {0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00002000u, 0x00000002u, 0x04000000u, 0x00004000u, 0x00000004u, 0x08000000u, 0x00008000u, 0x00000008u, 0x10000000u, 0x00010000u, 0x00000010u}},
        {0x109u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x01000000u, 0x00002000u, 0x00000002u, 0x02000000u, 0x00004000u, 0x00000004u, 0x04000000u, 0x00008000u, 0x00000008u, 0x08000000u, 0x00010000u, 0x00000010u}},
        {0x109u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x00000002u, 0x02000000u, 0x00002000u, 0x00000004u, 0x04000000u, 0x00004000u, 0x00000008u, 0x08000000u, 0x00008000u, 0x00000010u}},
        {0x109u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01000000u, 0x00001000u, 0x00000001u, 0x02000000u, 0x00002000u, 0x00000002u, 0x04000000u, 0x00004000u, 0x00000004u, 0x08000000u, 0x00008000u, 0x00000008u}},
        {0x119u, 1u, {0x00000001u, 0x00000002u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00002000u, 0x00000004u, 0x04000000u, 0x40004040u, 0x20020008u, 0x08010020u, 0x10008010u, 0x00000010u, 0x10000000u, 0x00010000u, 0x00000020u}},
        {0x119u, 2u, {0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00002000u, 0x00000002u, 0x04000000u, 0x40004020u, 0x20020004u, 0x08010010u, 0x10008008u, 0x00000008u, 0x10000000u, 0x00010000u, 0x00000010u}},
        {0x119u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x01000000u, 0x00002000u, 0x00000002u, 0x02000000u, 0x20004020u, 0x10020004u, 0x04010010u, 0x08008008u, 0x00000008u, 0x08000000u, 0x00010000u, 0x00000010u}},
        {0x119u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x00000002u, 0x02000000u, 0x20002020u, 0x10010004u, 0x04008010u, 0x08004008u, 0x00000008u, 0x08000000u, 0x00008000u, 0x00000010u}},
        {0x119u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01000000u, 0x00001000u, 0x00000001u, 0x02000000u, 0x20002010u, 0x10010002u, 0x04008008u, 0x08004004u, 0x00000004u, 0x08000000u, 0x00008000u, 0x00000008u}},
    };

std::uint32_t TexelLevelDimension(std::uint32_t guestDimension, std::uint32_t level, std::uint32_t texelScale) {
    return std::max((std::max(guestDimension >> level, 1u) + texelScale - 1u) / texelScale, 1u);
}

std::vector<TileMipLayout> ComputeLinearMipLayout(std::uint32_t bytesPerElement, std::uint32_t texelWidth, std::uint32_t texelHeight, std::uint32_t width, std::uint32_t height, std::uint32_t mipCount) {
    const auto compressed = texelWidth != 1u || texelHeight != 1u;
    const auto elementsWidth0 = (width + texelWidth - 1u) / texelWidth;
    const auto elementsHeight0 = (height + texelHeight - 1u) / texelHeight;
    const auto blockWidth = CalcLinearBlockWidth(bytesPerElement);

    std::vector<TileMipLayout> mips(mipCount);
    std::uint64_t offset = 0;
    for (auto level = mipCount; level-- > 0;) {
        const auto elementsLevelWidth = std::max(ShiftCeil(elementsWidth0, level), 1u);
        const auto elementsLevelHeight = std::max(ShiftCeil(elementsHeight0, level), 1u);
        const auto paddedElementsWidth = AlignUp(elementsLevelWidth, blockWidth);
        const auto size = static_cast<std::uint64_t>(paddedElementsWidth) * elementsLevelHeight * bytesPerElement;
        Require(size != 0, "computed a zero-sized linear texture mip level");

        auto pitchBytes = paddedElementsWidth * bytesPerElement;
        if (compressed) pitchBytes = std::max(pitchBytes, 32u);

        auto& mip = mips[level];
        mip.tiledOffset = offset;
        mip.tiledSize = size;
        mip.linearOffset = offset;
        mip.linearSize = size;
        mip.width = TexelLevelDimension(width, level, texelWidth);
        mip.height = TexelLevelDimension(height, level, texelHeight);
        mip.blocksPerRow = paddedElementsWidth;
        mip.pitchBytes = pitchBytes;
        mip.tail = false;
        mip.tailX = 0;
        mip.tailY = 0;
        offset += size;
    }
    return mips;
}

std::vector<TileMipLayout> ComputeTiledMipLayout(TextureTileMode tileMode, std::uint32_t bytesPerElement, std::uint32_t texelWidth, std::uint32_t texelHeight, std::uint32_t width, std::uint32_t height, std::uint32_t mipCount) {
    const auto block = GetBlockLayout(tileMode, bytesPerElement);
    const auto elementsWidth0 = (width + texelWidth - 1u) / texelWidth;
    const auto elementsHeight0 = (height + texelHeight - 1u) / texelHeight;

    MipTailLayout tail{};
    const auto hasTail = GetMipTailLayout(tileMode, block, bytesPerElement, tail);

    auto firstTailLevel = mipCount;
    if (hasTail && mipCount > 1) {
        for (std::uint32_t level = 0; level < mipCount; ++level) {
            if (ShiftCeil(elementsWidth0, level) <= tail.widthLimit && ShiftCeil(elementsHeight0, level) <= tail.heightLimit && mipCount - level <= tail.maxLevels) {
                firstTailLevel = level;
                break;
            }
        }
    }

    std::vector<TileMipLayout> mips(mipCount);
    std::uint64_t blockSliceSize = 0;

    for (std::uint32_t level = 0; level < firstTailLevel; ++level) {
        auto& mip = mips[level];
        mip.width = TexelLevelDimension(width, level, texelWidth);
        mip.height = TexelLevelDimension(height, level, texelHeight);
        const auto paddedWidth = AlignUp(std::max(ShiftCeil(elementsWidth0, level), 1u), block.blockWidth);
        const auto paddedHeight = AlignUp(std::max(ShiftCeil(elementsHeight0, level), 1u), block.blockHeight);
        mip.blocksPerRow = paddedWidth / block.blockWidth;
        mip.tiledSize = static_cast<std::uint64_t>(paddedWidth) * paddedHeight * bytesPerElement;
        mip.linearSize = static_cast<std::uint64_t>(mip.width) * mip.height * bytesPerElement;
        mip.tail = false;
        mip.tailX = 0;
        mip.tailY = 0;
        blockSliceSize += mip.tiledSize;
    }

    if (firstTailLevel < mipCount) blockSliceSize += block.blockSize;

    for (auto level = firstTailLevel; level < mipCount; ++level) {
        auto& mip = mips[level];
        mip.width = TexelLevelDimension(width, level, texelWidth);
        mip.height = TexelLevelDimension(height, level, texelHeight);
        mip.blocksPerRow = 1u;
        mip.tiledSize = block.blockSize;
        mip.linearSize = static_cast<std::uint64_t>(mip.width) * mip.height * bytesPerElement;
        mip.tail = true;
        mip.tailX = tail.locations[level - firstTailLevel].x;
        mip.tailY = tail.locations[level - firstTailLevel].y;
    }

    std::uint64_t offset = firstTailLevel < mipCount ? block.blockSize : 0;
    for (auto level = firstTailLevel; level-- > 0;) {
        mips[level].tiledOffset = offset;
        offset += mips[level].tiledSize;
    }
    for (auto level = firstTailLevel; level < mipCount; ++level) {
        mips[level].tiledOffset = 0;
    }

    Require(offset == blockSliceSize, "tiled texture mip chain geometry is inconsistent");
    for (const auto& mip : mips) {
        Require(mip.width != 0 && mip.height != 0, "computed a zero-sized tiled texture mip level");
        Require(mip.tiledSize != 0 && mip.linearSize != 0, "computed a zero-sized tiled texture mip level");
    }
    std::uint64_t linearOffset = 0;
    for (auto& mip : mips) {
        const auto alignment = std::max(bytesPerElement, 4u);
        linearOffset = (linearOffset + alignment - 1u) / alignment * alignment;
        mip.linearOffset = linearOffset;
        mip.pitchBytes = mip.width * bytesPerElement;
        mip.linearSize = (static_cast<std::uint64_t>(mip.pitchBytes) * mip.height + alignment - 1u) / alignment * alignment;
        linearOffset += mip.linearSize;
    }
    return mips;
}

}

std::vector<TileMipLayout> ComputeMipLayout(TextureTileMode tileMode, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t mipCount) {
    Require(width != 0 && height != 0, "cannot compute mip layout for a zero-sized texture");
    Require(mipCount != 0 && mipCount <= 16u, "texture mip count is out of range");

    const auto bytesPerElement = BytesPerElement(format);
    const auto texelWidth = BlockWidth(format);
    const auto texelHeight = BlockHeight(format);

    if (tileMode == TextureTileMode::RenderTarget64KB) {
        Require(!IsBlockCompressed(format), "render target tiling does not support block compressed formats");
        Require(format != 128 && format != 129 && format != 132, "texture format does not support render target tiling");
    }
    if (tileMode == TextureTileMode::Depth64KB) {
        Require(bytesPerElement == 1 || bytesPerElement == 2 || bytesPerElement == 4, "depth tiling requires 8-, 16- or 32-bit elements");
        Require(!IsBlockCompressed(format) && mipCount == 1, "depth tiling supports single-level uncompressed textures only");
    }
    if (tileMode == TextureTileMode::kLinear) return ComputeLinearMipLayout(bytesPerElement, texelWidth, texelHeight, width, height, mipCount);
    return ComputeTiledMipLayout(tileMode, bytesPerElement, texelWidth, texelHeight, width, height, mipCount);
}

std::vector<TileMipLayout> ComputeElementMipLayout(TextureTileMode tileMode, std::uint32_t elementBytes, std::uint32_t width, std::uint32_t height, std::uint32_t mipCount) {
    Require(width != 0 && height != 0, "cannot compute mip layout for a zero-sized surface");
    Require(mipCount != 0 && mipCount <= 16u, "surface mip count is out of range");
    Require(tileMode != TextureTileMode::kLinear, "element mip layouts are for tiled surfaces");
    return ComputeTiledMipLayout(tileMode, elementBytes, 1u, 1u, width, height, mipCount);
}

std::uint64_t ComputeSurfaceSize(const std::vector<TileMipLayout>& mips, std::uint32_t arrayLayers) {
    Require(!mips.empty(), "cannot compute surface size for an empty mip chain");
    Require(arrayLayers != 0, "cannot compute surface size for zero array layers");

    std::uint64_t sliceSize = 0;
    for (const auto& mip : mips) {
        Require(mip.tiledSize != 0, "encountered a zero-sized mip level while computing surface size");
        sliceSize = std::max(sliceSize, mip.tiledOffset + mip.tiledSize);
    }

    Require(sliceSize <= UINT64_MAX / arrayLayers, "tiled texture surface size overflows");
    return sliceSize * arrayLayers;
}

const TextureSwizzleEquation* FindTextureSwizzleEquation(std::uint32_t swizzleMode, std::uint32_t elementBytes) {
    for (const auto& equation : kTextureSwizzleEquations) {
        if (equation.swizzleMode == swizzleMode && equation.elementBytes == elementBytes) return &equation;
    }
    return nullptr;
}

std::uint32_t EquationOffset(const TextureSwizzleEquation& equation, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    std::uint32_t offset = 0;
    for (std::uint32_t bit = 0; bit < 16u; ++bit) {
        const auto mask = equation.bits[bit];
        const auto selected = (x & mask & 0xfffu) ^ ((y << 12u) & mask & 0xfff000u) ^ ((z << 24u) & mask & 0xff000000u);
        offset |= static_cast<std::uint32_t>(std::popcount(selected) & 1) << bit;
    }
    return offset;
}

bool MipLevelsFitAllocation(TextureTileMode tileMode, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t allocatedLevels, std::uint32_t levels) {
    Require(allocatedLevels != 0 && levels >= allocatedLevels && levels <= 16u, "an extended mip chain must cover the allocated levels and at most 16");
    // Depth surfaces have a single level and no mip tail: another level always grows them.
    if (tileMode == TextureTileMode::Depth64KB) return levels == allocatedLevels;
    const auto allocated = ComputeMipLayout(tileMode, format, width, height, allocatedLevels);
    const auto extended = ComputeMipLayout(tileMode, format, width, height, levels);
    const auto bytes = ComputeSurfaceSize(allocated, 1);
    if (ComputeSurfaceSize(extended, 1) != bytes) return false;
    for (std::uint32_t level = 0; level < allocatedLevels; ++level) {
        const auto& before = allocated[level];
        const auto& after = extended[level];
        if (before.tiledOffset != after.tiledOffset || before.tiledSize != after.tiledSize || before.blocksPerRow != after.blocksPerRow || before.tail != after.tail || before.tailX != after.tailX || before.tailY != after.tailY) return false;
    }
    for (auto level = allocatedLevels; level < levels; ++level) {
        if (extended[level].tiledOffset + extended[level].tiledSize > bytes) return false;
    }
    return true;
}

}
