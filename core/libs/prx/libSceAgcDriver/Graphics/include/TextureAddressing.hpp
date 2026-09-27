#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTUREADDRESSING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTUREADDRESSING_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include <cstdint>

namespace AgcDriver::Graphics {

// Byte offset of element (x, y) of one mip inside a guest surface, measured from the surface base.
// CPU counterpart of TextureDetile.comp; both must stay in step.
std::uint64_t TexelOffset(TextureTileMode mode, std::uint32_t elementBytes, const TileMipLayout& mip, std::uint32_t x, std::uint32_t y, std::uint32_t arrayLayer = 0);

// 3D textures in the standard 4 KiB/64 KiB tilings use thick blocks that also span slices
// (swizzles from KytyPS5); only single-mip volumes are modelled.
bool IsThickVolume(const GuestTextureResource& resource);
std::uint64_t ThickVolumeOffset(TextureTileMode mode, std::uint32_t elementBytes, std::uint32_t width, std::uint32_t height, std::uint32_t x, std::uint32_t y, std::uint32_t z);

// Guest bytes of a texture's whole surface: every mip of every layer or slice.
std::uint64_t GuestTextureBytes(const GuestTextureResource& resource);

}

#endif
