#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHTARGETLAYOUT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHTARGETLAYOUT_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace AgcDriver::Graphics {

// One plane (depth or stencil) of a PS5 depth surface: 64 KiB depth-swizzled blocks laid out
// row-major over the block-aligned surface. Block shapes and in-block swizzles follow KytyPS5.
class DepthTargetLayout {
public:
    DepthTargetLayout(std::uint32_t width, std::uint32_t height, std::uint32_t bytesPerElement);
    std::size_t Bytes() const { return bytes; }
    std::size_t LinearBytes() const { return static_cast<std::size_t>(width) * height * elementBytes; }
    std::size_t Alignment() const { return 65536u; }
    std::uint32_t ElementBytes() const { return elementBytes; }
    std::size_t Offset(std::uint32_t x, std::uint32_t y) const;
    void Detile(std::span<const std::byte> source, std::span<std::byte> destination) const;
    void Tile(std::span<const std::byte> source, std::span<std::byte> destination) const;

private:
    std::size_t offset(std::uint32_t x, std::uint32_t y) const;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t elementBytes;
    std::uint32_t blockWidth;
    std::uint32_t blockHeight;
    std::uint32_t blocksPerRow;
    std::size_t bytes;
};

}

#endif
