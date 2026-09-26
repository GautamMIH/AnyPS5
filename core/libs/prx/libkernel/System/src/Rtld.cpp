#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/ApplicationHeap.hpp"

extern "C" {

#if defined(__linux__)
void* __tls_get_addr(void* index);
#endif

void* APS5_VABI __tls_get_addr_nid_postfix(void* index) {
#if defined(__linux__)
    return __tls_get_addr(index);
#else
    (void)index;
    NotImplemented_nid_no_patch(__func__);
    return nullptr;
#endif
}

void APS5_VABI sceKernelRtldSetApplicationHeapAPI(void* api[]) {
    ApplicationHeapRegister_nid_no_patch(api);
}

int APS5_VABI sceKernelRtldThreadAtexitDecrement(uint64_t* c) {
 (void)c;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceKernelRtldThreadAtexitIncrement(uint64_t* c) {
 (void)c;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

void APS5_VABI sceKernelSetThreadAtexitCount(get_thread_atexit_count_func_t func) {
 (void)func;
 NotImplemented_nid_no_patch(__func__);
}

void APS5_VABI sceKernelSetThreadAtexitReport(thread_atexit_report_func_t func) {
 (void)func;
 NotImplemented_nid_no_patch(__func__);
}

void APS5_VABI sceKernelSetThreadDtors(thread_dtors_func_t dtors) {
 (void)dtors;
 NotImplemented_nid_no_patch(__func__);
}

}
