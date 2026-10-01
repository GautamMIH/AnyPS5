#include "prx/libkernel/Time/include/StallWatch.hpp"
#include "../include/Pthread.hpp"
#include "../include/PthreadSync.hpp"
#include "prx/libc/include/General.hpp"
#include <new>
#include <stdexcept>

static constexpr int SCE_OK = 0;

extern "C" {

int APS5_VABI scePthreadMutexattrInit(PthreadMutexattr* attr) {
    if (!attr) throw std::runtime_error("scePthreadMutexattrInit: null attr");
    auto* p = new (std::nothrow) PthreadMutexattrPrivate{MutexType::Normal};
    if (!p) return PthreadSync::SceError(PthreadSync::kErrorNoMemory);
    *attr = p;
    return SCE_OK;
}

int APS5_VABI scePthreadMutexattrDestroy(PthreadMutexattr* attr) {
    if (!attr || !*attr) throw std::runtime_error("scePthreadMutexattrDestroy: null attr");
    delete *attr;
    *attr = nullptr;
    return SCE_OK;
}

int APS5_VABI scePthreadMutexattrSettype(PthreadMutexattr* attr, int type) {
    if (!attr || !*attr) throw std::runtime_error("scePthreadMutexattrSettype: null attr");
    switch (type) {
    case 1: (*attr)->type = MutexType::ErrorCheck; break;
    case 2: (*attr)->type = MutexType::Recursive; break;
    case 3: (*attr)->type = MutexType::Normal; break;
    // PTHREAD_MUTEX_ADAPTIVE_NP spins before sleeping; otherwise it behaves as a normal mutex.
    case 4: (*attr)->type = MutexType::Normal; break;
    default: return PthreadSync::SceError(PthreadSync::kErrorInvalid);
    }
    return SCE_OK;
}

int APS5_VABI scePthreadMutexattrSetprotocol(PthreadMutexattr* attr, int protocol) {
    if (!attr || !*attr) throw std::runtime_error("scePthreadMutexattrSetprotocol: null attr");
    if (protocol < 0 || protocol > 2) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
    (*attr)->protocol = protocol;
    return SCE_OK;
}

int APS5_VABI scePthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* attr, const char*) {
    if (!mutex) throw std::runtime_error("scePthreadMutexInit: null mutex");
    return PthreadSync::SceError(PthreadSync::MutexCreate(mutex, attr && *attr ? (*attr)->type : MutexType::Normal));
}

int APS5_VABI scePthreadMutexDestroy(PthreadMutex* mutex) {
    return PthreadSync::SceError(PthreadSync::MutexDestroy(mutex));
}

int APS5_VABI scePthreadMutexLock(PthreadMutex* mutex) {
    APS5_STALL_WATCH("mutex", reinterpret_cast<const void*>(mutex));
    return PthreadSync::SceError(PthreadSync::MutexLock(mutex, std::nullopt));
}

int APS5_VABI scePthreadMutexUnlock(PthreadMutex* mutex) {
    return PthreadSync::SceError(PthreadSync::MutexUnlock(mutex));
}

int APS5_VABI scePthreadMutexTimedlock(PthreadMutex* mutex, KernelUseconds usec) {
    return PthreadSync::SceError(PthreadSync::MutexLock(mutex, PthreadSync::DeadlineAfterMicroseconds(usec)));
}

int APS5_VABI scePthreadMutexTrylock(PthreadMutex* mutex) {
    return PthreadSync::SceError(PthreadSync::MutexTryLock(mutex));
}

}
