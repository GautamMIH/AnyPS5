#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

static_assert(sizeof(JpegEncCreateParam) == 0x8);
static_assert(sizeof(JpegEncEncodeParam) == 0x30);
static_assert(sizeof(JpegEncOutputInfo) == 0x8);

namespace {

constexpr std::int32_t SCE_JPEG_ENC_ERROR_INVALID_ADDR = static_cast<std::int32_t>(0x80650101);
constexpr std::int32_t SCE_JPEG_ENC_ERROR_INVALID_SIZE = static_cast<std::int32_t>(0x80650102);
constexpr std::int32_t SCE_JPEG_ENC_ERROR_INVALID_PARAM = static_cast<std::int32_t>(0x80650103);

constexpr std::uint32_t ATTRIBUTE_NONE = 0;
constexpr std::int32_t MEMORY_SIZE = 0x800;

std::int32_t validateCreateParam(const JpegEncCreateParam* param) {
    if (!param) return SCE_JPEG_ENC_ERROR_INVALID_ADDR;
    if (param->size != sizeof(JpegEncCreateParam)) return SCE_JPEG_ENC_ERROR_INVALID_SIZE;
    if (param->attr != ATTRIBUTE_NONE) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
    return 0;
}

}  // namespace

extern "C" {

int32_t APS5_VABI sceJpegEncCreate(const JpegEncCreateParam* param, void* memory, uint32_t memory_size, void** handle) {
    (void)param;
    (void)memory;
    (void)memory_size;
    (void)handle;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int32_t APS5_VABI sceJpegEncDelete(void* handle) {
    (void)handle;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int32_t APS5_VABI sceJpegEncEncode(void* handle, const JpegEncEncodeParam* param, JpegEncOutputInfo* output_info) {
    (void)handle;
    (void)param;
    (void)output_info;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int32_t APS5_VABI sceJpegEncQueryMemorySize(const JpegEncCreateParam* param) {
    const std::int32_t result = validateCreateParam(param);
    if (result != 0) return result;
    return MEMORY_SIZE;
}

}
