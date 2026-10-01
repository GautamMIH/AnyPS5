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
