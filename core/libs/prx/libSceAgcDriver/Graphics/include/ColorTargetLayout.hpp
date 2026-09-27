#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COLORTARGETLAYOUT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COLORTARGETLAYOUT_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace AgcDriver::Graphics {

enum class ColorTileMode : std::uint32_t {
    Linear = 0,
    RenderTarget = 0x1b
};

ColorTileMode DecodeColorTileMode(std::uint32_t attrib3);

// A mip packed into a surface's mip tail: it lives inside one 64 KiB block at an element origin.
struct ColorTail {
    bool present = false;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    bool operator==(const ColorTail&) const = default;
};

// A color surface of 1-, 2-, 4-, 8- or 16-byte elements, linear or SW_64KB_R_X tiled.
class ColorTargetLayout {
public:
    ColorTargetLayout(std::uint32_t width, std::uint32_t height, ColorTileMode mode, std::uint32_t elementBytes = 4, ColorTail tail = {});
    std::size_t Bytes() const { return bytes; }
    std::size_t LinearBytes() const { return static_cast<std::size_t>(width) * height * elementBytes; }
    std::size_t Alignment() const { return mode == ColorTileMode::Linear ? 256u : 65536u; }
    std::uint32_t ElementBytes() const { return elementBytes; }
    std::uint32_t BlockWidth() const { return blockWidth; }
    std::uint32_t BlockHeight() const { return blockHeight; }
    std::uint32_t BlocksPerRow() const { return pitch / blockWidth; }
    const ColorTail& Tail() const { return tail; }
    std::size_t Offset(std::uint32_t x, std::uint32_t y) const;
    void Detile(std::span<const std::byte> source, std::span<std::byte> destination) const;
    void Tile(std::span<const std::byte> source, std::span<std::byte> destination) const;

private:
    std::size_t offset(std::uint32_t x, std::uint32_t y) const;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pitch;
    ColorTileMode mode;
    std::uint32_t elementBytes;
    std::uint32_t blockWidth = 1;
    std::uint32_t blockHeight = 1;
    ColorTail tail;
    std::size_t bytes;
};

}

#endif
