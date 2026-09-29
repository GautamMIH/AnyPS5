#include <cstdint>
#include <cstddef>
#include <new>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

static_assert(sizeof(JpegEncCreateParam) == 0x8);
static_assert(sizeof(JpegEncEncodeParam) == 0x30);
static_assert(sizeof(JpegEncOutputInfo) == 0x8);

namespace {

constexpr std::int32_t SCE_JPEG_ENC_ERROR_INVALID_ADDR = static_cast<std::int32_t>(0x80650101);
constexpr std::int32_t SCE_JPEG_ENC_ERROR_INVALID_SIZE = static_cast<std::int32_t>(0x80650102);
constexpr std::int32_t SCE_JPEG_ENC_ERROR_INVALID_PARAM = static_cast<std::int32_t>(0x80650103);
constexpr std::int32_t SCE_JPEG_ENC_ERROR_INVALID_HANDLE = static_cast<std::int32_t>(0x80650104);

constexpr std::uint32_t ATTRIBUTE_NONE = 0;
constexpr std::uint32_t MEMORY_SIZE = 0x800;
constexpr std::uintptr_t HANDLE_ALIGNMENT = 0x20;

constexpr std::uint16_t PIXEL_FORMAT_R8G8B8A8 = 0;
constexpr std::uint16_t PIXEL_FORMAT_B8G8R8A8 = 1;
constexpr std::uint16_t PIXEL_FORMAT_Y8U8Y8V8 = 10;
constexpr std::uint16_t PIXEL_FORMAT_Y8 = 11;

constexpr std::uint16_t ENCODE_MODE_NORMAL = 0;
constexpr std::uint16_t ENCODE_MODE_MJPEG = 1;

constexpr std::uint16_t COLOR_SPACE_YCC = 1;
constexpr std::uint16_t COLOR_SPACE_GRAYSCALE = 2;

constexpr std::uint8_t SAMPLING_TYPE_FULL = 0;
constexpr std::uint8_t SAMPLING_TYPE_422 = 1;
constexpr std::uint8_t SAMPLING_TYPE_420 = 2;

constexpr std::uint32_t MAX_IMAGE_DIMENSION = 0xFFFF;
constexpr std::uint32_t MAX_IMAGE_PITCH = 0xFFFFFFF;
constexpr std::uint64_t MAX_IMAGE_SIZE = 0x7FFFFFFF;

struct Encoder {
    Encoder* self;
};

std::int32_t validateCreateParam(const JpegEncCreateParam* param) {
    if (!param) return SCE_JPEG_ENC_ERROR_INVALID_ADDR;
    if (param->size != sizeof(JpegEncCreateParam)) return SCE_JPEG_ENC_ERROR_INVALID_SIZE;
    if (param->attr != ATTRIBUTE_NONE) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
    return 0;
}

Encoder* toEncoder(void* handle) {
    const auto address = reinterpret_cast<std::uintptr_t>(handle);
    if (address == 0 || address % HANDLE_ALIGNMENT != 0) return nullptr;
    auto* encoder = reinterpret_cast<Encoder*>(handle);
    return encoder->self == encoder ? encoder : nullptr;
}

std::int32_t validateEncodeParam(const JpegEncEncodeParam* param) {
    if (!param) return SCE_JPEG_ENC_ERROR_INVALID_ADDR;
    const bool grayscaleInput = param->pixel_format == PIXEL_FORMAT_Y8;
    if (!param->image) return SCE_JPEG_ENC_ERROR_INVALID_ADDR;
    if (!grayscaleInput && reinterpret_cast<std::uintptr_t>(param->image) % 4 != 0) return SCE_JPEG_ENC_ERROR_INVALID_ADDR;
    if (!param->jpeg) return SCE_JPEG_ENC_ERROR_INVALID_ADDR;

    if (param->image_size == 0 || param->jpeg_size == 0) return SCE_JPEG_ENC_ERROR_INVALID_SIZE;

    if (param->image_width > MAX_IMAGE_DIMENSION || param->image_height > MAX_IMAGE_DIMENSION) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
    if (param->image_pitch == 0 || param->image_pitch > MAX_IMAGE_PITCH) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
    if (!grayscaleInput && param->image_pitch % 4 != 0) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
    const std::uint64_t requiredSize = static_cast<std::uint64_t>(param->image_height) * param->image_pitch;
    if (requiredSize > MAX_IMAGE_SIZE || requiredSize > param->image_size) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
    if (param->encode_mode != ENCODE_MODE_NORMAL && param->encode_mode != ENCODE_MODE_MJPEG) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
    if (param->color_space != COLOR_SPACE_YCC && param->color_space != COLOR_SPACE_GRAYSCALE) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
    if (param->sampling_type != SAMPLING_TYPE_FULL && param->sampling_type != SAMPLING_TYPE_422 && param->sampling_type != SAMPLING_TYPE_420) {
        return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->restart_interval > static_cast<std::int32_t>(MAX_IMAGE_DIMENSION)) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;

    switch (param->pixel_format) {
    case PIXEL_FORMAT_R8G8B8A8:
    case PIXEL_FORMAT_B8G8R8A8:
        if (param->image_pitch / 4 < param->image_width) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
        if (param->color_space != COLOR_SPACE_YCC || param->sampling_type == SAMPLING_TYPE_FULL) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
        return 0;
    case PIXEL_FORMAT_Y8U8Y8V8:
        if (param->image_pitch / 2 < ((param->image_width + 1) & ~1u)) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
        if (param->color_space != COLOR_SPACE_YCC || param->sampling_type == SAMPLING_TYPE_FULL) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
        return 0;
    case PIXEL_FORMAT_Y8:
        if (param->image_pitch < param->image_width) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
        if (param->color_space != COLOR_SPACE_GRAYSCALE || param->sampling_type != SAMPLING_TYPE_FULL) return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
        return 0;
    default:
        return SCE_JPEG_ENC_ERROR_INVALID_PARAM;
    }
}

}  // namespace

extern "C" {

int32_t APS5_VABI sceJpegEncCreate(const JpegEncCreateParam* param, void* memory, uint32_t memory_size, void** handle) {
    const std::int32_t result = validateCreateParam(param);
    if (result != 0) return result;
    if (!memory) return SCE_JPEG_ENC_ERROR_INVALID_ADDR;
    if (memory_size < MEMORY_SIZE) return SCE_JPEG_ENC_ERROR_INVALID_SIZE;
    if (!handle) return SCE_JPEG_ENC_ERROR_INVALID_ADDR;
    const auto address = reinterpret_cast<std::uintptr_t>(memory);
    const std::uintptr_t aligned = (address + HANDLE_ALIGNMENT - 1) & ~(HANDLE_ALIGNMENT - 1);
    auto* encoder = new (reinterpret_cast<void*>(aligned)) Encoder{};
    encoder->self = encoder;
    *handle = encoder;
    return 0;
}

int32_t APS5_VABI sceJpegEncDelete(void* handle) {
    Encoder* encoder = toEncoder(handle);
    if (!encoder) return SCE_JPEG_ENC_ERROR_INVALID_HANDLE;
    encoder->self = nullptr;
    return 0;
}

int32_t APS5_VABI sceJpegEncEncode(void* handle, const JpegEncEncodeParam* param, JpegEncOutputInfo* output_info) {
    if (!toEncoder(handle)) return SCE_JPEG_ENC_ERROR_INVALID_HANDLE;
    const std::int32_t result = validateEncodeParam(param);
    if (result != 0) return result;
    (void)output_info;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int32_t APS5_VABI sceJpegEncQueryMemorySize(const JpegEncCreateParam* param) {
    const std::int32_t result = validateCreateParam(param);
    if (result != 0) return result;
    return static_cast<std::int32_t>(MEMORY_SIZE);
}

}
