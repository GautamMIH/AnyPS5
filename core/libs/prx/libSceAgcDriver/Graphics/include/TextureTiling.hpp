#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURETILING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURETILING_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include <cstdint>
#include <vector>

namespace AgcDriver::Graphics {

struct TileMipLayout {
    std::uint64_t tiledOffset;
    std::uint64_t tiledSize;
    std::uint64_t linearOffset;
    std::uint64_t linearSize;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t blocksPerRow;
    std::uint32_t pitchBytes;
    bool tail;
    std::uint32_t tailX;
    std::uint32_t tailY;
};

std::vector<TileMipLayout> ComputeMipLayout(TextureTileMode tileMode, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t mipCount);
// Mip chain of an uncompressed tiled surface described by its element size (render targets).
std::vector<TileMipLayout> ComputeElementMipLayout(TextureTileMode tileMode, std::uint32_t elementBytes, std::uint32_t width, std::uint32_t height, std::uint32_t mipCount);
std::uint64_t ComputeSurfaceSize(const std::vector<TileMipLayout>& mips, std::uint32_t arrayLayers);
// GFX10 uses MAX_MIP only to place the levels: a level past it is addressed like the levels below
// it (a mip-tail level is a fixed slot of the tail block). Whether the chain of `levels` levels keeps
// the size and every level of the `allocatedLevels`-level chain and places its extra levels inside
// that allocation, so a view of those extra levels addresses the surface's own memory.
bool MipLevelsFitAllocation(TextureTileMode tileMode, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t allocatedLevels, std::uint32_t levels);

// Address equation of a swizzle mode inside one block, for the PS5 GPU (a Navi1x-class part with 16
// pipes), generated from AMD addrlib's GFX10 non-RB+ swizzle patterns (upstream). For byte-address
// bit 0..15 inside a block, the element-coordinate bits XORed into it: bits 0-11 select x bits,
// bits 12-23 select y bits and bits 24-31 select slice (or depth) bits. Thick (3D) standard
// swizzles are keyed 0x100 | SW_MODE.
struct TextureSwizzleEquation {
    std::uint32_t swizzleMode;
    std::uint32_t elementBytes;
    std::uint32_t bits[16];
};
const TextureSwizzleEquation* FindTextureSwizzleEquation(std::uint32_t swizzleMode, std::uint32_t elementBytes);
// The in-block byte offset an equation gives element (x, y) of slice z.
std::uint32_t EquationOffset(const TextureSwizzleEquation& equation, std::uint32_t x, std::uint32_t y, std::uint32_t z);

}

#endif
