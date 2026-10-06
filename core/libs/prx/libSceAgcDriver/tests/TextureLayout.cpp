// Texture and colour surface layouts against addrlib (GFX10, Navi10 configuration): views past a
// surface's last mip level and 4 KiB standard colour targets.
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureAddressing.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include <array>
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

}

int main() {
    try {
        viewsPastLastMip();
        std::puts("AGC driver texture layout tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
