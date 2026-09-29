#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdint>
#include <cstdlib>

extern "C" {
std::int32_t APS5_VABI sceJpegEncQueryMemorySize(const JpegEncCreateParam*);
std::int32_t APS5_VABI sceJpegEncCreate(const JpegEncCreateParam*, void*, std::uint32_t, void**);
std::int32_t APS5_VABI sceJpegEncDelete(void*);
}

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    constexpr std::int32_t invalidAddr = static_cast<std::int32_t>(0x80650101);
    constexpr std::int32_t invalidSize = static_cast<std::int32_t>(0x80650102);
    constexpr std::int32_t invalidParam = static_cast<std::int32_t>(0x80650103);
    constexpr std::int32_t invalidHandle = static_cast<std::int32_t>(0x80650104);

    JpegEncCreateParam param{sizeof(JpegEncCreateParam), 0};
    Require(sceJpegEncQueryMemorySize(&param) == 0x800);
    Require(sceJpegEncQueryMemorySize(nullptr) == invalidAddr);

    JpegEncCreateParam badSize{sizeof(JpegEncCreateParam) - 1, 0};
    Require(sceJpegEncQueryMemorySize(&badSize) == invalidSize);

    JpegEncCreateParam badAttr{sizeof(JpegEncCreateParam), 1};
    Require(sceJpegEncQueryMemorySize(&badAttr) == invalidParam);

    alignas(32) static unsigned char memory[0x800 + 32];
    unsigned char* unaligned = memory + 1;
    void* handle = nullptr;

    Require(sceJpegEncCreate(nullptr, unaligned, 0x800, &handle) == invalidAddr);
    Require(sceJpegEncCreate(&badSize, unaligned, 0x800, &handle) == invalidSize);
    Require(sceJpegEncCreate(&badAttr, unaligned, 0x800, &handle) == invalidParam);
    Require(sceJpegEncCreate(&param, nullptr, 0x800, &handle) == invalidAddr);
    Require(sceJpegEncCreate(&param, unaligned, 0x7FF, &handle) == invalidSize);
    Require(sceJpegEncCreate(&param, unaligned, 0x800, nullptr) == invalidAddr);
    Require(handle == nullptr);

    Require(sceJpegEncCreate(&param, unaligned, 0x800, &handle) == 0);
    const auto address = reinterpret_cast<std::uintptr_t>(handle);
    Require(address % 32 == 0);
    Require(address >= reinterpret_cast<std::uintptr_t>(unaligned));
    Require(address < reinterpret_cast<std::uintptr_t>(unaligned) + 32);

    Require(sceJpegEncDelete(nullptr) == invalidHandle);
    Require(sceJpegEncDelete(static_cast<unsigned char*>(handle) + 1) == invalidHandle);
    alignas(32) static unsigned char garbage[64] = {};
    Require(sceJpegEncDelete(garbage) == invalidHandle);

    Require(sceJpegEncDelete(handle) == 0);
    Require(sceJpegEncDelete(handle) == invalidHandle);

    return 0;
}
