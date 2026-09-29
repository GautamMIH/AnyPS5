#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdint>
#include <cstdlib>

extern "C" {
std::int32_t APS5_VABI sceJpegEncQueryMemorySize(const JpegEncCreateParam*);
std::int32_t APS5_VABI sceJpegEncCreate(const JpegEncCreateParam*, void*, std::uint32_t, void**);
std::int32_t APS5_VABI sceJpegEncDelete(void*);
std::int32_t APS5_VABI sceJpegEncEncode(void*, const JpegEncEncodeParam*, JpegEncOutputInfo*);
}

static void Require(bool value) { if (!value) std::abort(); }

alignas(4) static unsigned char image[16 * 16 * 4];
static unsigned char jpeg[4096];

static JpegEncEncodeParam ValidEncodeParam() {
    JpegEncEncodeParam param{};
    param.image = image;
    param.jpeg = jpeg;
    param.image_size = sizeof(image);
    param.jpeg_size = sizeof(jpeg);
    param.image_width = 16;
    param.image_height = 16;
    param.image_pitch = 16 * 4;
    param.pixel_format = 0;
    param.encode_mode = 0;
    param.color_space = 1;
    param.sampling_type = 2;
    param.compression_ratio = 80;
    param.restart_interval = 0;
    return param;
}

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

    JpegEncOutputInfo info{};
    const JpegEncEncodeParam valid = ValidEncodeParam();
    Require(sceJpegEncEncode(handle, &valid, &info) == invalidHandle);
    Require(sceJpegEncEncode(nullptr, &valid, &info) == invalidHandle);

    Require(sceJpegEncCreate(&param, unaligned, 0x800, &handle) == 0);
    Require(sceJpegEncEncode(handle, nullptr, &info) == invalidAddr);

    auto encodeWith = [&](auto change) {
        JpegEncEncodeParam changed = ValidEncodeParam();
        change(changed);
        return sceJpegEncEncode(handle, &changed, &info);
    };

    Require(encodeWith([](JpegEncEncodeParam& p) { p.image = nullptr; }) == invalidAddr);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.image = image + 1; }) == invalidAddr);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.jpeg = nullptr; }) == invalidAddr);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.image_size = 0; }) == invalidSize);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.jpeg_size = 0; }) == invalidSize);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.image_width = 0x10000; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.image_height = 0x10000; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.image_pitch = 0; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.image_pitch = 66; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.image_size = sizeof(image) - 1; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.image_height = 0xFFFF; p.image_pitch = 0xFFFFFFC; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.encode_mode = 2; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.color_space = 0; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.sampling_type = 3; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.restart_interval = 0x10000; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.pixel_format = 2; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.image_width = 17; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.color_space = 2; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.sampling_type = 0; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.pixel_format = 10; p.image_width = 33; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.pixel_format = 11; p.color_space = 2; p.sampling_type = 0; p.image_width = 65; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.pixel_format = 11; p.color_space = 2; p.sampling_type = 2; }) == invalidParam);
    Require(encodeWith([](JpegEncEncodeParam& p) { p.pixel_format = 11; p.sampling_type = 0; }) == invalidParam);

    Require(sceJpegEncDelete(handle) == 0);

    return 0;
}
