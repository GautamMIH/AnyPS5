#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTUREADDRESSING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTUREADDRESSING_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include <cstdint>

namespace AgcDriver::Graphics {

// Byte offset of element (x, y) of one mip inside a guest surface, measured from the surface base.
// CPU counterpart of TextureDetile.comp; both must stay in step.
std::uint64_t TexelOffset(TextureTileMode mode, std::uint32_t elementBytes, const TileMipLayout& mip, std::uint32_t x, std::uint32_t y, std::uint32_t arrayLayer = 0);

}

#endif
