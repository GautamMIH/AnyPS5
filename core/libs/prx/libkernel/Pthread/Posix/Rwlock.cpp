#include <cstdint>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <string>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "../include/PthreadSync.hpp"

extern "C" {

int APS5_VABI pthread_rwlock_destroy_nid_postfix(PthreadRwlock* rwlock) {
    return PthreadSync::RwlockDestroy(rwlock);
}

int APS5_VABI pthread_rwlock_init_nid_postfix(PthreadRwlock* rwlock, const PthreadRwlockattr*) {
    return PthreadSync::RwlockCreate(rwlock);
}

int APS5_VABI pthread_rwlock_rdlock_nid_postfix(PthreadRwlock* rwlock) {
    return PthreadSync::RwlockLock(rwlock, false, std::nullopt);
}

int APS5_VABI pthread_rwlock_wrlock_nid_postfix(PthreadRwlock* rwlock) {
    return PthreadSync::RwlockLock(rwlock, true, std::nullopt);
}

// FreeBSD: a lock that is free is taken without looking at abstime; only a wait checks it.
static int timedLock(PthreadRwlock* rwlock, const KernelTimespec* abstime, const bool exclusive, const char* function) {
    if (!abstime) throw std::runtime_error(std::string(function) + ": null abstime");
    if (PthreadSync::RwlockTryLock(rwlock, exclusive) == 0) return 0;
    if (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000) return PthreadSync::kErrorInvalid;
    return PthreadSync::RwlockLock(rwlock, exclusive, PthreadSync::DeadlineAt(PthreadSync::kClockRealtime, abstime->tv_sec, abstime->tv_nsec));
}

int APS5_VABI pthread_rwlock_timedrdlock_nid_postfix(PthreadRwlock* rwlock, const KernelTimespec* abstime) {
    return timedLock(rwlock, abstime, false, __func__);
}

int APS5_VABI pthread_rwlock_timedwrlock_nid_postfix(PthreadRwlock* rwlock, const KernelTimespec* abstime) {
    return timedLock(rwlock, abstime, true, __func__);
}

int APS5_VABI pthread_rwlock_tryrdlock_nid_postfix(PthreadRwlock* rwlock) {
    return PthreadSync::RwlockTryLock(rwlock, false);
}

int APS5_VABI pthread_rwlock_trywrlock_nid_postfix(PthreadRwlock* rwlock) {
    return PthreadSync::RwlockTryLock(rwlock, true);
}

int APS5_VABI pthread_rwlock_unlock_nid_postfix(PthreadRwlock* rwlock) {
    return PthreadSync::RwlockUnlock(rwlock);
}

int APS5_VABI pthread_rwlockattr_init_nid_postfix(PthreadRwlockattr* attr) {
    if (!attr) return PthreadSync::kErrorInvalid;
    auto* created = new (std::nothrow) PthreadRwlockattrPrivate{};
    if (!created) return PthreadSync::kErrorNoMemory;
    *attr = created;
    return 0;
}

int APS5_VABI pthread_rwlockattr_destroy_nid_postfix(PthreadRwlockattr* attr) {
    if (!attr || !*attr) return PthreadSync::kErrorInvalid;
    delete *attr;
    *attr = nullptr;
    return 0;
}

}
