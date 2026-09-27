#include "../include/Pthread.hpp"
#include "../include/PthreadSync.hpp"
#include "prx/libc/include/General.hpp"
#include <new>

extern "C" {

int APS5_VABI scePthreadRwlockDestroy(PthreadRwlock* rwlock) {
    return PthreadSync::SceError(PthreadSync::RwlockDestroy(rwlock));
}

int APS5_VABI scePthreadRwlockInit(PthreadRwlock* rwlock, const PthreadRwlockattr*, const char*) {
    return PthreadSync::SceError(PthreadSync::RwlockCreate(rwlock));
}

int APS5_VABI scePthreadRwlockRdlock(PthreadRwlock* rwlock) {
    return PthreadSync::SceError(PthreadSync::RwlockLock(rwlock, false, std::nullopt));
}

int APS5_VABI scePthreadRwlockWrlock(PthreadRwlock* rwlock) {
    return PthreadSync::SceError(PthreadSync::RwlockLock(rwlock, true, std::nullopt));
}

int APS5_VABI scePthreadRwlockTimedrdlock(PthreadRwlock* rwlock, KernelUseconds usec) {
    return PthreadSync::SceError(PthreadSync::RwlockLock(rwlock, false, PthreadSync::DeadlineAfterMicroseconds(usec)));
}

int APS5_VABI scePthreadRwlockTimedwrlock(PthreadRwlock* rwlock, KernelUseconds usec) {
    return PthreadSync::SceError(PthreadSync::RwlockLock(rwlock, true, PthreadSync::DeadlineAfterMicroseconds(usec)));
}

int APS5_VABI scePthreadRwlockTryrdlock(PthreadRwlock* rwlock) {
    return PthreadSync::SceError(PthreadSync::RwlockTryLock(rwlock, false));
}

int APS5_VABI scePthreadRwlockTrywrlock(PthreadRwlock* rwlock) {
    return PthreadSync::SceError(PthreadSync::RwlockTryLock(rwlock, true));
}

int APS5_VABI scePthreadRwlockUnlock(PthreadRwlock* rwlock) {
    return PthreadSync::SceError(PthreadSync::RwlockUnlock(rwlock));
}

int APS5_VABI scePthreadRwlockattrDestroy(PthreadRwlockattr* attr) {
    if (!attr || !*attr) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
    delete *attr;
    *attr = nullptr;
    return 0;
}

int APS5_VABI scePthreadRwlockattrInit(PthreadRwlockattr* attr) {
    if (!attr) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
    auto* created = new (std::nothrow) PthreadRwlockattrPrivate{};
    if (!created) return PthreadSync::SceError(PthreadSync::kErrorNoMemory);
    *attr = created;
    return 0;
}

int APS5_VABI scePthreadRwlockattrSettype(PthreadRwlockattr* attr, int type) {
    if (!attr || !*attr) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
    (*attr)->type = type;
    return 0;
}

}
