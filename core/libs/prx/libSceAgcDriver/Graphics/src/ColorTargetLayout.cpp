#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include <cstring>
#include <limits>
#include <stdexcept>

namespace AgcDriver::Graphics {
namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

// SW_64KB_R_X in-block offsets per element size (as in TextureDetile.comp).
std::uint32_t blockOffset(std::uint32_t x, std::uint32_t y, std::uint32_t elementBytes) {
    switch (elementBytes) {
        case 1:
            return ((y << 2u) & 0x0008u) ^ ((y << 4u) & 0x0010u) ^ ((y << 3u) & 0x00a0u) ^ ((y << 5u) & 0x0f00u) ^ ((y << 6u) & 0x1000u) ^ ((y << 7u) & 0x4000u)
                ^ (x & 0x0007u) ^ ((x << 3u) & 0x0040u) ^ ((x << 5u) & 0x0300u) ^ ((x << 4u) & 0x0400u) ^ ((x << 6u) & 0x0800u) ^ ((x << 7u) & 0x2000u) ^ ((x << 8u) & 0x8000u);
        case 2:
            return ((y << 4u) & 0x0070u) ^ ((y << 5u) & 0x0f00u) ^ ((y << 8u) & 0x5000u)
                ^ ((x << 1u) & 0x000eu) ^ ((x << 4u) & 0x0480u) ^ ((x << 5u) & 0x0300u) ^ ((x << 6u) & 0x0800u) ^ ((x << 7u) & 0x2000u) ^ ((x << 8u) & 0x8000u);
        case 4:
            return ((y << 4u) & 0x0070u) ^ ((y << 5u) & 0x0f00u) ^ ((y << 9u) & 0x1000u) ^ ((y << 8u) & 0x4000u)
                ^ ((x << 2u) & 0x000cu) ^ ((x << 5u) & 0x0380u) ^ ((x << 4u) & 0x0400u) ^ ((x << 6u) & 0x0800u) ^ ((x << 9u) & 0xa000u);
        case 8:
            return ((y << 4u) & 0x0010u) ^ ((y << 6u) & 0x0080u) ^ ((y << 5u) & 0x0f00u) ^ ((y << 10u) & 0x5000u)
                ^ ((x << 3u) & 0x0008u) ^ ((x << 4u) & 0x0460u) ^ ((x << 5u) & 0x0300u) ^ ((x << 6u) & 0x0800u) ^ ((x << 10u) & 0x2000u) ^ ((x << 9u) & 0x8000u);
        default:
            return ((x << 4u) & 0x0410u) ^ ((x << 5u) & 0x0340u) ^ ((x << 6u) & 0x0800u) ^ ((x << 11u) & 0xa000u)
                ^ ((y << 5u) & 0x0f20u) ^ ((y << 6u) & 0x0080u) ^ ((y << 10u) & 0x1000u) ^ ((y << 11u) & 0x4000u);
    }
}

// SW_4KB_S in-block offsets per element size (TextureAddressing.cpp, as for textures).
std::uint32_t standardOffset(std::uint32_t x, std::uint32_t y, std::uint32_t elementBytes) {
    switch (elementBytes) {
        case 1: return ((y << 4u) & 0x1f0u) ^ ((y << 5u) & 0x400u) ^ (x & 0x00fu) ^ ((x << 5u) & 0x200u) ^ ((x << 6u) & 0x800u);
        case 2: return ((y << 4u) & 0x070u) ^ ((y << 5u) & 0x100u) ^ ((y << 6u) & 0x400u) ^ ((x << 1u) & 0x00eu) ^ ((x << 4u) & 0x080u) ^ ((x << 5u) & 0x200u) ^ ((x << 6u) & 0x800u);
        case 4: return ((y << 4u) & 0x070u) ^ ((y << 5u) & 0x100u) ^ ((y << 6u) & 0x400u) ^ ((x << 2u) & 0x00cu) ^ ((x << 5u) & 0x080u) ^ ((x << 6u) & 0x200u) ^ ((x << 7u) & 0x800u);
        case 8: return ((y << 4u) & 0x030u) ^ ((y << 6u) & 0x100u) ^ ((y << 7u) & 0x400u) ^ ((x << 3u) & 0x008u) ^ ((x << 5u) & 0x0c0u) ^ ((x << 6u) & 0x200u) ^ ((x << 7u) & 0x800u);
        default: return ((y << 4u) & 0x030u) ^ ((y << 6u) & 0x100u) ^ ((y << 7u) & 0x400u) ^ ((x << 6u) & 0x0c0u) ^ ((x << 7u) & 0x200u) ^ ((x << 8u) & 0x800u);
    }
}

}

ColorTileMode DecodeColorTileMode(std::uint32_t attrib3) {
    // MIP0_DEPTH (bits 0-12) counts array or volume slices (see DecodeState); RESOURCE_TYPE
    // (bits 24-25) is 2D or 3D, whose render-target slices are laid out alike.
    const auto resourceType = (attrib3 >> 24u) & 3u;
    require((attrib3 & 0x80002000u) == 0 && (resourceType == 1 || resourceType == 2) && ((attrib3 >> 27u) & 7u) == 1, "AGC graphics: unsupported color dimension, resource level or metadata mode");
    const auto mode = (attrib3 >> 14u) & 0x1fu;
    const auto fmaskMode = (attrib3 >> 19u) & 0x1fu;
    require(fmaskMode == 0 || fmaskMode == 0x18, "AGC graphics: unsupported color FMASK swizzle mode");
    require(mode == 0 || mode == 5 || mode == 0x1b, "AGC graphics: unsupported color tile mode");
    return static_cast<ColorTileMode>(mode);
}

ColorTargetLayout::ColorTargetLayout(std::uint32_t width, std::uint32_t height, ColorTileMode mode, std::uint32_t elementBytes, ColorTail tail) : width(width), height(height), pitch(width), mode(mode), elementBytes(elementBytes), tail(tail), bytes(0) {
    require(width != 0 && height != 0 && width <= 16384 && height <= 16384, "AGC graphics: invalid color surface extent");
    require(elementBytes == 1 || elementBytes == 2 || elementBytes == 4 || elementBytes == 8 || elementBytes == 16, "AGC graphics: unsupported color element size");
    std::uint32_t paddedHeight = height;
    switch (mode) {
        case ColorTileMode::Linear: {
            // Rows are padded to 256 bytes, addrlib's linear pitch alignment (linear textures use the same).
            const auto pitchAlignment = 256u / elementBytes;
            pitch = (width + pitchAlignment - 1u) / pitchAlignment * pitchAlignment;
            break;
        }
        case ColorTileMode::RenderTarget:
            // 64 KiB blocks: 256x256, 256x128, 128x128, 128x64 or 64x64 elements.
            blockWidth = elementBytes <= 2u ? 256u : elementBytes <= 8u ? 128u : 64u;
            blockHeight = 65536u / (blockWidth * elementBytes);
            pitch = (width + blockWidth - 1u) / blockWidth * blockWidth;
            paddedHeight = (height + blockHeight - 1u) / blockHeight * blockHeight;
            break;
        case ColorTileMode::Standard4KB:
            // 4 KiB blocks: 64x64, 64x32, 32x32, 32x16 or 16x16 elements (addrlib's thin 4 KiB blocks).
            blockWidth = elementBytes <= 2u ? 64u : elementBytes <= 8u ? 32u : 16u;
            blockHeight = 4096u / (blockWidth * elementBytes);
            pitch = (width + blockWidth - 1u) / blockWidth * blockWidth;
            paddedHeight = (height + blockHeight - 1u) / blockHeight * blockHeight;
            break;
        default: throw std::runtime_error("AGC graphics: unsupported color tile mode");
    }
    if (tail.present) {
        require(mode != ColorTileMode::Linear, "AGC graphics: a mip tail requires a tiled surface");
        require(tail.x + width <= blockWidth && tail.y + height <= blockHeight, "AGC graphics: mip tail level exceeds its block");
        pitch = blockWidth;
        paddedHeight = blockHeight;
    }
    const auto size = static_cast<std::uint64_t>(pitch) * paddedHeight * elementBytes;
    require(size <= std::numeric_limits<std::size_t>::max(), "AGC graphics: color surface size overflow");
    bytes = static_cast<std::size_t>(size);
}

std::size_t ColorTargetLayout::offset(std::uint32_t x, std::uint32_t y) const {
    if (mode == ColorTileMode::Linear) return (static_cast<std::size_t>(y) * pitch + x) * elementBytes;
    const bool standard = mode == ColorTileMode::Standard4KB;
    const auto inner = [&](std::uint32_t bx, std::uint32_t by) -> std::size_t { return standard ? standardOffset(bx, by, elementBytes) & 0xfffu : blockOffset(bx, by, elementBytes) & 0xffffu; };
    if (tail.present) return inner(x + tail.x, y + tail.y);
    const auto block = static_cast<std::size_t>(y / blockHeight) * (pitch / blockWidth) + x / blockWidth;
    return block * Alignment() + inner(x, y);
}

std::size_t ColorTargetLayout::Offset(std::uint32_t x, std::uint32_t y) const {
    require(x < width && y < height, "AGC graphics: color surface coordinate out of range");
    return offset(x, y);
}

void ColorTargetLayout::Detile(std::span<const std::byte> source, std::span<std::byte> destination) const {
    require(source.size() == Bytes() && destination.size() == LinearBytes(), "AGC graphics: color detile buffer size mismatch");
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::memcpy(destination.data() + (static_cast<std::size_t>(y) * width + x) * elementBytes, source.data() + offset(x, y), elementBytes);
        }
    }
}

void ColorTargetLayout::Tile(std::span<const std::byte> source, std::span<std::byte> destination) const {
    require(source.size() == LinearBytes() && destination.size() == Bytes(), "AGC graphics: color tile buffer size mismatch");
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::memcpy(destination.data() + offset(x, y), source.data() + (static_cast<std::size_t>(y) * width + x) * elementBytes, elementBytes);
        }
    }
}

}
