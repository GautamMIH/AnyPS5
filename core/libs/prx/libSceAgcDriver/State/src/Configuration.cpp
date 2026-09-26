#include "prx/libSceAgcDriver/State/include/Configuration.hpp"

#include <array>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

// Ring and off-chip parameters only size hardware tessellation buffers; they are recorded for the
// shader translator, which lowers tessellation without a guest-visible ring.
struct RingConfiguration {
    std::mutex mutex;
    const volatile void* tessellationFactorBase = nullptr;
    std::uint32_t tessellationFactorSize = 0;
    std::array<std::uint64_t, 3> hullShaderOffchip{};
};

RingConfiguration& configuration() {
    static RingConfiguration instance;
    return instance;
}

}

extern "C" {

int APS5_VABI sceAgcDriverSetHsOffchipParam(uint64_t value0, uint64_t value1, uint64_t value2) {
    auto& state = configuration();
    std::lock_guard lock(state.mutex);
    state.hullShaderOffchip = {value0, value1, value2};
    return 0;
}

int APS5_VABI sceAgcDriverSetTFRing(const volatile void* base, uint32_t size) {
    if ((base == nullptr) != (size == 0)) throw std::runtime_error("sceAgcDriverSetTFRing: base and size must both be set or both be empty");
    if ((reinterpret_cast<std::uintptr_t>(base) & 0xffu) != 0) throw std::runtime_error("sceAgcDriverSetTFRing: misaligned ring base");
    auto& state = configuration();
    std::lock_guard lock(state.mutex);
    state.tessellationFactorBase = base;
    state.tessellationFactorSize = size;
    return 0;
}

}
