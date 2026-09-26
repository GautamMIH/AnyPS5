#include "prx/libSceAgcDriver/Graphics/include/DepthTargetLayout.hpp"
#include <cstring>
#include <limits>
#include <stdexcept>

namespace AgcDriver::Graphics {
namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::uint32_t blockOffset8(std::uint32_t x, std::uint32_t y) {
    return (x & 1u) ^ ((x << 1u) & 0x4u) ^ ((x << 2u) & 0x10u) ^ ((x << 3u) & 0x40u) ^ ((x << 5u) & 0x300u) ^ ((x << 4u) & 0x400u) ^ ((x << 6u) & 0x800u) ^ ((x << 7u) & 0x2000u) ^ ((x << 8u) & 0x8000u)
        ^ ((y << 1u) & 0x2u) ^ ((y << 2u) & 0x8u) ^ ((y << 3u) & 0xa0u) ^ ((y << 5u) & 0xf00u) ^ ((y << 6u) & 0x1000u) ^ ((y << 7u) & 0x4000u);
}

std::uint32_t blockOffset16(std::uint32_t x, std::uint32_t y) {
    return ((x << 1u) & 0x2u) ^ ((x << 2u) & 0x8u) ^ ((x << 3u) & 0x20u) ^ ((x << 4u) & 0x480u) ^ ((x << 5u) & 0x300u) ^ ((x << 6u) & 0x800u) ^ ((x << 7u) & 0x2000u) ^ ((x << 8u) & 0x8000u)
        ^ ((y << 2u) & 0x4u) ^ ((y << 3u) & 0x10u) ^ ((y << 4u) & 0x40u) ^ ((y << 5u) & 0xf00u) ^ ((y << 8u) & 0x5000u);
}

std::uint32_t blockOffset32(std::uint32_t x, std::uint32_t y) {
    return ((x << 2u) & 0x4u) ^ ((x << 3u) & 0x10u) ^ ((x << 4u) & 0x440u) ^ ((x << 5u) & 0x300u) ^ ((x << 6u) & 0x800u) ^ ((x << 9u) & 0xa000u)
        ^ ((y << 3u) & 0x8u) ^ ((y << 4u) & 0x20u) ^ ((y << 5u) & 0xf80u) ^ ((y << 9u) & 0x1000u) ^ ((y << 8u) & 0x4000u);
}

}

DepthTargetLayout::DepthTargetLayout(std::uint32_t width, std::uint32_t height, std::uint32_t bytesPerElement) : width(width), height(height), elementBytes(bytesPerElement), blockWidth(0), blockHeight(0), blocksPerRow(0), bytes(0) {
    require(width != 0 && height != 0 && width <= 16384 && height <= 16384, "AGC graphics: invalid depth surface extent");
    switch (bytesPerElement) {
        case 1: blockWidth = 256; blockHeight = 256; break;
        case 2: blockWidth = 256; blockHeight = 128; break;
        case 4: blockWidth = 128; blockHeight = 128; break;
        default: throw std::runtime_error("AGC graphics: unsupported depth element size");
    }
    blocksPerRow = (width + blockWidth - 1u) / blockWidth;
    const auto blockRows = (height + blockHeight - 1u) / blockHeight;
    const auto size = static_cast<std::uint64_t>(blocksPerRow) * blockRows * 65536u;
    require(size <= std::numeric_limits<std::size_t>::max(), "AGC graphics: depth surface size overflow");
    bytes = static_cast<std::size_t>(size);
}

std::size_t DepthTargetLayout::offset(std::uint32_t x, std::uint32_t y) const {
    const auto block = static_cast<std::size_t>(y / blockHeight) * blocksPerRow + x / blockWidth;
    const auto inner = elementBytes == 1 ? blockOffset8(x, y) : elementBytes == 2 ? blockOffset16(x, y) : blockOffset32(x, y);
    return block * 65536u + (inner & 0xffffu);
}

std::size_t DepthTargetLayout::Offset(std::uint32_t x, std::uint32_t y) const {
    require(x < width && y < height, "AGC graphics: depth surface coordinate out of range");
    return offset(x, y);
}

void DepthTargetLayout::Detile(std::span<const std::byte> source, std::span<std::byte> destination) const {
    require(source.size() == Bytes() && destination.size() == LinearBytes(), "AGC graphics: depth detile buffer size mismatch");
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::memcpy(destination.data() + (static_cast<std::size_t>(y) * width + x) * elementBytes, source.data() + offset(x, y), elementBytes);
        }
    }
}

void DepthTargetLayout::Tile(std::span<const std::byte> source, std::span<std::byte> destination) const {
    require(source.size() == LinearBytes() && destination.size() == Bytes(), "AGC graphics: depth tile buffer size mismatch");
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::memcpy(destination.data() + offset(x, y), source.data() + (static_cast<std::size_t>(y) * width + x) * elementBytes, elementBytes);
        }
    }
}

}
