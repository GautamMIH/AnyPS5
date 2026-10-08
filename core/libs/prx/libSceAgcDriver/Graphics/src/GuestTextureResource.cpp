#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace AgcDriver::Graphics {

namespace {

void requireValidDstSel(std::uint32_t value) {
    Require(value == 0 || value == 1 || (value >= 4 && value <= 7), "guest texture descriptor has an invalid destination channel selector");
}

TextureTileMode resolveTileMode(std::uint32_t raw) {
    switch (raw) {
        case 0x00: return TextureTileMode::kLinear;
        case 0x01: return TextureTileMode::kStandard256B;
        case 0x02: return TextureTileMode::kD256B;
        case 0x05: return TextureTileMode::kStandard4KB;
        case 0x06: return TextureTileMode::kD4KB;
        case 0x09: return TextureTileMode::kStandard64KB;
        case 0x0a: return TextureTileMode::kD64KB;
        case 0x11: return TextureTileMode::kS64KBT;
        case 0x12: return TextureTileMode::kD64KBT;
        case 0x15: return TextureTileMode::kS4KBX;
        case 0x16: return TextureTileMode::kD4KBX;
        case 0x18: return TextureTileMode::Depth64KB;
        case 0x19: return TextureTileMode::kS64KBX;
        case 0x1a: return TextureTileMode::kD64KBX;
        case 0x1b: return TextureTileMode::RenderTarget64KB;
        default: throw std::runtime_error("AGC graphics: guest texture descriptor uses an unsupported tile mode " + std::to_string(raw));
    }
}

TextureDimension resolveDimension(std::uint32_t raw) {
    switch (raw) {
        case 8: return TextureDimension::k1D;
        case 9: return TextureDimension::k2D;
        case 10: return TextureDimension::k3D;
        case 11: return TextureDimension::kCube;
        case 12: return TextureDimension::k1DArray;
        case 13: return TextureDimension::k2DArray;
        default: throw std::runtime_error("AGC graphics: guest texture descriptor uses an unsupported image type " + std::to_string(raw));
    }
}

}

GuestTextureResource DecodeTextureResource(std::span<const std::uint32_t> words) {
    Require(words.size() == 8, "guest texture descriptor must contain 8 dwords");

    const auto base40 = (static_cast<std::uint64_t>(words[0]) | (static_cast<std::uint64_t>(words[1]) << 32u)) & 0xffffffffffull;
    const auto baseAddress = base40 << 8u;
    Require(baseAddress != 0, "guest texture descriptor has a null base address");

    const auto minLod = (words[1] >> 8u) & 0xfffu;
    const auto format = (words[1] >> 20u) & 0x1ffu;
    const auto width = (((words[1] >> 30u) & 0x3u) | (((words[2] >> 0u) & 0xfffu) << 2u)) + 1u;
    const auto height = ((words[2] >> 14u) & 0x3fffu) + 1u;

    const auto dstSelX = (words[3] >> 0u) & 0x7u;
    const auto dstSelY = (words[3] >> 3u) & 0x7u;
    const auto dstSelZ = (words[3] >> 6u) & 0x7u;
    const auto dstSelW = (words[3] >> 9u) & 0x7u;
    const auto baseLevel = (words[3] >> 12u) & 0xfu;
    const auto lastLevel = (words[3] >> 16u) & 0xfu;
    const auto tileModeRaw = (words[3] >> 20u) & 0x1fu;
    const auto bcSwizzle = (words[3] >> 25u) & 0x7u;
    const auto typeRaw = (words[3] >> 28u) & 0xfu;

    const auto depth = (words[4] >> 0u) & 0x1fffu;
    const auto baseArray = (words[4] >> 16u) & 0x1fffu;

    const auto arrayPitch = (words[5] >> 0u) & 0xfu;
    const auto maxMip = (words[5] >> 4u) & 0xfu;
    const auto minLodWarn = (words[5] >> 8u) & 0xfffu;
    const auto cornerSample = ((words[5] >> 23u) & 0x1u) != 0;
    const auto mipStatsCntEn = ((words[5] >> 25u) & 0x1u) != 0;
    const auto prtDefColor = ((words[5] >> 26u) & 0x1u) != 0;

    const auto mipStatsCntId = words[6] & 0xffu;
    const auto msaaDepth = ((words[6] >> 10u) & 0x1u) != 0;
    const auto maxUncompBlkSize = (words[6] >> 15u) & 0x3u;
    const auto maxCompBlkSize = (words[6] >> 17u) & 0x3u;
    const auto metaPipeAligned = ((words[6] >> 19u) & 0x1u) != 0;
    const auto writeCompress = ((words[6] >> 20u) & 0x1u) != 0;
    const auto metaCompress = ((words[6] >> 21u) & 0x1u) != 0;
    const auto dccAlphaPos = ((words[6] >> 22u) & 0x1u) != 0;
    const auto dccColorTransf = ((words[6] >> 23u) & 0x1u) != 0;
    const auto metaAddr = ((static_cast<std::uint64_t>(words[6]) >> 24u) & 0xffu) | (static_cast<std::uint64_t>(words[7]) << 8u);

    requireValidDstSel(dstSelX);
    requireValidDstSel(dstSelY);
    requireValidDstSel(dstSelZ);
    requireValidDstSel(dstSelW);

    Require(minLodWarn == 0, "guest texture descriptor uses a minimum LOD warning threshold which is not implemented");
    Require(mipStatsCntId == 0 && !mipStatsCntEn, "guest texture descriptor uses mip statistics counters which are not implemented");
    Require(!cornerSample, "guest texture descriptor uses corner sampling which is not implemented");
    Require(!prtDefColor, "guest texture descriptor uses a partially resident default color which is not implemented");
    Require(arrayPitch == 0, "guest texture descriptor uses a nonzero array pitch which is not implemented");
    Require(!msaaDepth, "guest texture descriptor uses MSAA which is not implemented");
    // DCC fields (block sizes, compression, metadata address) describe how a surface may be
    // stored compressed. Emulated GPU writes are always plain texels (colour targets ignore
    // DCC_ENABLE too), so the surface is read uncompressed and the metadata is never consulted,
    // as in shadPS4.
    static_cast<void>(maxUncompBlkSize);
    static_cast<void>(maxCompBlkSize);
    static_cast<void>(metaPipeAligned);
    static_cast<void>(writeCompress);
    static_cast<void>(metaCompress);
    static_cast<void>(dccAlphaPos);
    static_cast<void>(dccColorTransf);
    static_cast<void>(metaAddr);
    Require(bcSwizzle == 0, "guest texture descriptor uses a BC swizzle which is not implemented");

    Require(baseLevel <= lastLevel, "guest texture descriptor has a base mip level past its last mip level");

    const auto tileMode = resolveTileMode(tileModeRaw);
    const auto dimension = resolveDimension(typeRaw);

    switch (dimension) {
        case TextureDimension::k1D:
            Require(height == 1 && depth == 0 && baseArray == 0, "guest 1D texture descriptor has a nonzero height, depth or base array");
            break;
        case TextureDimension::k2D:
            Require(depth == 0 && baseArray == 0, "guest 2D texture descriptor has a nonzero depth or base array");
            break;
        case TextureDimension::k2DArray:
            Require(baseArray <= depth, "guest 2D array texture descriptor has a base array past its last array slice");
            break;
        case TextureDimension::k3D:
            // Thin tilings store a volume's slices like array layers (SW_64KB_Z_X too, with its
            // equation's slice term; upstream c10401e3) and standard tilings and SW_64KB_S_X use
            // thick blocks (KytyPS5, addrlib); mip chains, whose depth shrinks per level, are not
            // modelled.
            Require(baseArray == 0, "guest 3D texture descriptor has a nonzero base slice");
            Require(tileMode != TextureTileMode::kStandard256B && tileMode != TextureTileMode::kD256B, "3D textures in 256-byte tiling are invalid");
            Require(tileMode == TextureTileMode::kLinear || tileMode == TextureTileMode::kStandard4KB || tileMode == TextureTileMode::kStandard64KB || tileMode == TextureTileMode::kS64KBX || tileMode == TextureTileMode::Depth64KB || tileMode == TextureTileMode::RenderTarget64KB || tileMode == TextureTileMode::kD64KBX, "3D textures are implemented linear, in SW_4KB_S, SW_64KB_S or SW_64KB_S_X (thick) and in SW_64KB_Z_X, SW_64KB_D_X or SW_64KB_R_X (thin) only");
            Require(maxMip == 0, "mipmapped 3D textures are not implemented");
            break;
        case TextureDimension::k1DArray:
            // Laid out as a 2D array of height 1; addrlib allows only the linear, Z and R swizzles
            // for 1D resources, whose S and D layouts differ from a 2D array's (upstream eb30830d).
            Require(height == 1, "guest 1D array texture descriptor has a nonzero height");
            Require(baseArray <= depth, "guest 1D array texture descriptor has a base array past its last array slice");
            Require(tileMode == TextureTileMode::kLinear || tileMode == TextureTileMode::Depth64KB || tileMode == TextureTileMode::RenderTarget64KB, "guest 1D array texture descriptor uses a tile mode other than linear, Z or R, which 1D resources cannot use");
            break;
        case TextureDimension::kCube:
            Require(width == height, "guest cube texture descriptor is not square");
            Require(baseArray <= depth, "guest cube texture descriptor has a base array past its last array slice");
            Require((depth - baseArray + 1u) % 6u == 0, "guest cube texture descriptor does not contain a multiple of 6 array slices");
            break;
    }

    // The equation XOR and T swizzles fold a pipe/bank XOR into the low address bits; only bases
    // aligned to the block are modelled (SW_64KB_Z_X and R_X keep their own formula and accept any).
    if (EquationSwizzleMode(tileMode) != 0 && XorSwizzleMode(tileMode) != 0) {
        const auto blockMask = tileMode == TextureTileMode::kS4KBX || tileMode == TextureTileMode::kD4KBX ? 0xfffu : 0xffffu;
        Require((baseAddress & blockMask) == 0, "guest texture descriptor combines an XOR swizzle with a pipe/bank XOR base which is not implemented");
    }

    // Views may name levels past MAX_MIP. One that starts inside the surface ends at its last level
    // (a 512x512 view through 1x1 over a 9-level surface). One that starts past it addresses those
    // levels as the hardware does, through the chain extended to its last level, which must keep
    // the allocated levels and the surface size (levels in the mip tail: PPSA21564's bloom pass
    // stores level 6 of a 6-level 1920x1080 RGBA16F SW_64KB_R_X chain).
    auto viewLastLevel = lastLevel;
    auto mipCount = maxMip + 1u;
    if (baseLevel <= maxMip) {
        viewLastLevel = std::min(lastLevel, maxMip);
    } else {
        Require(dimension != TextureDimension::k3D, "guest 3D texture descriptor starts past the surface's last mip level");
        Require(MipLevelsFitAllocation(tileMode, format, width, height, mipCount, lastLevel + 1u), "guest texture descriptor starts past the surface's last mip level at levels that would move the surface's own");
        mipCount = lastLevel + 1u;
    }

    GuestTextureResource result{};
    result.baseAddress = baseAddress;
    result.width = width;
    result.height = height;
    result.depthOrLastArray = depth;
    result.baseArray = baseArray;
    result.mipCount = mipCount;
    result.baseLevel = baseLevel;
    result.lastLevel = viewLastLevel;
    result.tileMode = tileMode;
    result.dimension = dimension;
    result.viewDimension = dimension;
    result.format = format;
    result.dstSelX = static_cast<std::uint8_t>(dstSelX);
    result.dstSelY = static_cast<std::uint8_t>(dstSelY);
    result.dstSelZ = static_cast<std::uint8_t>(dstSelZ);
    result.dstSelW = static_cast<std::uint8_t>(dstSelW);
    result.minLod = minLod;
    return result;
}

float EffectiveMinLod(const GuestTextureResource& resource) {
    if (resource.minLod <= resource.baseLevel * 256u) return 0.0f;
    return std::min(static_cast<float>(resource.minLod) / 256.0f, static_cast<float>(resource.lastLevel));
}

bool MatchesGuestDimension(ShaderRecompiler::DescriptorImageShape shape, TextureDimension dimension) {
    switch (shape) {
        case ShaderRecompiler::DescriptorImageShape::Image1D: return dimension == TextureDimension::k1D;
        case ShaderRecompiler::DescriptorImageShape::Image2D: return dimension == TextureDimension::k2D;
        // A cube's faces are addressable as 2D array layers.
        case ShaderRecompiler::DescriptorImageShape::Image2DArray: return dimension == TextureDimension::k2DArray || dimension == TextureDimension::kCube;
        case ShaderRecompiler::DescriptorImageShape::ImageCube: return dimension == TextureDimension::kCube;
        case ShaderRecompiler::DescriptorImageShape::Image3D: return dimension == TextureDimension::k3D;
        case ShaderRecompiler::DescriptorImageShape::Image1DArray: return dimension == TextureDimension::k1DArray;
    }
    throw std::runtime_error("AGC graphics: MatchesGuestDimension encountered an unknown descriptor image shape");
}

std::optional<TextureDimension> SampledViewDimension(ShaderRecompiler::DescriptorImageShape shape, TextureDimension dimension) {
    using Shape = ShaderRecompiler::DescriptorImageShape;
    const bool layered = dimension == TextureDimension::k2D || dimension == TextureDimension::k2DArray || dimension == TextureDimension::kCube;
    switch (shape) {
        case Shape::Image1D: return dimension == TextureDimension::k1D || dimension == TextureDimension::k1DArray ? std::optional(TextureDimension::k1D) : std::nullopt;
        case Shape::Image1DArray: return dimension == TextureDimension::k1D || dimension == TextureDimension::k1DArray ? std::optional(TextureDimension::k1DArray) : std::nullopt;
        case Shape::Image2D: return layered ? std::optional(TextureDimension::k2D) : std::nullopt;
        case Shape::Image2DArray: return layered ? std::optional(TextureDimension::k2DArray) : std::nullopt;
        case Shape::ImageCube: return dimension == TextureDimension::kCube ? std::optional(TextureDimension::kCube) : std::nullopt;
        case Shape::Image3D: return dimension == TextureDimension::k3D ? std::optional(TextureDimension::k3D) : std::nullopt;
    }
    throw std::runtime_error("AGC graphics: SampledViewDimension encountered an unknown descriptor image shape");
}

}
