#include "prx/libkernel/Time/include/StallWatch.hpp"
#include <cstdint>
#include <cstddef>
#include <new>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "../include/PthreadSync.hpp"

extern "C" {

int APS5_VABI pthread_mutex_destroy_nid_postfix(PthreadMutex* mutex) {
    return PthreadSync::MutexDestroy(mutex);
}

int APS5_VABI pthread_mutex_init_nid_postfix(PthreadMutex* mutex, const PthreadMutexattr* attr) {
    // Without attributes a POSIX mutex has the default type, error-checking on FreeBSD.
    return PthreadSync::MutexCreate(mutex, attr && *attr ? (*attr)->type : MutexType::ErrorCheck);
}

int APS5_VABI pthread_mutex_lock_nid_postfix(PthreadMutex* mutex) {
    APS5_STALL_WATCH("mutex", reinterpret_cast<const void*>(mutex));
    return PthreadSync::MutexLock(mutex, std::nullopt);
}

int APS5_VABI pthread_mutex_timedlock_nid_postfix(PthreadMutex* mutex, const KernelTimespec* abstime) {
    if (!abstime) return PthreadSync::kErrorInvalid;
    return PthreadSync::MutexLock(mutex, PthreadSync::DeadlineAt(PthreadSync::kClockRealtime, abstime->tv_sec, abstime->tv_nsec));
}

int APS5_VABI pthread_mutex_trylock_nid_postfix(PthreadMutex* mutex) {
    return PthreadSync::MutexTryLock(mutex);
}

int APS5_VABI pthread_mutex_unlock_nid_postfix(PthreadMutex* mutex) {
    return PthreadSync::MutexUnlock(mutex);
}

int APS5_VABI pthread_mutexattr_destroy_nid_postfix(PthreadMutexattr* attr) {
    if (!attr || !*attr) return PthreadSync::kErrorInvalid;
    delete *attr;
    *attr = nullptr;
    return 0;
}

int APS5_VABI pthread_mutexattr_init_nid_postfix(PthreadMutexattr* attr) {
    if (!attr) return PthreadSync::kErrorInvalid;
    auto* created = new (std::nothrow) PthreadMutexattrPrivate{MutexType::ErrorCheck};
    if (!created) return PthreadSync::kErrorNoMemory;
    *attr = created;
    return 0;
}

int APS5_VABI pthread_mutexattr_setprotocol_nid_postfix(PthreadMutexattr* attr, int protocol) {
    if (!attr || !*attr || protocol < 0 || protocol > 2) return PthreadSync::kErrorInvalid;
    (*attr)->protocol = protocol;
    return 0;
}

int APS5_VABI pthread_mutexattr_settype_nid_postfix(PthreadMutexattr* attr, int type) {
    if (!attr || !*attr) return PthreadSync::kErrorInvalid;
    switch (type) {
    case 1: (*attr)->type = MutexType::ErrorCheck; break;
    case 2: (*attr)->type = MutexType::Recursive; break;
    case 3: (*attr)->type = MutexType::Normal; break;
    // PTHREAD_MUTEX_ADAPTIVE_NP spins before sleeping; a relock reports EDEADLK.
    case 4: (*attr)->type = MutexType::Adaptive; break;
    default: return PthreadSync::kErrorInvalid;
    }
    return 0;
}

}
