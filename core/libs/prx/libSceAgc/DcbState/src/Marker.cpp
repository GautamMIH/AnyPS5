#include "prx/libSceAgc/DcbState/include/Marker.hpp"

#include "prx/libSceAgc/Command/include/Packet.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr std::uint32_t CustomNopHeader(std::uint32_t operation, std::uint32_t words) {
    return 0xc0000000u | ((words - 2u) << 16u) | (0x10u << 8u) | (operation << 2u);
}

// Markers carry their NUL-terminated text in the packet payload; the colour only tints GPU captures.
std::uint32_t* WriteTextPacket(CommandBuffer* buf, std::uint32_t operation, std::uint32_t leadingWords, const char* text, const char* function) {
    Agc::Command::Require(text != nullptr, function, "null marker text");
    const auto length = std::strlen(text);
    Agc::Command::Require(length < 0x10000u, function, "marker text is too long");
    const auto textWords = static_cast<std::uint32_t>(length / sizeof(std::uint32_t) + 1u);
    const auto words = 1u + leadingWords + textWords;
    auto* packet = Agc::Command::Allocate(buf, words, function);
    std::memset(packet, 0, words * sizeof(std::uint32_t));
    packet[0] = CustomNopHeader(operation, words);
    std::memcpy(packet + 1 + leadingWords, text, length);
    return packet;
}

}

extern "C" {

uint32_t* APS5_VABI sceAgcDcbSetMarker(CommandBuffer* buf, const char* str, uint32_t color) {
    (void)color;
    return WriteTextPacket(buf, 0, 1, str, __func__);
}

uint32_t* APS5_VABI sceAgcDcbPopMarker(CommandBuffer* buf) {
    auto* packet = Agc::Command::Allocate(buf, 2, __func__);
    packet[0] = CustomNopHeader(0x0c, 2);
    packet[1] = 0;
    return packet;
}

uint32_t* APS5_VABI sceAgcDcbPushMarker(CommandBuffer* buf, const char* str, uint32_t color) {
    (void)color;
    return WriteTextPacket(buf, 0x0b, 0, str, __func__);
}

}
