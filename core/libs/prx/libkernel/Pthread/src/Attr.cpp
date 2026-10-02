#include "../include/Pthread.hpp"
#include "../include/PthreadSync.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include <stdexcept>
#include <string>

static constexpr int SCE_OK = 0;

static constexpr std::size_t DEFAULT_STACK_SIZE = 1u << 20;
static constexpr int DETACH_JOINABLE = 0;
static constexpr int DETACH_DETACHED = 1;
static constexpr int SCHED_FIFO_PS5 = 1;

#ifdef _WIN32
#include <windows.h>
#include <limits>
#endif

extern "C" {

int APS5_VABI scePthreadAttrInit(PthreadAttr* attr) {
    if (!attr) throw std::runtime_error("scePthreadAttrInit: null attr");
    auto* p = new (std::nothrow) PthreadAttrPrivate{};
    if (!p) return SCE_KERNEL_ERROR_ENOMEM;
    p->_stacksize = DEFAULT_STACK_SIZE;
    p->_detachstate = DETACH_JOINABLE;
    p->_schedpriority = 700;
    p->_schedpolicy = SCHED_FIFO_PS5;
    p->_inheritsched = 4;
    *attr = p;
    return SCE_OK;
}

int APS5_VABI scePthreadAttrDestroy(PthreadAttr* attr) {
    if (!attr || !*attr) throw std::runtime_error("scePthreadAttrDestroy: null attr");
    delete *attr;
    *attr = nullptr;
    return SCE_OK;
}

int APS5_VABI scePthreadAttrSetdetachstate(PthreadAttr* attr, int detachstate) {
    if (!attr || !*attr) throw std::runtime_error("scePthreadAttrSetdetachstate: null attr");
    if (detachstate != DETACH_JOINABLE && detachstate != DETACH_DETACHED)
        throw std::runtime_error("scePthreadAttrSetdetachstate: invalid state");
    (*attr)->_detachstate = detachstate;
    return SCE_OK;
}

int APS5_VABI scePthreadAttrSetschedparam(PthreadAttr* attr, const KernelSchedParam* param) {
    if (!attr || !*attr || !param)
        throw std::runtime_error("scePthreadAttrSetschedparam: null arg");
    (*attr)->_schedpriority = param->sched_priority;
    return SCE_OK;
}

int APS5_VABI scePthreadAttrSetstacksize(PthreadAttr* attr, std::size_t stacksize) {
    if (!attr || !*attr) throw std::runtime_error("scePthreadAttrSetstacksize: null attr");
    if (stacksize < 16384) throw std::runtime_error("scePthreadAttrSetstacksize: too small");
#ifdef _WIN32
    if (stacksize > std::numeric_limits<unsigned>::max() - 0xffffu)
        throw std::runtime_error(std::string(__func__) + ": stack size exceeds the Windows limit");
#endif
    (*attr)->_stacksize = stacksize;
    return SCE_OK;
}

int APS5_VABI scePthreadAttrGetstack(const PthreadAttr* attr, void** stackaddr, std::size_t* stacksize) {
    if (!attr || !*attr || !stackaddr || !stacksize)
        throw std::runtime_error("scePthreadAttrGetstack: null arg");
    *stackaddr = (*attr)->stackAddress;
    *stacksize = (*attr)->_stacksize;
    return SCE_OK;
}

int APS5_VABI scePthreadAttrGet(Pthread thread, PthreadAttr* attr) {
    if (!thread || !attr || !*attr) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
    (*attr)->_stacksize = thread->stackSize;
    (*attr)->stackAddress = thread->stackAddress;
    (*attr)->_detachstate = thread->_detached ? DETACH_DETACHED : DETACH_JOINABLE;
    (*attr)->_schedpriority = thread->priority;
    (*attr)->_schedpolicy = thread->policy;
    (*attr)->affinity = thread->affinity;
    return SCE_OK;
}

int APS5_VABI scePthreadAttrGetaffinity(const PthreadAttr* attr, KernelCpumask* mask) {
 if (!attr || !*attr || !mask) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
 *mask = (*attr)->affinity;
 return SCE_OK;
}

int APS5_VABI scePthreadAttrGetdetachstate(const PthreadAttr* attr, int* state) {
 if (!attr || !*attr || !state) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
 *state = (*attr)->_detachstate;
 return SCE_OK;
}

int APS5_VABI scePthreadAttrGetguardsize(const PthreadAttr* attr, size_t* guard_size) {
 if (!attr || !*attr || !guard_size) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
 *guard_size = (*attr)->guardSize;
 return SCE_OK;
}

int APS5_VABI scePthreadAttrGetschedparam(const PthreadAttr* attr, KernelSchedParam* param) {
 if (!attr || !*attr || !param) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
 param->sched_priority = (*attr)->_schedpriority;
 return SCE_OK;
}

int APS5_VABI scePthreadAttrGetsolosched(const PthreadAttr* attr, int* solosched) {
 if (!attr || !*attr || !solosched) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
 *solosched = (*attr)->solosched;
 return SCE_OK;
}

int APS5_VABI scePthreadAttrGetstackaddr(const PthreadAttr* attr, void** stack_addr) {
 if (!attr || !*attr || !stack_addr) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
 *stack_addr = (*attr)->stackAddress;
 return SCE_OK;
}

int APS5_VABI scePthreadAttrGetstacksize(const PthreadAttr* attr, size_t* stack_size) {
 if (!attr || !*attr || !stack_size) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
 *stack_size = (*attr)->_stacksize;
 return SCE_OK;
}

int APS5_VABI scePthreadAttrSetaffinity(PthreadAttr* attr, KernelCpumask mask) {
 if (!attr || !*attr || mask == 0) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
 (*attr)->affinity = mask;
 return SCE_OK;
}

int APS5_VABI scePthreadAttrSetguardsize(PthreadAttr* attr, size_t guard_size) {
 if (!attr || !*attr) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
 (*attr)->guardSize = guard_size;
 return SCE_OK;
}

int APS5_VABI scePthreadAttrSetinheritsched(PthreadAttr* attr, int inherit_sched) {
 if (!attr || !*attr || (inherit_sched != 0 && inherit_sched != 4)) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
 (*attr)->_inheritsched = inherit_sched;
 return SCE_OK;
}

int APS5_VABI scePthreadAttrSetschedpolicy(PthreadAttr* attr, int policy) {
 if (!attr || !*attr || policy < 1 || policy > 3) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
 (*attr)->_schedpolicy = policy;
 return SCE_OK;
}

int APS5_VABI scePthreadAttrSetsolosched(PthreadAttr* attr, int solosched) {
 if (!attr || !*attr) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
 (*attr)->solosched = solosched;
 return SCE_OK;
}

int APS5_VABI scePthreadAttrSetstack(PthreadAttr* attr, void* addr, size_t size) {
 if (!attr || !*attr || !addr || size < 16384) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
 (*attr)->stackAddress = addr;
 (*attr)->_stacksize = size;
 return SCE_OK;
}

int APS5_VABI scePthreadAttrSetstackaddr(PthreadAttr* attr, void* addr) {
 if (!attr || !*attr || !addr) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
 (*attr)->stackAddress = addr;
 return SCE_OK;
}

}
