#include "prx/libSceAgcDriver/Graphics/include/TextureAddressing.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include <bit>
#include <limits>
#include <stdexcept>

namespace AgcDriver::Graphics {
namespace {

std::uint32_t standardOffset(std::uint32_t x, std::uint32_t y, std::uint32_t elementBytes) {
    switch (elementBytes) {
        case 1:
            return ((y << 4) & 0x1f0u) ^ ((y << 5) & 0x400u) ^ (x & 0x00fu) ^ ((x << 5) & 0x200u) ^ ((x << 6) & 0x800u);
        case 2:
            return ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^ ((x << 1) & 0x00eu) ^ ((x << 4) & 0x080u) ^ ((x << 5) & 0x200u) ^ ((x << 6) & 0x800u);
        case 4:
            return ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^ ((x << 2) & 0x00cu) ^ ((x << 5) & 0x080u) ^ ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u);
        case 8:
            return ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^ ((x << 3) & 0x008u) ^ ((x << 5) & 0x0c0u) ^ ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u);
        default:
            return ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^ ((x << 6) & 0x0c0u) ^ ((x << 7) & 0x200u) ^ ((x << 8) & 0x800u);
    }
}

std::uint32_t standard64Extra(std::uint32_t x, std::uint32_t y, std::uint32_t elementBytes) {
    switch (elementBytes) {
        case 1: return ((x << 7) & 0x2000u) ^ ((x << 8) & 0x8000u) ^ ((y << 6) & 0x1000u) ^ ((y << 7) & 0x4000u);
        case 2: return ((x << 7) & 0x2000u) ^ ((x << 8) & 0x8000u) ^ ((y << 7) & 0x1000u) ^ ((y << 8) & 0x4000u);
        case 4: return ((x << 8) & 0x2000u) ^ ((x << 9) & 0x8000u) ^ ((y << 7) & 0x1000u) ^ ((y << 8) & 0x4000u);
        case 8: return ((x << 8) & 0x2000u) ^ ((x << 9) & 0x8000u) ^ ((y << 8) & 0x1000u) ^ ((y << 9) & 0x4000u);
        default: return ((x << 9) & 0x2000u) ^ ((x << 10) & 0x8000u) ^ ((y << 8) & 0x1000u) ^ ((y << 9) & 0x4000u);
    }
}

std::uint32_t renderTargetOffset(std::uint32_t x, std::uint32_t y, std::uint32_t elementBytes, std::uint32_t z) {
    std::uint32_t offset = 0;
    switch (elementBytes) {
        case 1:
            offset = ((y << 2u) & 0x0008u) ^ ((y << 4u) & 0x0010u) ^ ((y << 3u) & 0x00a0u) ^ ((y << 5u) & 0x0f00u) ^ ((y << 6u) & 0x1000u) ^ ((y << 7u) & 0x4000u)
                ^ (x & 0x0007u) ^ ((x << 3u) & 0x0040u) ^ ((x << 5u) & 0x0300u) ^ ((x << 4u) & 0x0400u) ^ ((x << 6u) & 0x0800u) ^ ((x << 7u) & 0x2000u) ^ ((x << 8u) & 0x8000u);
            break;
        case 2:
            offset = ((y << 4u) & 0x0070u) ^ ((y << 5u) & 0x0f00u) ^ ((y << 8u) & 0x5000u)
                ^ ((x << 1u) & 0x000eu) ^ ((x << 4u) & 0x0480u) ^ ((x << 5u) & 0x0300u) ^ ((x << 6u) & 0x0800u) ^ ((x << 7u) & 0x2000u) ^ ((x << 8u) & 0x8000u);
            break;
        case 4:
            offset = ((y << 4u) & 0x0070u) ^ ((y << 5u) & 0x0f00u) ^ ((y << 9u) & 0x1000u) ^ ((y << 8u) & 0x4000u)
                ^ ((x << 2u) & 0x000cu) ^ ((x << 5u) & 0x0380u) ^ ((x << 4u) & 0x0400u) ^ ((x << 6u) & 0x0800u) ^ ((x << 9u) & 0xa000u);
            break;
        case 8:
            offset = ((y << 4u) & 0x0010u) ^ ((y << 6u) & 0x0080u) ^ ((y << 5u) & 0x0f00u) ^ ((y << 10u) & 0x5000u)
                ^ ((x << 3u) & 0x0008u) ^ ((x << 4u) & 0x0460u) ^ ((x << 5u) & 0x0300u) ^ ((x << 6u) & 0x0800u) ^ ((x << 10u) & 0x2000u) ^ ((x << 9u) & 0x8000u);
            break;
        case 16:
            offset = ((x << 4u) & 0x0410u) ^ ((x << 5u) & 0x0340u) ^ ((x << 6u) & 0x0800u) ^ ((x << 11u) & 0xa000u)
                ^ ((y << 5u) & 0x0f20u) ^ ((y << 6u) & 0x0080u) ^ ((y << 10u) & 0x1000u) ^ ((y << 11u) & 0x4000u);
            break;
        default: break;
    }
    return offset ^ ((z & 8u) << 5u) ^ ((z & 4u) << 7u) ^ ((z & 2u) << 9u) ^ ((z & 1u) << 11u);
}

std::uint32_t depthOffset(std::uint32_t x, std::uint32_t y, std::uint32_t elementBytes) {
    switch (elementBytes) {
        case 1:
            return (x & 1u) ^ ((x << 1u) & 0x4u) ^ ((x << 2u) & 0x10u) ^ ((x << 3u) & 0x40u) ^ ((x << 5u) & 0x300u) ^ ((x << 4u) & 0x400u) ^ ((x << 6u) & 0x800u) ^ ((x << 7u) & 0x2000u) ^ ((x << 8u) & 0x8000u)
                ^ ((y << 1u) & 0x2u) ^ ((y << 2u) & 0x8u) ^ ((y << 3u) & 0xa0u) ^ ((y << 5u) & 0xf00u) ^ ((y << 6u) & 0x1000u) ^ ((y << 7u) & 0x4000u);
        case 2:
            return ((x << 1u) & 0x2u) ^ ((x << 2u) & 0x8u) ^ ((x << 3u) & 0x20u) ^ ((x << 4u) & 0x480u) ^ ((x << 5u) & 0x300u) ^ ((x << 6u) & 0x800u) ^ ((x << 7u) & 0x2000u) ^ ((x << 8u) & 0x8000u)
                ^ ((y << 2u) & 0x4u) ^ ((y << 3u) & 0x10u) ^ ((y << 4u) & 0x40u) ^ ((y << 5u) & 0xf00u) ^ ((y << 8u) & 0x5000u);
        default:
            return ((x << 2u) & 0x4u) ^ ((x << 3u) & 0x10u) ^ ((x << 4u) & 0x440u) ^ ((x << 5u) & 0x300u) ^ ((x << 6u) & 0x800u) ^ ((x << 9u) & 0xa000u)
                ^ ((y << 3u) & 0x8u) ^ ((y << 4u) & 0x20u) ^ ((y << 5u) & 0xf80u) ^ ((y << 9u) & 0x1000u) ^ ((y << 8u) & 0x4000u);
    }
}

std::uint32_t standard4KBVolumeOffset(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t elementBytes) {
    switch (elementBytes) {
        case 1:
            return (x & 0x3u) ^ ((x << 4u) & 0x40u) ^ ((x << 6u) & 0x200u) ^ ((y << 3u) & 0x8u) ^ ((y << 4u) & 0x20u) ^ ((y << 6u) & 0x100u) ^ ((y << 8u) & 0x800u)
                ^ ((z << 2u) & 0x4u) ^ ((z << 3u) & 0x10u) ^ ((z << 5u) & 0x80u) ^ ((z << 7u) & 0x400u);
        case 2:
            return ((x << 1u) & 0x2u) ^ ((x << 5u) & 0x40u) ^ ((x << 7u) & 0x200u) ^ ((y << 3u) & 0x8u) ^ ((y << 4u) & 0x20u) ^ ((y << 6u) & 0x100u) ^ ((y << 8u) & 0x800u)
                ^ ((z << 2u) & 0x4u) ^ ((z << 3u) & 0x10u) ^ ((z << 5u) & 0x80u) ^ ((z << 7u) & 0x400u);
        case 4:
            return ((x << 2u) & 0x4u) ^ ((x << 5u) & 0x40u) ^ ((x << 7u) & 0x200u) ^ ((y << 3u) & 0x8u) ^ ((y << 4u) & 0x20u) ^ ((y << 6u) & 0x100u) ^ ((y << 8u) & 0x800u)
                ^ ((z << 4u) & 0x10u) ^ ((z << 6u) & 0x80u) ^ ((z << 8u) & 0x400u);
        case 8:
            return ((x << 3u) & 0x8u) ^ ((x << 5u) & 0x40u) ^ ((x << 7u) & 0x200u) ^ ((y << 5u) & 0x20u) ^ ((y << 7u) & 0x100u) ^ ((y << 9u) & 0x800u)
                ^ ((z << 4u) & 0x10u) ^ ((z << 6u) & 0x80u) ^ ((z << 8u) & 0x400u);
        default:
            return ((x << 6u) & 0x40u) ^ ((x << 8u) & 0x200u) ^ ((y << 5u) & 0x20u) ^ ((y << 7u) & 0x100u) ^ ((y << 9u) & 0x800u)
                ^ ((z << 4u) & 0x10u) ^ ((z << 6u) & 0x80u) ^ ((z << 8u) & 0x400u);
    }
}

std::uint32_t bit(std::uint32_t value, std::uint32_t source, std::uint32_t destination) {
    return ((value >> source) & 1u) << destination;
}

std::uint32_t standard64KBVolumeOffset(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t elementBytes) {
    static constexpr std::uint8_t sources[5][4] = {{4, 4, 4, 5}, {3, 4, 4, 4}, {3, 3, 4, 4}, {3, 3, 3, 4}, {2, 3, 3, 3}};
    const auto* bits = sources[std::countr_zero(elementBytes)];
    return standard4KBVolumeOffset(x, y, z, elementBytes) ^ bit(x, bits[0], 12) ^ bit(z, bits[1], 13) ^ bit(y, bits[2], 14) ^ bit(x, bits[3], 15);
}

struct ThickBlock {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
    std::uint32_t bytes;
};

ThickBlock thickBlock(TextureTileMode mode, std::uint32_t elementBytes) {
    struct Log2 { std::uint8_t width, height, depth; };
    static constexpr Log2 thick4KB[5] = {{4, 4, 4}, {3, 4, 4}, {3, 4, 3}, {3, 3, 3}, {2, 3, 3}};
    static constexpr Log2 thick64KB[5] = {{6, 5, 5}, {5, 5, 5}, {5, 5, 4}, {5, 4, 4}, {4, 4, 4}};
    if (elementBytes == 0 || elementBytes > 16 || (elementBytes & (elementBytes - 1u)) != 0) throw std::runtime_error("AGC graphics: unsupported thick volume element size");
    const auto index = static_cast<std::size_t>(std::countr_zero(elementBytes));
    const auto& shape = mode == TextureTileMode::kStandard4KB ? thick4KB[index] : thick64KB[index];
    return {1u << shape.width, 1u << shape.height, 1u << shape.depth, mode == TextureTileMode::kStandard4KB ? 4096u : 65536u};
}

std::uint32_t blockBytes(TextureTileMode mode) {
    switch (mode) {
        case TextureTileMode::kStandard256B: return 256u;
        case TextureTileMode::kStandard4KB: return 4096u;
        case TextureTileMode::kStandard64KB:
        case TextureTileMode::RenderTarget64KB:
        case TextureTileMode::Depth64KB: return 65536u;
        case TextureTileMode::kLinear: break;
    }
    throw std::runtime_error("AGC graphics: TexelOffset needs a tiled mode");
}

}

std::uint64_t TexelOffset(TextureTileMode mode, std::uint32_t elementBytes, const TileMipLayout& mip, std::uint32_t x, std::uint32_t y, std::uint32_t arrayLayer) {
    if (mode == TextureTileMode::kLinear) return mip.tiledOffset + static_cast<std::uint64_t>(y) * mip.pitchBytes + static_cast<std::uint64_t>(x) * elementBytes;
    const auto bytes = blockBytes(mode);
    const auto width = bytes <= 256u ? (elementBytes <= 2u ? 16u : elementBytes <= 8u ? 8u : 4u) : bytes <= 4096u ? (elementBytes <= 2u ? 64u : elementBytes <= 8u ? 32u : 16u) : (elementBytes <= 2u ? 256u : elementBytes <= 8u ? 128u : 64u);
    const auto height = bytes / (width * elementBytes);
    std::uint32_t sx = x;
    std::uint32_t sy = y;
    std::uint64_t block = 0;
    if (mip.tail) {
        sx += mip.tailX;
        sy += mip.tailY;
    } else {
        block = static_cast<std::uint64_t>(y / height) * mip.blocksPerRow + x / width;
    }
    std::uint32_t inner = 0;
    if (mode == TextureTileMode::RenderTarget64KB) {
        inner = renderTargetOffset(sx, sy, elementBytes, arrayLayer);
    } else if (mode == TextureTileMode::Depth64KB) {
        inner = depthOffset(sx, sy, elementBytes) & 0xffffu;
    } else {
        inner = standardOffset(sx, sy, elementBytes);
        if (bytes > 4096u) inner ^= standard64Extra(sx, sy, elementBytes);
        inner &= bytes - 1u;
    }
    return mip.tiledOffset + block * bytes + inner;
}

bool IsThickVolume(const GuestTextureResource& resource) {
    return resource.dimension == TextureDimension::k3D && (resource.tileMode == TextureTileMode::kStandard4KB || resource.tileMode == TextureTileMode::kStandard64KB);
}

std::uint64_t ThickVolumeOffset(TextureTileMode mode, std::uint32_t elementBytes, std::uint32_t width, std::uint32_t height, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    const auto block = thickBlock(mode, elementBytes);
    const auto columns = static_cast<std::uint64_t>((width + block.width - 1u) / block.width);
    const auto rows = static_cast<std::uint64_t>((height + block.height - 1u) / block.height);
    const auto index = (static_cast<std::uint64_t>(z / block.depth) * rows + y / block.height) * columns + x / block.width;
    const auto inner = mode == TextureTileMode::kStandard4KB ? standard4KBVolumeOffset(x % block.width, y % block.height, z % block.depth, elementBytes) : standard64KBVolumeOffset(x % block.width, y % block.height, z % block.depth, elementBytes);
    return index * block.bytes + inner;
}

std::uint64_t GuestTextureBytes(const GuestTextureResource& resource) {
    const auto slices = resource.dimension == TextureDimension::k2DArray || resource.dimension == TextureDimension::kCube || resource.dimension == TextureDimension::k3D ? resource.depthOrLastArray + 1u : 1u;
    if (IsThickVolume(resource)) {
        const auto block = thickBlock(resource.tileMode, BytesPerElement(resource.format));
        const auto columns = static_cast<std::uint64_t>((resource.width + block.width - 1u) / block.width);
        const auto rows = static_cast<std::uint64_t>((resource.height + block.height - 1u) / block.height);
        const auto depth = static_cast<std::uint64_t>((slices + block.depth - 1u) / block.depth);
        return columns * rows * depth * block.bytes;
    }
    const auto mips = ComputeMipLayout(resource.tileMode, resource.format, resource.width, resource.height, resource.mipCount);
    return ComputeSurfaceSize(mips, slices);
}

}
