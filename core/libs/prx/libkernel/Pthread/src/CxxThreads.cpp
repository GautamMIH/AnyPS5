#include "../include/Pthread.hpp"
#include "../include/PthreadSync.hpp"
#include "prx/libc/include/General.hpp"
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

constexpr int kThrdSuccess = 0;
constexpr int kThrdNoMemory = 1;
constexpr int kThrdTimedOut = 2;
constexpr int kThrdBusy = 3;
constexpr int kThrdError = 4;
constexpr int kMtxRecursive = 0x100;

struct Xtime {
    std::int64_t sec;
    std::int64_t nsec;
};

int toThrdResult(const int errorNumber) {
    switch (errorNumber) {
    case 0: return kThrdSuccess;
    case PthreadSync::kErrorBusy: return kThrdBusy;
    case PthreadSync::kErrorTimedOut: return kThrdTimedOut;
    case PthreadSync::kErrorNoMemory: return kThrdNoMemory;
    default: return kThrdError;
    }
}

PthreadSync::Deadline deadlineOf(const Xtime* time) {
    if (time == nullptr)
        throw std::runtime_error("xtime: null pointer");
    return PthreadSync::DeadlineAt(PthreadSync::kClockRealtime, time->sec, time->nsec);
}

}

extern "C" {

int APS5_VABI scePthreadJoin(Pthread thread, void** retval);

int APS5_VABI _Mtx_init_nid_postfix(PthreadMutex* mutex, int type) {
    return toThrdResult(PthreadSync::MutexCreate(mutex, (type & kMtxRecursive) != 0 ? MutexType::Recursive : MutexType::Normal));
}

int APS5_VABI _Mtx_init_with_name_nid_postfix(PthreadMutex* mutex, int type, const char*) {
    return _Mtx_init_nid_postfix(mutex, type);
}

void APS5_VABI _Mtx_destroy_nid_postfix(PthreadMutex* mutex) {
    PthreadSync::MutexDestroy(mutex);
}

int APS5_VABI _Mtx_lock_nid_postfix(PthreadMutex* mutex) {
    return toThrdResult(PthreadSync::MutexLock(mutex, std::nullopt));
}

int APS5_VABI _Mtx_trylock_nid_postfix(PthreadMutex* mutex) {
    return toThrdResult(PthreadSync::MutexTryLock(mutex));
}

int APS5_VABI _Mtx_timedlock_nid_postfix(PthreadMutex* mutex, const Xtime* time) {
    return toThrdResult(PthreadSync::MutexLock(mutex, deadlineOf(time)));
}

int APS5_VABI _Mtx_unlock_nid_postfix(PthreadMutex* mutex) {
    return toThrdResult(PthreadSync::MutexUnlock(mutex));
}

int APS5_VABI _Cnd_init_nid_postfix(PthreadCond* cond) {
    return toThrdResult(PthreadSync::CondCreate(cond, PthreadSync::kClockRealtime));
}

void APS5_VABI _Cnd_destroy_nid_postfix(PthreadCond* cond) {
    PthreadSync::CondDestroy(cond);
}

int APS5_VABI _Cnd_wait_nid_postfix(PthreadCond* cond, PthreadMutex* mutex) {
    return toThrdResult(PthreadSync::CondWait(cond, mutex, std::nullopt));
}

int APS5_VABI _Cnd_timedwait_nid_postfix(PthreadCond* cond, PthreadMutex* mutex, const Xtime* time) {
    return toThrdResult(PthreadSync::CondWait(cond, mutex, deadlineOf(time)));
}

int APS5_VABI _Cnd_signal_nid_postfix(PthreadCond* cond) {
    return toThrdResult(PthreadSync::CondSignal(cond));
}

int APS5_VABI _Cnd_broadcast_nid_postfix(PthreadCond* cond) {
    return toThrdResult(PthreadSync::CondBroadcast(cond));
}

void APS5_VABI _Thrd_yield_nid_postfix() {
    std::this_thread::yield();
}

void APS5_VABI _Thrd_sleep_nid_postfix(const Xtime* time) {
    std::this_thread::sleep_until(*deadlineOf(time));
}

std::uint64_t APS5_VABI _Thrd_id_nid_postfix() {
#if defined(__linux__)
    return static_cast<std::uint64_t>(syscall(SYS_gettid));
#else
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
}

int APS5_VABI _Thrd_join_nid_postfix(Pthread thread, int* result) {
    void* value = nullptr;
    if (scePthreadJoin(thread, &value) != 0)
        return kThrdError;
    if (result != nullptr)
        *result = static_cast<int>(reinterpret_cast<std::intptr_t>(value));
    return kThrdSuccess;
}

// _Xtime_get_ticks is a libc export (OpenOrbis lists it under libc/LibcInternal); it lives in
// prx/libc/src/Time.cpp.

}
