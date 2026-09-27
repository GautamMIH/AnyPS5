#include "prx/libSceAgcDriver/Graphics/include/FastClear.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Graphics {
namespace {

// Registered CMASK addresses and whether each is cleared. Array slices are not modelled, so one
// flag stands for shadPS4's per-slice clear mask.
std::mutex metaMutex;
std::unordered_map<std::uint64_t, bool> metaCleared;

// Returns whether the target's CMASK was cleared, registering it (as cleared) on first use,
// and marks it written.
bool takeCleared(std::uint64_t cmask) {
    std::lock_guard lock(metaMutex);
    const auto [it, inserted] = metaCleared.emplace(cmask, true);
    const auto cleared = it->second;
    it->second = false;
    return cleared;
}

// CLEAR_WORD0/1 hold one element in the surface format, so the fill is the same in every tiling.
void fillWithClearColor(const ColorTarget& color) {
    Require(color.elementBytes == 1 || color.elementBytes == 2 || color.elementBytes == 4 || color.elementBytes == 8, "fast clear of a colour target wider than 64 bits");
    std::array<std::byte, 8> element{};
    std::memcpy(element.data(), color.clearWords.data(), sizeof(element));
    std::vector<std::byte> fill(color.bytes);
    for (std::size_t offset = 0; offset < fill.size(); offset += color.elementBytes) std::memcpy(fill.data() + offset, element.data(), std::min<std::size_t>(color.elementBytes, fill.size() - offset));
    GuestMemory::Write(color.address, fill, 1);
}

}

void ApplyFastClears(const State& state) {
    for (std::uint32_t slot = 0; slot < MaxColorTargets; ++slot) {
        if ((state.colorTargetMask & (1u << slot)) == 0) continue;
        // shadPS4 resolves only colour target 0 in an elimination pass.
        if (state.eliminateFastClear && slot != 0) continue;
        const auto& color = state.colors[slot];
        if (!color.fastClear || color.cmaskAddress == 0) continue;
        if (takeCleared(color.cmaskAddress)) fillWithClearColor(color);
    }
}

void NoteMetadataClear(std::uint64_t address) {
    std::lock_guard lock(metaMutex);
    const auto it = metaCleared.find(address);
    if (it != metaCleared.end()) it->second = true;
}

}
