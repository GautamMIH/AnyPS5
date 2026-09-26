#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTTEXTURERESOURCE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTTEXTURERESOURCE_HPP

#include "Recompiler.hpp"
#include <cstdint>
#include <span>

namespace AgcDriver::Graphics {

enum class TextureTileMode {
    kLinear,
    kStandard256B,
    kStandard4KB,
    kStandard64KB,
    RenderTarget64KB,
    // SW_64KB_Z_X: depth and stencil planes written by the depth block (see DepthTargetLayout).
    Depth64KB
};

enum class TextureDimension {
    k1D,
    k2D,
    k2DArray,
    kCube,
    // Volume; its slices are addressed like array layers (thin tilings only).
    k3D
};

struct GuestTextureResource {
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
    std::uint32_t format;
    std::uint8_t dstSelX;
    std::uint8_t dstSelY;
    std::uint8_t dstSelZ;
    std::uint8_t dstSelW;
};

GuestTextureResource DecodeTextureResource(std::span<const std::uint32_t> words);
bool MatchesGuestDimension(ShaderRecompiler::DescriptorImageShape shape, TextureDimension dimension);

}

#endif
