#include <cstdint>
#include <cstddef>
#include <new>
#include <stdexcept>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "../include/PthreadSync.hpp"

extern "C" {

int APS5_VABI pthread_cond_destroy_nid_postfix(PthreadCond* cond) {
    if (!cond) return PosixThread::GUEST_EINVAL;
    return toPosix(scePthreadCondDestroy(cond));
}

int APS5_VABI pthread_cond_broadcast_nid_postfix(PthreadCond* cond) {
    return PthreadSync::CondBroadcast(cond);
}

int APS5_VABI pthread_cond_init_nid_postfix(PthreadCond* cond, const PthreadCondattr* attr) {
    return PthreadSync::CondCreate(cond, attr && *attr ? (*attr)->_clockid : PthreadSync::kClockRealtime);
}

int APS5_VABI pthread_cond_destroy_nid_postfix(PthreadCond* cond) {
    return PthreadSync::CondDestroy(cond);
}

int APS5_VABI pthread_cond_signal_nid_postfix(PthreadCond* cond) {
    return PthreadSync::CondSignal(cond);
}

int APS5_VABI pthread_cond_timedwait_nid_postfix(PthreadCond* cond, PthreadMutex* mutex, const KernelTimespec* abstime) {
    if (!abstime) return PthreadSync::kErrorInvalid;
    return PthreadSync::CondWait(cond, mutex, PthreadSync::DeadlineAt(PthreadSync::CondClock(cond), abstime->tv_sec, abstime->tv_nsec));
}

int APS5_VABI pthread_cond_wait_nid_postfix(PthreadCond* cond, PthreadMutex* mutex) {
    return PthreadSync::CondWait(cond, mutex, std::nullopt);
}

int APS5_VABI pthread_condattr_destroy_nid_postfix(PthreadCondattr* attr) {
    if (!attr || !*attr) return PthreadSync::kErrorInvalid;
    delete *attr;
    *attr = nullptr;
    return 0;
}

int APS5_VABI pthread_condattr_init_nid_postfix(PthreadCondattr* attr) {
    if (!attr) return PthreadSync::kErrorInvalid;
    auto* created = new (std::nothrow) PthreadCondattrPrivate{PthreadSync::kClockRealtime};
    if (!created) return PthreadSync::kErrorNoMemory;
    *attr = created;
    return 0;
}

int APS5_VABI pthread_condattr_setclock_nid_postfix(PthreadCondattr* attr, KernelClockid clock_id) {
    if (!attr || !*attr) return PthreadSync::kErrorInvalid;
    if (clock_id != PthreadSync::kClockRealtime && clock_id != PthreadSync::kClockMonotonic) return PthreadSync::kErrorInvalid;
    (*attr)->_clockid = clock_id;
    return 0;
}

}
