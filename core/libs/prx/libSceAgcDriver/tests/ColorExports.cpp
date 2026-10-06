// Colour export decoding: compacted pixel shader exports and their MRT slots (DecodeState).
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <initializer_list>
#include <string>
#include <utility>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

alignas(256) std::array<std::byte, 1024> colorMemory{};
alignas(256) std::array<std::byte, 1024> secondColorMemory{};

AgcDriver::QueueState makeState() {
    AgcDriver::QueueState queue;
    queue.userConfig[0x242] = 4;
    // Applied over the default context (the decoder reads registers this test leaves unset).
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
    return queue;
}

// Pixel shader exports are compacted (upstream 1c4aadb3): with CB_SHADER_MASK 0xf000f, export 1
// (SPI_SHADER_COL_FORMAT field 1) goes to MRT slot 4.
void compactedExportTests() {
    auto queue = makeState();
    queue.shader[0x8] = 0x100;  // SPI_SHADER_PGM_LO_PS: a pixel shader exports the colours.
    const auto copySlot = [&](std::uint32_t reg, std::uint32_t stride) { queue.context[reg + stride * 4u] = queue.context.at(reg); };
    for (const auto reg : {0x31bu, 0x31cu, 0x31du}) copySlot(reg, 0xfu);
    for (const auto reg : {0x3b0u, 0x3b8u, 0x1e0u}) copySlot(reg, 1u);
    const auto second = reinterpret_cast<std::uintptr_t>(secondColorMemory.data());
    queue.context[0x318 + 0xf * 4] = static_cast<std::uint32_t>(second >> 8u);
    queue.context[0x390 + 4] = static_cast<std::uint32_t>(second >> 40u);
    queue.context[0x8e] = 0xf000f;
    queue.context[0x8f] = 0xf000f;
    queue.context[0x1c5] = 0x99;
    const auto state = AgcDriver::Graphics::DecodeState(queue);
    Require(state.colorTargetMask == 3u, "compacted exports did not reach slots 0 and 4 as attachments 0 and 1 (mask " + std::to_string(state.colorTargetMask) + ")");
    Require(state.colors[1].address == second && state.colors[0].address == reinterpret_cast<std::uintptr_t>(colorMemory.data()), "export 1 did not take slot 4's colour buffer");
    // Export 1 has no format (field 1 is ZERO): slot 4 receives nothing.
    queue.context[0x1c5] = 0x9;
    Require(AgcDriver::Graphics::DecodeState(queue).colorTargetMask == 1u, "a compacted export without a format kept its target");
}

}

int main() {
    try {
        compactedExportTests();
        std::puts("AGC driver colour export tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
