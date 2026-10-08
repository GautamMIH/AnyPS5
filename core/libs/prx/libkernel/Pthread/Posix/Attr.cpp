#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "../include/Pthread.hpp"
#include "../include/PthreadSync.hpp"

namespace {

int posixFromSce(const int result) {
    return result == 0 ? 0 : (result & 0xffff);
}

}

extern "C" {

int APS5_VABI scePthreadAttrInit(PthreadAttr* attr);
int APS5_VABI scePthreadAttrDestroy(PthreadAttr* attr);
int APS5_VABI scePthreadAttrGet(Pthread thread, PthreadAttr* attr);
int APS5_VABI scePthreadAttrGetdetachstate(const PthreadAttr* attr, int* state);
int APS5_VABI scePthreadAttrGetguardsize(const PthreadAttr* attr, size_t* guard_size);
int APS5_VABI scePthreadAttrGetschedparam(const PthreadAttr* attr, KernelSchedParam* param);
int APS5_VABI scePthreadAttrGetstack(const PthreadAttr* attr, void** stackaddr, std::size_t* stacksize);
int APS5_VABI scePthreadAttrGetstacksize(const PthreadAttr* attr, size_t* stack_size);
int APS5_VABI scePthreadAttrSetdetachstate(PthreadAttr* attr, int detachstate);
int APS5_VABI scePthreadAttrSetguardsize(PthreadAttr* attr, size_t guard_size);
int APS5_VABI scePthreadAttrSetinheritsched(PthreadAttr* attr, int inherit_sched);
int APS5_VABI scePthreadAttrSetschedparam(PthreadAttr* attr, const KernelSchedParam* param);
int APS5_VABI scePthreadAttrSetschedpolicy(PthreadAttr* attr, int policy);
int APS5_VABI scePthreadAttrSetstacksize(PthreadAttr* attr, std::size_t stacksize);
int APS5_VABI scePthreadAttrSetstack(PthreadAttr* attr, void* addr, std::size_t size);

int APS5_VABI pthread_attr_init_nid_postfix(PthreadAttr* attr) {
    return posixFromSce(scePthreadAttrInit(attr));
}

int APS5_VABI pthread_attr_destroy_nid_postfix(PthreadAttr* attr) {
    return posixFromSce(scePthreadAttrDestroy(attr));
}

int APS5_VABI pthread_attr_get_np_nid_postfix(Pthread thread, PthreadAttr* attr) {
    return posixFromSce(scePthreadAttrGet(thread, attr));
}

int APS5_VABI pthread_attr_getdetachstate_nid_postfix(const PthreadAttr* attr, int* state) {
    return posixFromSce(scePthreadAttrGetdetachstate(attr, state));
}

int APS5_VABI pthread_attr_getguardsize_nid_postfix(const PthreadAttr* attr, size_t* guard_size) {
    return posixFromSce(scePthreadAttrGetguardsize(attr, guard_size));
}

int APS5_VABI pthread_attr_getschedparam_nid_postfix(const PthreadAttr* attr, KernelSchedParam* param) {
    return posixFromSce(scePthreadAttrGetschedparam(attr, param));
}

int APS5_VABI pthread_attr_getschedpolicy_nid_postfix(const PthreadAttr* attr, int* policy) {
    if (!attr || !*attr || !policy) return PthreadSync::kErrorInvalid;
    *policy = (*attr)->_schedpolicy;
    return 0;
}

int APS5_VABI pthread_attr_getstack_nid_postfix(const PthreadAttr* __restrict attr, void** __restrict stack_addr, size_t* __restrict stack_size) {
    return posixFromSce(scePthreadAttrGetstack(attr, stack_addr, stack_size));
}

int APS5_VABI pthread_attr_getstacksize_nid_postfix(const PthreadAttr* attr, size_t* stack_size) {
    return posixFromSce(scePthreadAttrGetstacksize(attr, stack_size));
}

int APS5_VABI pthread_attr_setdetachstate_nid_postfix(PthreadAttr* attr, int state) {
    if (state != 0 && state != 1) return PthreadSync::kErrorInvalid;
    return posixFromSce(scePthreadAttrSetdetachstate(attr, state));
}

int APS5_VABI pthread_attr_setguardsize_nid_postfix(PthreadAttr* attr, size_t guard_size) {
    return posixFromSce(scePthreadAttrSetguardsize(attr, guard_size));
}

int APS5_VABI pthread_attr_setinheritsched_nid_postfix(PthreadAttr* attr, int inherit_sched) {
    return posixFromSce(scePthreadAttrSetinheritsched(attr, inherit_sched));
}

int APS5_VABI pthread_attr_setschedparam_nid_postfix(PthreadAttr* attr, const KernelSchedParam* param) {
    return posixFromSce(scePthreadAttrSetschedparam(attr, param));
}

int APS5_VABI pthread_attr_setschedpolicy_nid_postfix(PthreadAttr* attr, int policy) {
    return posixFromSce(scePthreadAttrSetschedpolicy(attr, policy));
}

int APS5_VABI pthread_attr_setstacksize_nid_postfix(PthreadAttr* attr, size_t stack_size) {
    if (stack_size < 16384) return PthreadSync::kErrorInvalid;
    return posixFromSce(scePthreadAttrSetstacksize(attr, stack_size));
}

int APS5_VABI pthread_attr_setstack_nid_postfix(PthreadAttr* attr, void* stack_addr, size_t stack_size) {
    if (!attr || !*attr || !stack_addr || stack_size < 16384) return PthreadSync::kErrorInvalid;
    return posixFromSce(scePthreadAttrSetstack(attr, stack_addr, stack_size));
}

int APS5_VABI pthread_attr_setsolosched_np_nid_postfix() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
