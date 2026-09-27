#include <atomic>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

// Voice chat needs the network, which an offline console never provides, so titles initialise the
// library and never create endpoints; the endpoint functions are deliberately not provided yet.
namespace {

constexpr int VOICE_QOS_OK = 0;

std::atomic<bool> initialized{false};

}

extern "C" {

int APS5_VABI sceVoiceQoSInit(void* mem_block, uint32_t mem_size, int32_t app_type) {
    (void)app_type;
    if (mem_block == nullptr || mem_size == 0) APS5_INVALID_ARG_EX;
    // The library's error codes for misuse are unknown, so misuse fails loudly.
    if (initialized.exchange(true)) throw std::logic_error("sceVoiceQoSInit: already initialized");
    return VOICE_QOS_OK;
}

int APS5_VABI sceVoiceQoSEnd(void) {
    if (!initialized.exchange(false)) throw std::logic_error("sceVoiceQoSEnd: not initialized");
    return VOICE_QOS_OK;
}

}
