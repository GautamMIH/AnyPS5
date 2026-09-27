#include "../include/Pthread.hpp"
#include "../include/PthreadSync.hpp"
#include "prx/libc/include/General.hpp"
#include <new>
#include <stdexcept>

static constexpr int SCE_OK = 0;

extern "C" {

int APS5_VABI scePthreadCondattrInit(PthreadCondattr* attr) {
    if (!attr) throw std::runtime_error("scePthreadCondattrInit: null attr");
    auto* p = new (std::nothrow) PthreadCondattrPrivate{PthreadSync::kClockRealtime};
    if (!p) return PthreadSync::SceError(PthreadSync::kErrorNoMemory);
    *attr = p;
    return SCE_OK;
}

int APS5_VABI scePthreadCondattrDestroy(PthreadCondattr* attr) {
    if (!attr || !*attr) throw std::runtime_error("scePthreadCondattrDestroy: null attr");
    delete *attr;
    *attr = nullptr;
    return SCE_OK;
}

int APS5_VABI scePthreadCondInit(PthreadCond* cond, const PthreadCondattr* attr, const char*) {
    return PthreadSync::SceError(PthreadSync::CondCreate(cond, attr && *attr ? (*attr)->_clockid : PthreadSync::kClockRealtime));
}

int APS5_VABI scePthreadCondDestroy(PthreadCond* cond) {
    return PthreadSync::SceError(PthreadSync::CondDestroy(cond));
}

int APS5_VABI scePthreadCondSignal(PthreadCond* cond) {
    return PthreadSync::SceError(PthreadSync::CondSignal(cond));
}

int APS5_VABI scePthreadCondBroadcast(PthreadCond* cond) {
    return PthreadSync::SceError(PthreadSync::CondBroadcast(cond));
}

int APS5_VABI scePthreadCondSignalto(PthreadCond* cond, Pthread) {
    return PthreadSync::SceError(PthreadSync::CondBroadcast(cond));
}

int APS5_VABI scePthreadCondWait(PthreadCond* cond, PthreadMutex* mutex) {
    return PthreadSync::SceError(PthreadSync::CondWait(cond, mutex, std::nullopt));
}

int APS5_VABI scePthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex, unsigned int usec) {
    return PthreadSync::SceError(PthreadSync::CondWait(cond, mutex, PthreadSync::DeadlineAfterMicroseconds(usec)));
}

}
