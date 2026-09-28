#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include <mutex>
#include <set>
#include <stdexcept>

namespace {

// Contexts only carry identity here; batches, which run the audio DSP work, are not modelled.
std::mutex contextMutex;
std::set<AcmContextId> contexts;
AcmContextId nextContext = 1;

}

extern "C" {

int APS5_VABI sceAcmBatchStartBuffer(AcmContextId context, const void* batch_commands, size_t batch_size, AcmBatchError* batch_error, AcmBatchId* batch) {
 (void)context;
 (void)batch_commands;
 (void)batch_size;
 (void)batch_error;
 (void)batch;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAcmBatchStartBuffers(AcmContextId context, uint32_t batch_info_count, const AcmBatchInfo* const batch_info[], AcmBatchError* batch_error, AcmBatchId* batch) {
 (void)context;
 (void)batch_info_count;
 (void)batch_info;
 (void)batch_error;
 (void)batch;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAcmBatchWait(AcmContextId context, AcmBatchId batch, uint32_t timeout) {
 (void)context;
 (void)batch;
 (void)timeout;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAcmContextCreate(AcmContextId* context) {
 if (context == nullptr) throw std::runtime_error("sceAcmContextCreate: null context");
 std::lock_guard lock(contextMutex);
 const auto id = nextContext++;
 contexts.insert(id);
 *context = id;
 return 0;
}

int APS5_VABI sceAcmContextDestroy(AcmContextId context) {
 std::lock_guard lock(contextMutex);
 if (contexts.erase(context) == 0) throw std::runtime_error("sceAcmContextDestroy: unknown context");
 return 0;
}

// Builds a convolution-reverb command into a batch; batch encoding is not modelled yet.
int APS5_VABI sceAcm_ConvReverb_SharedInput(void) {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

}
