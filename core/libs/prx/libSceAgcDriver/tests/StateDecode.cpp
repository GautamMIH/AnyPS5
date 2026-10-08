// Draw-state decoding ported from upstream's state tests (DecodeState): reversed colour component
// orders, disabled colour formats and colour writes, conservative rasterization, depth maintenance
// passes, render-backend tuning fields, inert pixel stages and 64 KiB standard colour targets.
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace AgcDriver::Graphics;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
void Reject(TAction action, std::string_view reason) {
    try {
        action();
    } catch (const std::exception& error) {
        Require(std::string_view(error.what()).find(reason) != std::string_view::npos, std::string("unexpected rejection: ") + error.what() + " (expected " + std::string(reason) + ")");
        return;
    }
    throw std::runtime_error("expected a rejection: " + std::string(reason));
}

// 64x4 elements of up to 8 bytes.
alignas(256) std::array<std::byte, 4096> colorMemory{};

// A linear 64x4 R8G8B8A8_UNORM target in slot 0 written by a bound pixel shader.
AgcDriver::QueueState makeState() {
    AgcDriver::QueueState queue;
    queue.userConfig[0x242] = 4;
    const std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> values = {
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040}
    };
    for (const auto& [reg, value] : values) queue.context[reg] = value;
    const auto address = reinterpret_cast<std::uintptr_t>(colorMemory.data());
    queue.context[0x318] = static_cast<std::uint32_t>(address >> 8u);
    queue.context[0x390] = static_cast<std::uint32_t>(address >> 40u);
    queue.context[0x10f] = std::bit_cast<std::uint32_t>(32.0f);
    queue.context[0x110] = std::bit_cast<std::uint32_t>(32.0f);
    queue.context[0x111] = std::bit_cast<std::uint32_t>(-2.0f);
    queue.context[0x112] = std::bit_cast<std::uint32_t>(2.0f);
    queue.context[0x113] = std::bit_cast<std::uint32_t>(1.0f);
    queue.context[0x114] = 0;
    queue.context[0xb4] = 0;
    queue.context[0xb5] = std::bit_cast<std::uint32_t>(1.0f);
    queue.shader[0x8] = 0x100;
    return queue;
}

constexpr std::uint32_t SwapShift = 11;

void reversedOrderTests() {
    auto queue = makeState();
    queue.context[0x31c] = 0x28028u | (2u << SwapShift);
    auto state = DecodeState(queue);
    Require(state.colorTargetMask == 1u && state.colors[0].format == VK_FORMAT_R8G8B8A8_UNORM && state.colors[0].componentMapping == 0x1bu, "8_8_8_8 SWAP_STD_REV must store A, B, G, R");
    queue.context[0x31c] = 0x28028u | (3u << SwapShift);
    Require(DecodeState(queue).colors[0].componentMapping == 0x93u, "8_8_8_8 SWAP_ALT_REV must store A, R, G, B");
    // 16_16_16_16 (format 12).
    queue.context[0x31c] = (0x28028u & ~0x7cu) | (12u << 2u) | (2u << SwapShift);
    state = DecodeState(queue);
    Require(state.colors[0].format == VK_FORMAT_R16G16B16A16_UNORM && state.colors[0].componentMapping == 0x1bu && state.colors[0].elementBytes == 8u, "16_16_16_16 SWAP_STD_REV must decode reversed");
    queue.context[0x31c] = (0x28028u & ~0x7cu) | (12u << 2u) | (1u << SwapShift);
    Reject([&] { DecodeState(queue); }, "unsupported color format");
    queue.context[0x31c] = 0x28028u | (2u << SwapShift);
    queue.context[0x1e0] = 0x40000000u | 0x00010001u;
    queue.context[0x105] = queue.context[0x106] = queue.context[0x107] = queue.context[0x108] = 0;
    Reject([&] { DecodeState(queue); }, "reversed component order");
}

void disabledColorTests() {
    // CB_COLOR_INFO format INVALID: the slot is not written (upstream 1841f3c6).
    auto queue = makeState();
    queue.context[0x31c] = 0x28028u & ~0x7cu;
    Require(DecodeState(queue).colorTargetMask == 0u, "an INVALID colour format must disable its slot");
    Require(ColorWriteMask(queue.context) == 0u, "ColorWriteMask must drop the INVALID slot");
    // CB_COLOR_CONTROL MODE DISABLE with any ROP renders without colour (upstream 550920a1).
    queue = makeState();
    queue.context[0x202] = 0x330000u;
    Require(DecodeState(queue).colorTargetMask == 0u, "CB_COLOR_CONTROL mode disable must write no colour");
    // DISABLE_DUAL_QUAD (bit 0) is a tuning field (upstream 831ba5d2).
    queue.context[0x202] = 0xcc0011u;
    Require(DecodeState(queue).colorTargetMask == 1u, "DISABLE_DUAL_QUAD must not change colour rendering");
}

void tuningFieldTests() {
    auto queue = makeState();
    queue.context[0x292] = 0x22u;
    queue.context[0x2d5] = 0x2000u | (3u << 15u);
    Require(DecodeState(queue).colorTargetMask == 1u, "ALTERNATE_RBS_PER_TILE and MAX_PRIMGRP_IN_WAVE must be accepted");
}

void conservativeTests() {
    auto queue = makeState();
    Require(DecodeState(queue).conservativeRasterization == VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT, "0x6000 must leave conservative rasterization off");
    queue.context[0x313] = 0x6001u;
    queue.context[0x1b3] = 0x2u;
    Require(DecodeState(queue).conservativeRasterization == VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT, "0x6001 must overestimate triangles");
    queue.context[0x1b3] = 0x4u;
    Reject([&] { DecodeState(queue); }, "centroid interpolation");
    queue.context[0x1b3] = 0x2u;
    queue.userConfig[0x242] = 2;
    Reject([&] { DecodeState(queue); }, "conservative rasterization of lines");
    queue.userConfig[0x242] = 4;
    queue.context[0x313] = 0x6002u;
    Reject([&] { DecodeState(queue); }, "PA_SC_CONSERVATIVE_RASTERIZATION_CNTL");
}

void depthMaintenanceTests() {
    auto queue = makeState();
    Require(DepthMaintenanceRejection(queue).empty(), "a plain draw is no depth maintenance pass");
    queue.context[0x000] = 0x80u;
    Require(!DepthMaintenanceRejection(queue).empty(), "a depth copy pass must be rejected");
    Reject([&] { DecodeState(queue); }, "DB_RENDER_CONTROL");
}

void inertPixelStageTests() {
    // A stale PS address with no colour, export or forced run is not launched (upstream 98405e8f).
    auto queue = makeState();
    queue.context[0x8e] = 0;
    Require(!DecodeState(queue).hasFragmentShader, "an inert pixel stage must run without its program");
    queue.context[0x203] = 0x840u;
    Require(DecodeState(queue).hasFragmentShader, "KILL_ENABLE must keep the pixel program");
}

void standard64KBTests() {
    // SW_64KB_S colour targets (upstream 6aa8e58d): a 64 KiB block of distinct element offsets.
    for (const std::uint32_t elementBytes : {1u, 2u, 4u, 8u, 16u}) {
        const ColorTargetLayout layout(256, 256, ColorTileMode::Standard64KB, elementBytes);
        Require(layout.Alignment() == 65536u, "SW_64KB_S blocks are 64 KiB");
        const auto width = layout.BlockWidth();
        const auto height = static_cast<std::uint32_t>(65536u / (width * elementBytes));
        std::set<std::size_t> offsets;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto offset = layout.Offset(x, y);
                Require(offset % elementBytes == 0 && offset < 65536u, "SW_64KB_S element outside its block");
                offsets.insert(offset);
            }
        }
        Require(offsets.size() == static_cast<std::size_t>(width) * height, "SW_64KB_S elements alias at " + std::to_string(elementBytes) + " bytes per element");
    }
    Require(DecodeColorTileMode(0x9000000u | (9u << 14u)) == ColorTileMode::Standard64KB, "tile mode 9 must decode as SW_64KB_S");
}

}

int main() {
    try {
        reversedOrderTests();
        disabledColorTests();
        tuningFieldTests();
        conservativeTests();
        depthMaintenanceTests();
        inertPixelStageTests();
        standard64KBTests();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "state decode test failed: %s\n", error.what());
        return 1;
    }
    std::puts("state decode tests passed");
    return 0;
}
