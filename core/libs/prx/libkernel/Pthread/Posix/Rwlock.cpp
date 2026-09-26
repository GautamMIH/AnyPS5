#include <cstdint>
#include <cstddef>
#include <new>
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

int APS5_VABI pthread_rwlock_timedrdlock_nid_postfix(PthreadRwlock* rwlock, const KernelTimespec* abstime) {
    if (!abstime) return PthreadSync::kErrorInvalid;
    return PthreadSync::RwlockLock(rwlock, false, PthreadSync::DeadlineAt(PthreadSync::kClockRealtime, abstime->tv_sec, abstime->tv_nsec));
}

int APS5_VABI pthread_rwlock_timedwrlock_nid_postfix(PthreadRwlock* rwlock, const KernelTimespec* abstime) {
    if (!abstime) return PthreadSync::kErrorInvalid;
    return PthreadSync::RwlockLock(rwlock, true, PthreadSync::DeadlineAt(PthreadSync::kClockRealtime, abstime->tv_sec, abstime->tv_nsec));
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
