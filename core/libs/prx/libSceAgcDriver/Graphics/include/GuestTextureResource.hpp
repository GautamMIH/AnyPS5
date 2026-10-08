#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTTEXTURERESOURCE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTTEXTURERESOURCE_HPP

#include "Recompiler.hpp"
#include <cstdint>
#include <optional>
#include <span>

namespace AgcDriver::Graphics {

enum class TextureTileMode {
    kLinear,
    kStandard256B,
    kStandard4KB,
    kStandard64KB,
    RenderTarget64KB,
    // SW_64KB_Z_X: depth and stencil planes written by the depth block (see DepthTargetLayout).
    Depth64KB,
    // Swizzles addressed by an equation table (TextureTiling.hpp, FindTextureSwizzleEquation,
    // generated from AMD addrlib's GFX10 patterns, ported from upstream ace10fdb and c10401e3):
    // the display (D) swizzles, the thin (T) and XOR (X) swizzles other than Z_X and R_X. They share
    // the block shape and mip tail of the standard mode of their block size.
    kS64KBX,
    kD64KBX,
    kD256B,
    kD4KB,
    kD64KB,
    kS64KBT,
    kD64KBT,
    kS4KBX,
    kD4KBX,
    // Upstream's names for SW_64KB_Z_X (tile mode 0x18) and SW_64KB_R_X (0x1b).
    kZ64KBX = Depth64KB,
    kR64KBX = RenderTarget64KB
};

// The hardware SW_MODE of an XOR or T swizzle tile mode, or 0 for the other modes.
constexpr std::uint32_t XorSwizzleMode(TextureTileMode mode) {
    switch (mode) {
        case TextureTileMode::Depth64KB: return 24u;
        case TextureTileMode::kS64KBX: return 25u;
        case TextureTileMode::kD64KBX: return 26u;
        case TextureTileMode::RenderTarget64KB: return 27u;
        case TextureTileMode::kS64KBT: return 17u;
        case TextureTileMode::kD64KBT: return 18u;
        case TextureTileMode::kS4KBX: return 21u;
        case TextureTileMode::kD4KBX: return 22u;
        default: return 0u;
    }
}

// The SW_MODE whose equation addresses the mode in the equation family of the detiler, or 0 for the
// modes with their own formula (linear, standard, Z_X, R_X).
constexpr std::uint32_t EquationSwizzleMode(TextureTileMode mode) {
    switch (mode) {
        case TextureTileMode::kD256B: return 2u;
        case TextureTileMode::kD4KB: return 6u;
        case TextureTileMode::kD64KB: return 10u;
        case TextureTileMode::Depth64KB:
        case TextureTileMode::RenderTarget64KB: return 0u;
        default: return XorSwizzleMode(mode);
    }
}

enum class TextureDimension {
    k1D,
    k2D,
    k2DArray,
    kCube,
    // Volume; its slices are addressed like array layers (thin tilings), or interleaved in thick
    // blocks (SW_4KB_S, SW_64KB_S, SW_64KB_S_X; see IsThickVolume).
    k3D,
    // 1D array (T# type 12): height 1, depth + 1 layers laid out as a 2D array's (upstream fef66c14).
    k1DArray
};

struct GuestTextureResource {
    // T# word1 MIN_LOD: unsigned 4.8, in levels of the whole surface.
    std::uint32_t minLod = 0;
    std::uint64_t baseAddress;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depthOrLastArray;
    std::uint32_t baseArray;
    std::uint32_t mipCount;
    std::uint32_t baseLevel;
    // Last mip the view exposes; the surface layout still spans all mipCount levels.
    std::uint32_t lastLevel;
    TextureTileMode tileMode;
    TextureDimension dimension;
    // How shaders see the texture: the instruction's shape picks the view (shadPS4 ImageViewInfo),
    // while dimension keeps describing the surface in memory.
    TextureDimension viewDimension;
    std::uint32_t format;
    std::uint8_t dstSelX;
    std::uint8_t dstSelY;
    std::uint8_t dstSelZ;
    std::uint8_t dstSelW;
};

GuestTextureResource DecodeTextureResource(std::span<const std::uint32_t> words);
// The view's MIN_LOD clamp in levels of the whole image (the image holds every mip and the view
// starts at BASE_LEVEL): 0 when it cannot change the level a sample reads (at or below BASE_LEVEL),
// at most the view's last level (Vulkan's bound; the view selects that level anyway).
float EffectiveMinLod(const GuestTextureResource& resource);
bool MatchesGuestDimension(ShaderRecompiler::DescriptorImageShape shape, TextureDimension dimension);
// The view a sampled image of this dimension presents to a shader of this shape: a 2D shape views
// one layer of an array or cube, an array shape views a 2D texture or cube as layers.
std::optional<TextureDimension> SampledViewDimension(ShaderRecompiler::DescriptorImageShape shape, TextureDimension dimension);

}

#endif
