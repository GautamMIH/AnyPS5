#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

static_assert(sizeof(JpegEncCreateParam) == 0x8);
static_assert(sizeof(JpegEncEncodeParam) == 0x30);
static_assert(sizeof(JpegEncOutputInfo) == 0x8);

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
 (void)param;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

}
