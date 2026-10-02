#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int32_t APS5_VABI sceAudioPropagationRoomCreate(AudioPropagationHandle system_handle, AudioPropagationHandle* out_room_handle) {
 (void)system_handle;
 (void)out_room_handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSystemCreate(const void* options, AudioPropagationSystemMemory* memory, AudioPropagationHandle* out_system_handle) {
 (void)options;
 (void)memory;
 (void)out_system_handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSystemGetRays(AudioPropagationHandle system_handle, void* rays, uint32_t* num_rays) {
 (void)system_handle;
 (void)rays;
 (void)num_rays;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSystemQueryMemory(const void* options, AudioPropagationSystemMemory* out_memory) {
 (void)options;
 (void)out_memory;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSystemRegisterMaterial(AudioPropagationHandle system_handle, const void* material, AudioPropagationHandle* out_material_handle) {
 (void)system_handle;
 (void)material;
 (void)out_material_handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSystemSetAttributes(AudioPropagationHandle system_handle, const void* attributes, uint32_t num_attributes) {
 (void)system_handle;
 (void)attributes;
 (void)num_attributes;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSystemDestroy(AudioPropagationHandle system_handle) {
 (void)system_handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSystemSetRays(AudioPropagationHandle system_handle, const void* rays, uint32_t num_rays) {
 (void)system_handle;
 (void)rays;
 (void)num_rays;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSystemUnregisterMaterial(AudioPropagationHandle system_handle, uint32_t material_id) {
 (void)system_handle;
 (void)material_id;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationRoomDestroy(AudioPropagationHandle room_handle) {
 (void)room_handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationPortalCreate(AudioPropagationHandle system_handle, const void* params, AudioPropagationHandle* out_portal_handle) {
 (void)system_handle;
 (void)params;
 (void)out_portal_handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationPortalDestroy(AudioPropagationHandle portal_handle) {
 (void)portal_handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationPortalSetAttributes(AudioPropagationHandle portal_handle, const void* attributes) {
 (void)portal_handle;
 (void)attributes;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSourceCreate(AudioPropagationHandle system_handle, const void* params, AudioPropagationHandle* out_source_handle) {
 (void)system_handle;
 (void)params;
 (void)out_source_handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSourceDestroy(AudioPropagationHandle source_handle) {
 (void)source_handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSourceSetAttributes(AudioPropagationHandle source_handle, const void* attributes) {
 (void)source_handle;
 (void)attributes;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSourceCalculateAudioPaths(AudioPropagationHandle source_handle, const void* params) {
 (void)source_handle;
 (void)params;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSourceGetAudioPathCount(AudioPropagationHandle source_handle, uint32_t* count) {
 (void)source_handle;
 (void)count;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSourceGetAudioPath(AudioPropagationHandle source_handle, uint32_t index, void* path) {
 (void)source_handle;
 (void)index;
 (void)path;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSourceSetAudioPaths(AudioPropagationHandle source_handle, const void* paths, uint32_t count) {
 (void)source_handle;
 (void)paths;
 (void)count;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSourceGetRays(AudioPropagationHandle source_handle, void* rays, uint32_t* num_rays) {
 (void)source_handle;
 (void)rays;
 (void)num_rays;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioPropagationSourceRender(AudioPropagationHandle source_handle, const void* input, void* output) {
 (void)source_handle;
 (void)input;
 (void)output;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

}
