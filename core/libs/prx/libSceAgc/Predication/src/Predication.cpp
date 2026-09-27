#include "prx/libSceAgc/Predication/include/Predication.hpp"

#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

// Bit 0 of a type-3 header marks the packet as predicated (skipped while the predicate is set).
int APS5_VABI sceAgcSetPacketPredication(uint32_t* packet, uint32_t predication) {
    if (packet == nullptr) APS5_INVALID_ARG_EX;
    packet[0] = (packet[0] & ~1u) | (static_cast<std::uint8_t>(predication) == 1 ? 1u : 0u);
    return 0;
}

int APS5_VABI sceAgcSetRangePredication(uint32_t* start, const volatile uint32_t* end, uint32_t predication) {
    if (start == nullptr || end == nullptr) APS5_INVALID_ARG_EX;
    const auto bit = static_cast<std::uint8_t>(predication) == 1 ? 1u : 0u;
    auto* packet = start;
    const auto* last = const_cast<const uint32_t*>(end);
    while (packet < last) {
        const auto header = packet[0];
        packet[0] = (header & ~1u) | bit;
        packet += ((header >> 16u) & 0x3fffu) + 2u;
    }
    return 0;
}

}
