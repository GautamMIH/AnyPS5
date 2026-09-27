#include "prx/libSceAgc/Command/include/Draw.hpp"

namespace Agc::Command {

namespace {

std::uint32_t sgprBase(std::uint64_t modifier) {
    const auto stage = static_cast<std::uint32_t>(modifier) >> 29u;
    return stage == 3 || stage == 5 ? 0x10cu : 0x8cu;
}

}

std::uint32_t DrawInitiator(std::uint64_t modifier, bool indexed, const char* function) {
    CheckBits(modifier, 0x1ffffffffull, function);
    const auto flags = (modifier & 0x100000000ull) != 0 ? 0u : static_cast<std::uint32_t>((modifier >> 3u) & 0x20u);
    return flags | (indexed ? 0u : 2u);
}

std::uint64_t DrawPatchOffsets(std::uint64_t modifier, const char* function) {
    CheckBits(modifier, 0x1ffffffffull, function);
    const auto base = sgprBase(modifier);
    const auto vertex = (modifier & 1u) != 0 ? base + static_cast<std::uint32_t>((modifier >> 9u) & 0x1fu) : 0x280u;
    const auto instance = (modifier & 4u) != 0 ? base + static_cast<std::uint32_t>((modifier >> 19u) & 0x1fu) : 0x280u;
    return vertex | (static_cast<std::uint64_t>(instance) << 32u);
}

std::uint64_t DrawIndexedPatchOffsets(std::uint64_t modifier, const char* function) {
    auto offsets = DrawPatchOffsets(modifier, function);
    if ((modifier & 2u) != 0) offsets |= (static_cast<std::uint64_t>(sgprBase(modifier) + static_cast<std::uint32_t>((modifier >> 14u) & 0x1fu)) << 16u) | (1ull << 59u);
    return offsets;
}

std::uint32_t DrawIndexLocation(std::uint64_t modifier) {
    return (modifier & 8u) != 0 ? sgprBase(modifier) + static_cast<std::uint32_t>((modifier >> 24u) & 0x1fu) : 0x280u;
}

}
