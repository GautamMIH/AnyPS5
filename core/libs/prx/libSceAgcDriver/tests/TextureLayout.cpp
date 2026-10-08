// Texture and colour surface layouts against addrlib (GFX10, Navi10 configuration): views past a
// surface's last mip level and 4 KiB standard colour targets.
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureAddressing.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include <array>
#include <span>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
void reject(TAction action, std::string_view reason) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        Require(std::string_view(error.what()).find(reason) != std::string_view::npos, std::string("unexpected rejection: ") + error.what());
        return;
    }
    throw std::runtime_error(std::string("expected rejection: ") + std::string(reason));
}

struct Descriptor {
    std::uint64_t base40 = 0x55ea000ull;
    std::uint32_t format = 56;
    std::uint32_t width = 16;
    std::uint32_t height = 16;
    std::uint32_t baseLevel = 0;
    std::uint32_t lastLevel = 0;
    std::uint32_t tileModeRaw = 0;
    std::uint32_t typeRaw = 9;
    std::uint32_t depth = 0;
    std::uint32_t maxMip = 0;
};

std::array<std::uint32_t, 8> pack(const Descriptor& d) {
    std::array<std::uint32_t, 8> words{};
    const auto width = d.width - 1u;
    const auto height = d.height - 1u;
    words[0] = static_cast<std::uint32_t>(d.base40);
    words[1] = static_cast<std::uint32_t>((d.base40 >> 32u) & 0xffu) | ((d.format & 0x1ffu) << 20u) | ((width & 3u) << 30u);
    words[2] = ((width >> 2u) & 0xfffu) | ((height & 0x3fffu) << 14u);
    words[3] = 4u | (5u << 3u) | (6u << 6u) | (7u << 9u) | ((d.baseLevel & 0xfu) << 12u) | ((d.lastLevel & 0xfu) << 16u) | ((d.tileModeRaw & 0x1fu) << 20u) | ((d.typeRaw & 0xfu) << 28u);
    words[4] = d.depth & 0x1fffu;
    words[5] = (d.maxMip & 0xfu) << 4u;
    return words;
}

GuestTextureResource decode(const Descriptor& d) {
    const auto words = pack(d);
    return DecodeTextureResource(words);
}

// Views past MAX_MIP (upstream 3d3e6879; the layouts are addrlib's for these surfaces).
struct ElementAddress {
    std::uint32_t mip;
    std::uint32_t x;
    std::uint32_t y;
    std::uint32_t z;
    std::uint64_t address;
};

// Upstream's addrlib reference addresses for the display, T and 4 KiB XOR swizzles (ace10fdb),
// checked through this driver's mip layout and TexelOffset (the CPU counterpart of the detiler's
// equation family): slices are whole mip chains laid end to end.
void requireThinAddresses(TextureTileMode tileMode, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t mipCount, std::uint32_t slices, std::uint64_t guestBytes, std::uint64_t layerBytes, std::span<const ElementAddress> expected, const std::string& what) {
    const auto mips = ComputeMipLayout(tileMode, format, width, height, mipCount);
    const auto sliceBytes = ComputeSurfaceSize(mips, 1);
    Require(sliceBytes * slices == guestBytes && sliceBytes == layerBytes, what + ": surface or slice size differs from addrlib");
    const auto bytesPerElement = BytesPerElement(format);
    for (const auto& element : expected) {
        const auto address = element.z * sliceBytes + TexelOffset(tileMode, bytesPerElement, mips.at(element.mip), element.x, element.y, element.z);
        Require(address == element.address, what + ": mip " + std::to_string(element.mip) + " element (" + std::to_string(element.x) + ", " + std::to_string(element.y) + ", " + std::to_string(element.z) + ") detiles from " + std::to_string(address) + " instead of addrlib's " + std::to_string(element.address));
    }
}

// Upstream's addrlib reference addresses for the display, T and 4 KiB XOR swizzles (ace10fdb).
// Mipmapped volumes (upstream's thick and Z_X volume chains) are not modelled by this driver; the
// single-level XOR volumes are covered by the ImageXorSwizzleVolume execution test.
void equationSwizzles() {
    constexpr ElementAddress atD256B[] = {{0, 0, 0, 0, 0x300}, {0, 39, 19, 0, 0x81f}, {0, 20, 6, 0, 0x42c}, {0, 39, 0, 0, 0x507}, {0, 0, 19, 0, 0x618}, {1, 0, 0, 0, 0x100}, {1, 19, 9, 0, 0x293}, {1, 10, 3, 0, 0x15a}, {1, 19, 0, 0, 0x203}, {1, 0, 9, 0, 0x190}, {2, 0, 0, 0, 0x0}, {2, 9, 4, 0, 0x61}, {2, 5, 1, 0, 0x15}, {2, 9, 0, 0, 0x41}, {2, 0, 4, 0, 0x20}};
    requireThinAddresses(TextureTileMode::kD256B, 1, 40, 20, 3, 1, 2304, 2304, atD256B, "swizzle mode 2, 1 bytes, 40x20, 3 levels, 1 slices");
    constexpr ElementAddress atD4KB[] = {{0, 0, 0, 0, 0x6000}, {0, 99, 59, 0, 0x154b8}, {0, 50, 20, 0, 0xb920}, {0, 99, 0, 0, 0x9028}, {0, 0, 59, 0, 0x12490}, {1, 0, 0, 0, 0x2000}, {1, 49, 29, 0, 0x5d18}, {1, 25, 10, 0, 0x2e88}, {1, 49, 0, 0, 0x3808}, {1, 0, 29, 0, 0x4510}, {2, 0, 0, 0, 0x1000}, {2, 24, 14, 0, 0x1f80}, {2, 12, 5, 0, 0x1350}, {2, 24, 0, 0, 0x1a00}, {2, 0, 14, 0, 0x1580}, {3, 0, 0, 0, 0x800}, {3, 11, 6, 0, 0xba8}, {3, 6, 2, 0, 0x8e0}, {3, 11, 0, 0, 0xa28}, {3, 0, 6, 0, 0x980}, {4, 0, 0, 0, 0x600}, {4, 5, 2, 0, 0x6c8}, {4, 3, 1, 0, 0x638}, {4, 5, 0, 0, 0x648}, {4, 0, 2, 0, 0x680}, {5, 0, 0, 0, 0x500}, {5, 2, 0, 0, 0x520}, {5, 1, 0, 0, 0x508}, {5, 2, 0, 0, 0x520}, {5, 0, 0, 0, 0x500}, {6, 0, 0, 0, 0x400}, {6, 0, 0, 0, 0x400}, {6, 0, 0, 0, 0x400}, {6, 0, 0, 0, 0x400}, {6, 0, 0, 0, 0x400}};
    requireThinAddresses(TextureTileMode::kD4KB, 71, 100, 60, 7, 1, 90112, 90112, atD4KB, "swizzle mode 6, 8 bytes, 100x60, 7 levels, 1 slices");
    constexpr ElementAddress atD64KB[] = {{0, 0, 0, 0, 0x20000}, {0, 199, 149, 0, 0x584dc}, {0, 100, 50, 0, 0x2b4a0}, {0, 199, 0, 0, 0x3808c}, {0, 0, 149, 0, 0x40450}, {1, 0, 0, 0, 0x10000}, {1, 99, 74, 0, 0x1e12c}, {1, 50, 25, 0, 0x12d18}, {1, 99, 0, 0, 0x1a00c}, {1, 0, 74, 0, 0x14120}, {2, 0, 0, 0, 0x8000}, {2, 49, 36, 0, 0xb844}, {2, 25, 12, 0, 0x8b44}, {2, 49, 0, 0, 0xa804}, {2, 0, 36, 0, 0x9040}, {3, 0, 0, 0, 0x4000}, {3, 24, 17, 0, 0x4e10}, {3, 12, 6, 0, 0x42e0}, {3, 24, 0, 0, 0x4a00}, {3, 0, 17, 0, 0x4410}, {4, 0, 0, 0, 0x2000}, {4, 11, 8, 0, 0x230c}, {4, 6, 3, 0, 0x20b8}, {4, 11, 0, 0, 0x220c}, {4, 0, 8, 0, 0x2100}, {5, 0, 0, 0, 0x1000}, {5, 5, 3, 0, 0x10b4}, {5, 3, 1, 0, 0x101c}, {5, 5, 0, 0, 0x1084}, {5, 0, 3, 0, 0x1030}};
    requireThinAddresses(TextureTileMode::kD64KB, 56, 200, 150, 6, 1, 393216, 393216, atD64KB, "swizzle mode 10, 4 bytes, 200x150, 6 levels, 1 slices");
    constexpr ElementAddress atS64KBT[] = {{0, 0, 0, 0, 0x90000}, {0, 519, 259, 0, 0x1700bc}, {0, 260, 86, 0, 0xb46e0}, {0, 519, 0, 0, 0xd008c}, {0, 0, 259, 0, 0x130030}, {0, 0, 0, 1, 0x210000}, {0, 519, 259, 1, 0x2f00bc}, {0, 260, 86, 1, 0x2346e0}, {0, 519, 0, 1, 0x25008c}, {0, 0, 259, 1, 0x2b0030}, {0, 0, 0, 2, 0x390000}, {0, 519, 259, 2, 0x4700bc}, {0, 260, 86, 2, 0x3b46e0}, {0, 519, 0, 2, 0x3d008c}, {0, 0, 259, 2, 0x430030}, {1, 0, 0, 0, 0x30000}, {1, 259, 129, 0, 0x8001c}, {1, 130, 43, 0, 0x41938}, {1, 259, 0, 0, 0x5000c}, {1, 0, 129, 0, 0x60010}, {1, 0, 0, 1, 0x1b0000}, {1, 259, 129, 1, 0x20001c}, {1, 130, 43, 1, 0x1c1938}, {1, 259, 0, 1, 0x1d000c}, {1, 0, 129, 1, 0x1e0010}, {1, 0, 0, 2, 0x330000}, {1, 259, 129, 2, 0x38001c}, {1, 130, 43, 2, 0x341938}, {1, 259, 0, 2, 0x35000c}, {1, 0, 129, 2, 0x360010}, {2, 0, 0, 0, 0x10000}, {2, 129, 64, 0, 0x24204}, {2, 65, 21, 0, 0x18554}, {2, 129, 0, 0, 0x20004}, {2, 0, 64, 0, 0x14200}, {2, 0, 0, 1, 0x190000}, {2, 129, 64, 1, 0x1a4204}, {2, 65, 21, 1, 0x198554}, {2, 129, 0, 1, 0x1a0004}, {2, 0, 64, 1, 0x194200}, {2, 0, 0, 2, 0x310000}, {2, 129, 64, 2, 0x324204}, {2, 65, 21, 2, 0x318554}, {2, 129, 0, 2, 0x320004}, {2, 0, 64, 2, 0x314200}, {3, 0, 0, 0, 0x0}, {3, 64, 31, 0, 0x8470}, {3, 32, 10, 0, 0x2520}, {3, 64, 0, 0, 0x8100}, {3, 0, 31, 0, 0x570}, {3, 0, 0, 1, 0x180000}, {3, 64, 31, 1, 0x188470}, {3, 32, 10, 1, 0x182520}, {3, 64, 0, 1, 0x188100}, {3, 0, 31, 1, 0x180570}, {3, 0, 0, 2, 0x300000}, {3, 64, 31, 2, 0x308470}, {3, 32, 10, 2, 0x302520}, {3, 64, 0, 2, 0x308100}, {3, 0, 31, 2, 0x300570}};
    requireThinAddresses(TextureTileMode::kS64KBT, 56, 520, 260, 4, 3, 4718592, 1572864, atS64KBT, "swizzle mode 17, 4 bytes, 520x260, 4 levels, 3 slices");
    constexpr ElementAddress atD64KBT[] = {{0, 0, 0, 0, 0x10000}, {0, 63, 39, 0, 0x1ecf0}, {0, 32, 13, 0, 0x18420}, {0, 63, 0, 0, 0x1af50}, {0, 0, 39, 0, 0x143a0}, {0, 0, 0, 1, 0x30000}, {0, 63, 39, 1, 0x3ecf0}, {0, 32, 13, 1, 0x38420}, {0, 63, 0, 1, 0x3af50}, {0, 0, 39, 1, 0x343a0}, {1, 0, 0, 0, 0x8100}, {1, 31, 19, 0, 0xb7f0}, {1, 16, 6, 0, 0xa480}, {1, 31, 0, 0, 0xaf50}, {1, 0, 19, 0, 0x99a0}, {1, 0, 0, 1, 0x28100}, {1, 31, 19, 1, 0x2b7f0}, {1, 16, 6, 1, 0x2a480}, {1, 31, 0, 1, 0x2af50}, {1, 0, 19, 1, 0x299a0}, {2, 0, 0, 0, 0x4200}, {2, 15, 9, 0, 0x4c70}, {2, 8, 3, 0, 0x4aa0}, {2, 15, 0, 0, 0x4850}, {2, 0, 9, 0, 0x4620}, {2, 0, 0, 1, 0x24200}, {2, 15, 9, 1, 0x24c70}, {2, 8, 3, 1, 0x24aa0}, {2, 15, 0, 1, 0x24850}, {2, 0, 9, 1, 0x24620}, {3, 0, 0, 0, 0x2400}, {3, 7, 4, 0, 0x2750}, {3, 4, 1, 0, 0x2620}, {3, 7, 0, 0, 0x2650}, {3, 0, 4, 0, 0x2500}, {3, 0, 0, 1, 0x22400}, {3, 7, 4, 1, 0x22750}, {3, 4, 1, 1, 0x22620}, {3, 7, 0, 1, 0x22650}, {3, 0, 4, 1, 0x22500}, {4, 0, 0, 0, 0x1800}, {4, 3, 1, 0, 0x1870}, {4, 2, 0, 0, 0x1840}, {4, 3, 0, 0, 0x1850}, {4, 0, 1, 0, 0x1820}, {4, 0, 0, 1, 0x21800}, {4, 3, 1, 1, 0x21870}, {4, 2, 0, 1, 0x21840}, {4, 3, 0, 1, 0x21850}, {4, 0, 1, 1, 0x21820}};
    requireThinAddresses(TextureTileMode::kD64KBT, 77, 64, 40, 5, 2, 262144, 131072, atD64KBT, "swizzle mode 18, 16 bytes, 64x40, 5 levels, 2 slices");
    constexpr ElementAddress atS4KBX[] = {{0, 0, 0, 0, 0x2000}, {0, 99, 69, 0, 0x5453}, {0, 50, 23, 0, 0x2b72}, {0, 99, 0, 0, 0x3c03}, {0, 0, 69, 0, 0x4850}, {0, 0, 0, 1, 0x8800}, {0, 99, 69, 1, 0xbc53}, {0, 50, 23, 1, 0x8372}, {0, 99, 0, 1, 0x9403}, {0, 0, 69, 1, 0xa050}, {0, 0, 0, 2, 0xe400}, {0, 99, 69, 2, 0x11053}, {0, 50, 23, 2, 0xef72}, {0, 99, 0, 2, 0xf803}, {0, 0, 69, 2, 0x10c50}, {1, 0, 0, 0, 0x1000}, {1, 49, 34, 0, 0x1e21}, {1, 25, 11, 0, 0x12b9}, {1, 49, 0, 0, 0x1a01}, {1, 0, 34, 0, 0x1420}, {1, 0, 0, 1, 0x7800}, {1, 49, 34, 1, 0x7621}, {1, 25, 11, 1, 0x7ab9}, {1, 49, 0, 1, 0x7201}, {1, 0, 34, 1, 0x7c20}, {1, 0, 0, 2, 0xd400}, {1, 49, 34, 2, 0xda21}, {1, 25, 11, 2, 0xd6b9}, {1, 49, 0, 2, 0xde01}, {1, 0, 34, 2, 0xd020}, {2, 0, 0, 0, 0x800}, {2, 24, 16, 0, 0xb08}, {2, 12, 5, 0, 0x85c}, {2, 24, 0, 0, 0xa08}, {2, 0, 16, 0, 0x900}, {2, 0, 0, 1, 0x6000}, {2, 24, 16, 1, 0x6308}, {2, 12, 5, 1, 0x605c}, {2, 24, 0, 1, 0x6208}, {2, 0, 16, 1, 0x6100}, {2, 0, 0, 2, 0xcc00}, {2, 24, 16, 2, 0xcf08}, {2, 12, 5, 2, 0xcc5c}, {2, 24, 0, 2, 0xce08}, {2, 0, 16, 2, 0xcd00}, {3, 0, 0, 0, 0x600}, {3, 11, 7, 0, 0x67b}, {3, 6, 2, 0, 0x626}, {3, 11, 0, 0, 0x60b}, {3, 0, 7, 0, 0x670}, {3, 0, 0, 1, 0x6e00}, {3, 11, 7, 1, 0x6e7b}, {3, 6, 2, 1, 0x6e26}, {3, 11, 0, 1, 0x6e0b}, {3, 0, 7, 1, 0x6e70}, {3, 0, 0, 2, 0xc200}, {3, 11, 7, 2, 0xc27b}, {3, 6, 2, 2, 0xc226}, {3, 11, 0, 2, 0xc20b}, {3, 0, 7, 2, 0xc270}, {4, 0, 0, 0, 0x500}, {4, 5, 3, 0, 0x535}, {4, 3, 1, 0, 0x513}, {4, 5, 0, 0, 0x505}, {4, 0, 3, 0, 0x530}, {4, 0, 0, 1, 0x6d00}, {4, 5, 3, 1, 0x6d35}, {4, 3, 1, 1, 0x6d13}, {4, 5, 0, 1, 0x6d05}, {4, 0, 3, 1, 0x6d30}, {4, 0, 0, 2, 0xc100}, {4, 5, 3, 2, 0xc135}, {4, 3, 1, 2, 0xc113}, {4, 5, 0, 2, 0xc105}, {4, 0, 3, 2, 0xc130}, {5, 0, 0, 0, 0x400}, {5, 2, 1, 0, 0x412}, {5, 1, 0, 0, 0x401}, {5, 2, 0, 0, 0x402}, {5, 0, 1, 0, 0x410}, {5, 0, 0, 1, 0x6c00}, {5, 2, 1, 1, 0x6c12}, {5, 1, 0, 1, 0x6c01}, {5, 2, 0, 1, 0x6c02}, {5, 0, 1, 1, 0x6c10}, {5, 0, 0, 2, 0xc000}, {5, 2, 1, 2, 0xc012}, {5, 1, 0, 2, 0xc001}, {5, 2, 0, 2, 0xc002}, {5, 0, 1, 2, 0xc010}};
    requireThinAddresses(TextureTileMode::kS4KBX, 1, 100, 70, 6, 3, 73728, 24576, atS4KBX, "swizzle mode 21, 1 bytes, 100x70, 6 levels, 3 slices");
    constexpr ElementAddress atD4KBX[] = {{0, 0, 0, 0, 0x2000}, {0, 32, 19, 0, 0x3030}, {0, 16, 6, 0, 0x2860}, {0, 32, 0, 0, 0x3400}, {0, 0, 19, 0, 0x2430}, {0, 0, 0, 1, 0x6800}, {0, 32, 19, 1, 0x7830}, {0, 16, 6, 1, 0x6060}, {0, 32, 0, 1, 0x7c00}, {0, 0, 19, 1, 0x6c30}, {1, 0, 0, 0, 0x1000}, {1, 15, 9, 0, 0x139c}, {1, 8, 3, 0, 0x1230}, {1, 15, 0, 0, 0x128c}, {1, 0, 9, 0, 0x1110}, {1, 0, 0, 1, 0x5800}, {1, 15, 9, 1, 0x5b9c}, {1, 8, 3, 1, 0x5a30}, {1, 15, 0, 1, 0x5a8c}, {1, 0, 9, 1, 0x5910}, {2, 0, 0, 0, 0x800}, {2, 7, 4, 0, 0x8cc}, {2, 4, 1, 0, 0x890}, {2, 7, 0, 0, 0x88c}, {2, 0, 4, 0, 0x840}, {2, 0, 0, 1, 0x4000}, {2, 7, 4, 1, 0x40cc}, {2, 4, 1, 1, 0x4090}, {2, 7, 0, 1, 0x408c}, {2, 0, 4, 1, 0x4040}, {3, 0, 0, 0, 0x600}, {3, 3, 1, 0, 0x61c}, {3, 2, 0, 0, 0x608}, {3, 3, 0, 0, 0x60c}, {3, 0, 1, 0, 0x610}, {3, 0, 0, 1, 0x4e00}, {3, 3, 1, 1, 0x4e1c}, {3, 2, 0, 1, 0x4e08}, {3, 3, 0, 1, 0x4e0c}, {3, 0, 1, 1, 0x4e10}};
    requireThinAddresses(TextureTileMode::kD4KBX, 56, 33, 20, 4, 2, 32768, 16384, atD4KBX, "swizzle mode 22, 4 bytes, 33x20, 4 levels, 2 slices");
}

void viewsPastLastMip() {
    Descriptor bloom;
    bloom.format = 71;  // RGBA16F, 8 bytes per element
    bloom.width = 1920;
    bloom.height = 1080;
    bloom.tileModeRaw = 0x1b;
    bloom.maxMip = 5;
    bloom.baseLevel = 6;
    bloom.lastLevel = 6;
    const auto view = decode(bloom);
    Require(view.baseLevel == 6 && view.lastLevel == 6 && view.mipCount == 7, "a view one level past the last mip must address that level of the chain");
    const auto allocated = ComputeMipLayout(TextureTileMode::RenderTarget64KB, 71, 1920, 1080, 6);
    const auto extended = ComputeMipLayout(TextureTileMode::RenderTarget64KB, 71, 1920, 1080, 7);
    Require(ComputeSurfaceSize(allocated, 1) == 0x1640000u && ComputeSurfaceSize(extended, 1) == 0x1640000u, "a 1920x1080 64 bpp SW_64KB_R_X chain must take addrlib's 0x1640000 bytes with 6 and with 7 levels");
    constexpr std::array<std::uint64_t, 7> addrlibOffsets{0x650000u, 0x1d0000u, 0x90000u, 0x30000u, 0x10000u, 0u, 0u};
    for (std::uint32_t level = 0; level < 7; ++level) {
        Require(extended[level].tiledOffset == addrlibOffsets[level] && extended[level].tail == (level >= 5), "the 7-level chain must place level " + std::to_string(level) + " at its addrlib offset");
        if (level < 6) Require(allocated[level].tiledOffset == addrlibOffsets[level] && allocated[level].tail == extended[level].tail && allocated[level].tailX == extended[level].tailX && allocated[level].tailY == extended[level].tailY, "the level past the last mip must not move the allocated levels");
    }
    Require(extended[5].tailX == 64 && extended[5].tailY == 0, "the last allocated level must sit in its addrlib tail slot");
    Require(extended[6].tailX == 0 && extended[6].tailY == 32, "the level past the last mip must sit in its addrlib tail slot");
    Require(GuestTextureBytes(view) == 0x1640000u, "the view past the last mip must cover the surface's own bytes");

    bloom.lastLevel = 8;
    const auto deeper = decode(bloom);
    Require(deeper.mipCount == 9 && deeper.lastLevel == 8, "a view past the last mip must cover every level it names");
    const auto deeperMips = ComputeMipLayout(TextureTileMode::RenderTarget64KB, 71, 1920, 1080, 9);
    Require(ComputeSurfaceSize(deeperMips, 1) == 0x1640000u && deeperMips[7].tail && deeperMips[7].tailX == 32 && deeperMips[7].tailY == 0 && deeperMips[8].tail && deeperMips[8].tailX == 0 && deeperMips[8].tailY == 16, "levels 7 and 8 must sit in their addrlib tail slots");

    // A view that starts inside the surface ends at its last level.
    bloom.baseLevel = 2;
    bloom.lastLevel = 9;
    const auto clamped = decode(bloom);
    Require(clamped.mipCount == 6 && clamped.baseLevel == 2 && clamped.lastLevel == 5, "a view naming levels past MAX_MIP from inside the surface must end at its last level");

    // Linear chains place the smallest level first: another level moves the others.
    Descriptor linear;
    linear.maxMip = 1;
    linear.baseLevel = 2;
    linear.lastLevel = 2;
    reject([&] { decode(linear); }, "would move the surface's own");
    Require(ComputeSurfaceSize(ComputeMipLayout(TextureTileMode::kLinear, 56, 16, 16, 2), 1) == 0x1800u && ComputeSurfaceSize(ComputeMipLayout(TextureTileMode::kLinear, 56, 16, 16, 3), 1) == 0x1c00u, "a third level must grow a 16x16 32 bpp linear chain from addrlib's 0x1800 to 0x1c00 bytes");

    // A single-level surface has no mip tail: a second level grows it.
    Descriptor untailed = bloom;
    untailed.maxMip = 0;
    untailed.baseLevel = 1;
    untailed.lastLevel = 1;
    reject([&] { decode(untailed); }, "would move the surface's own");
    Require(ComputeSurfaceSize(ComputeMipLayout(TextureTileMode::RenderTarget64KB, 71, 1920, 1080, 1), 1) == 0xff0000u && ComputeSurfaceSize(ComputeMipLayout(TextureTileMode::RenderTarget64KB, 71, 1920, 1080, 2), 1) == 0x1470000u, "a second level must grow a 1920x1080 64 bpp SW_64KB_R_X surface from addrlib's 0xff0000 to 0x1470000 bytes");
    Require(!MipLevelsFitAllocation(TextureTileMode::Depth64KB, 56, 64, 64, 1, 2), "a depth surface has no levels past its last");
}

// SW_4KB_S colour targets (upstream d7d7c142) address every level of a chain, mip-tail levels
// included, where the texture path (TexelOffset, checked against addrlib) puts them; SW_64KB_R_X
// targets likewise.
void standardColorTargets() {
    Require(DecodeColorTileMode(0x4dc14000u) == ColorTileMode::Standard4KB, "a SW_4KB_S colour descriptor was rejected");
    reject([] { DecodeColorTileMode(0x4dc24000u); }, "unsupported color tile mode");
    for (const auto mode : {ColorTileMode::Standard4KB, ColorTileMode::RenderTarget}) {
        const auto tileMode = ColorTextureTileMode(mode);
        for (const std::uint32_t elementBytes : {1u, 2u, 4u, 8u, 16u}) {
            const auto what = std::string(mode == ColorTileMode::Standard4KB ? "SW_4KB_S" : "SW_64KB_R_X") + " " + std::to_string(elementBytes) + " B";
            const auto single = ComputeElementMipLayout(tileMode, elementBytes, 100, 70, 1);
            const ColorTargetLayout whole(100, 70, mode, elementBytes);
            Require(whole.Bytes() == ComputeSurfaceSize(single, 1), what + ": a 100x70 target's size differs from the texture layout");
            for (std::uint32_t y = 0; y < 70; ++y) {
                for (std::uint32_t x = 0; x < 100; ++x) Require(whole.Offset(x, y) == TexelOffset(tileMode, elementBytes, single[0], x, y), what + ": a 100x70 target's element differs from the texture layout");
            }
            const auto mips = ComputeElementMipLayout(tileMode, elementBytes, 256, 256, 9);
            for (std::uint32_t level = 0; level < mips.size(); ++level) {
                const auto& mip = mips[level];
                const ColorTail tail = mip.tail ? ColorTail{true, mip.tailX, mip.tailY} : ColorTail{};
                const ColorTargetLayout layout(mip.width, mip.height, mode, elementBytes, tail);
                Require(layout.Bytes() == mip.tiledSize, what + ": mip " + std::to_string(level) + " size differs from the texture layout");
                for (std::uint32_t y = 0; y < mip.height; ++y) {
                    for (std::uint32_t x = 0; x < mip.width; ++x) Require(mip.tiledOffset + layout.Offset(x, y) == TexelOffset(tileMode, elementBytes, mip, x, y), what + ": mip " + std::to_string(level) + " element (" + std::to_string(x) + ", " + std::to_string(y) + ") differs from the texture layout");
                }
            }
        }
    }
    const ColorTargetLayout standard(256, 256, ColorTileMode::Standard4KB);
    Require(standard.Alignment() == 4096u && standard.Offset(32, 0) == 4096u && standard.Offset(0, 32) == 8u * 4096u && standard.Offset(1, 0) == 4u && standard.Offset(0, 1) == 16u, "SW_4KB_S colour block or element addressing is wrong");
    // Thin 4 KiB blocks are 64x64, 64x32, 32x32, 32x16 and 16x16 elements (wide before tall).
    Require(ColorTargetLayout(64, 32, ColorTileMode::Standard4KB, 2).Bytes() == 4096u && ColorTargetLayout(32, 16, ColorTileMode::Standard4KB, 8).Bytes() == 4096u, "SW_4KB_S blocks of 2- and 8-byte elements have the wrong shape");
}

}

int main() {
    try {
        viewsPastLastMip();
        equationSwizzles();
        standardColorTargets();
        std::puts("AGC driver texture layout tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
