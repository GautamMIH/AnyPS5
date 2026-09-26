#include <cstdint>
#include <cstddef>
#include <random>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

#if defined(__linux__)
#include <sys/random.h>
#endif

namespace {

constexpr std::size_t RANDOM_MAX_SIZE = 64;
constexpr int RANDOM_ERROR_INVALID = static_cast<int>(0x817C0016);

}

extern "C" {

int APS5_VABI sceRandomGetRandomNumber(void* buf, size_t size) {
    if (!buf || size > RANDOM_MAX_SIZE) return RANDOM_ERROR_INVALID;
#if defined(__linux__)
    auto* bytes = static_cast<std::uint8_t*>(buf);
    std::size_t filled = 0;
    while (filled < size) {
        const auto result = getrandom(bytes + filled, size - filled, 0);
        if (result < 0) return RANDOM_ERROR_INVALID;
        filled += static_cast<std::size_t>(result);
    }
#else
    std::random_device device;
    auto* bytes = static_cast<std::uint8_t*>(buf);
    for (std::size_t index = 0; index < size; ++index) bytes[index] = static_cast<std::uint8_t>(device());
#endif
    return 0;
}

}
