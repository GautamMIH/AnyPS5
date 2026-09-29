#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdint>
#include <cstdlib>

extern "C" {
std::int32_t APS5_VABI sceJpegEncQueryMemorySize(const JpegEncCreateParam*);
}

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    constexpr std::int32_t invalidAddr = static_cast<std::int32_t>(0x80650101);
    constexpr std::int32_t invalidSize = static_cast<std::int32_t>(0x80650102);
    constexpr std::int32_t invalidParam = static_cast<std::int32_t>(0x80650103);

    JpegEncCreateParam param{sizeof(JpegEncCreateParam), 0};
    Require(sceJpegEncQueryMemorySize(&param) == 0x800);
    Require(sceJpegEncQueryMemorySize(nullptr) == invalidAddr);

    JpegEncCreateParam badSize{sizeof(JpegEncCreateParam) - 1, 0};
    Require(sceJpegEncQueryMemorySize(&badSize) == invalidSize);

    JpegEncCreateParam badAttr{sizeof(JpegEncCreateParam), 1};
    Require(sceJpegEncQueryMemorySize(&badAttr) == invalidParam);

    return 0;
}
