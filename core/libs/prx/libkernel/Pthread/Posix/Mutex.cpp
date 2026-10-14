#include "prx/libkernel/Time/include/StallWatch.hpp"
#include <cstdint>
#include <cstddef>
#include <new>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "../include/PthreadSync.hpp"
#include <stdexcept>

namespace {

constexpr int kPrioProtect = 2;
constexpr int kSchedRr = 3;

}

extern "C" {

int APS5_VABI sched_get_priority_max_nid_postfix(int policy);

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
    // Like FreeBSD, the deadline is only checked when the lock would block: a free mutex is taken
    // whatever the deadline, a held one with a malformed deadline fails with EINVAL.
    if (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000) {
        return PthreadSync::MutexTryLock(mutex) == 0 ? 0 : PthreadSync::kErrorInvalid;
    }
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
    if (!attr || !*attr || protocol < 0 || protocol > kPrioProtect) return PthreadSync::kErrorInvalid;
    (*attr)->protocol = protocol;
    (*attr)->ceiling = sched_get_priority_max_nid_postfix(kSchedRr);
    return 0;
}

int APS5_VABI pthread_mutexattr_setprioceiling_nid_postfix(PthreadMutexattr* attr, int prioceiling) {
    if (!attr || !*attr) throw std::runtime_error("pthread_mutexattr_setprioceiling: null attr");
    if ((*attr)->protocol != kPrioProtect) return PthreadSync::kErrorInvalid;
    (*attr)->ceiling = prioceiling;
    return 0;
}

int APS5_VABI pthread_mutexattr_getprioceiling_nid_postfix(const PthreadMutexattr* attr, int* prioceiling) {
    if (!attr || !*attr) throw std::runtime_error("pthread_mutexattr_getprioceiling: null attr");
    if (!prioceiling) throw std::runtime_error("pthread_mutexattr_getprioceiling: null prioceiling");
    if ((*attr)->protocol != kPrioProtect) return PthreadSync::kErrorInvalid;
    *prioceiling = (*attr)->ceiling;
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

int APS5_VABI pthread_mutexattr_gettype_nid_postfix(const PthreadMutexattr* attr, int* type) {
    if (!attr || !*attr) return PthreadSync::kErrorInvalid;
    if (!type) throw std::runtime_error("pthread_mutexattr_gettype: null type");
    *type = static_cast<int>((*attr)->type);
    return 0;
}

}
