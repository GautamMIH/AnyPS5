#include "../include/Pthread.hpp"
#include "../include/PthreadSync.hpp"
#include "prx/libc/include/General.hpp"
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
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
    // Relocking a plain mutex the caller holds is _Thrd_busy in Dinkumware (upstream libc Threads.cpp).
    case PthreadSync::kErrorDeadlock: return kThrdBusy;
    case PthreadSync::kErrorTimedOut: return kThrdTimedOut;
    case PthreadSync::kErrorNoMemory: return kThrdNoMemory;
    default: return kThrdError;
    }
}

PthreadCond* checkedCond(PthreadCond* cond, const char* function) {
    if (cond == nullptr || *cond == nullptr || (*cond)->_tag != kPthreadCondTag)
        throw std::invalid_argument(std::string(function) + ": not a condition created by _Cnd_init");
    return cond;
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

// The Dinkumware handle is cleared, as _Cnd_destroy does (upstream libc Threads.cpp): a destroyed
// _Mtx_t reads as null, not as the POSIX destroyed-mutex marker. Destroying a held mutex is a
// logic error there; the mutex is left intact.
void APS5_VABI _Mtx_destroy_nid_postfix(PthreadMutex* mutex) {
    if (mutex == nullptr) throw std::invalid_argument("_Mtx_destroy: null mutex handle pointer");
    if (*mutex == nullptr) return;
    const int result = PthreadSync::MutexDestroy(mutex);
    if (result == PthreadSync::kErrorBusy) throw std::logic_error("_Mtx_destroy: mutex destroyed while locked");
    *mutex = nullptr;
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

// Unlocking a mutex the caller does not hold changes nothing and succeeds (upstream libc
// Threads.cpp); a handle that was never created, or was destroyed, is a caller error.
int APS5_VABI _Mtx_unlock_nid_postfix(PthreadMutex* mutex) {
    if (mutex == nullptr || *mutex == nullptr) throw std::invalid_argument("_Mtx_unlock: null mutex handle");
    const int result = PthreadSync::MutexUnlock(mutex);
    return result == PthreadSync::kErrorPermission ? kThrdSuccess : toThrdResult(result);
}

int APS5_VABI _Cnd_init_nid_postfix(PthreadCond* cond) {
    if (cond == nullptr) throw std::invalid_argument("_Cnd_init: null handle pointer");
    *cond = nullptr;
    return toThrdResult(PthreadSync::CondCreate(cond, PthreadSync::kClockRealtime));
}

// Unlike POSIX, a null _Cnd_t is not statically initialized: the C++ runtime throws for a null,
// destroyed or foreign handle (upstream libc Threads.cpp).
void APS5_VABI _Cnd_destroy_nid_postfix(PthreadCond* cond) {
    if (cond == nullptr) throw std::invalid_argument("_Cnd_destroy: null condition handle pointer");
    if (*cond == nullptr) return;
    checkedCond(cond, "_Cnd_destroy");
    (*cond)->_tag = 0;
    PthreadSync::CondDestroy(cond);
}

int APS5_VABI _Cnd_wait_nid_postfix(PthreadCond* cond, PthreadMutex* mutex) {
    return toThrdResult(PthreadSync::CondWait(checkedCond(cond, "_Cnd_wait"), mutex, std::nullopt));
}

int APS5_VABI _Cnd_timedwait_nid_postfix(PthreadCond* cond, PthreadMutex* mutex, const Xtime* time) {
    return toThrdResult(PthreadSync::CondWait(checkedCond(cond, "_Cnd_timedwait"), mutex, deadlineOf(time)));
}

int APS5_VABI _Cnd_signal_nid_postfix(PthreadCond* cond) {
    return toThrdResult(PthreadSync::CondSignal(checkedCond(cond, "_Cnd_signal")));
}

int APS5_VABI _Cnd_broadcast_nid_postfix(PthreadCond* cond) {
    return toThrdResult(PthreadSync::CondBroadcast(checkedCond(cond, "_Cnd_broadcast")));
}

void APS5_VABI _Thrd_yield_nid_postfix() {
    std::this_thread::yield();
}

// The console's libc.prx _Thrd_sleep takes a second xtime* (remaining) and returns int (upstream
// fddcb00a4).
int APS5_VABI _Thrd_sleep_nid_postfix(const Xtime* time, Xtime*) {
    if (time == nullptr) APS5_INVALID_ARG_EX;
    std::this_thread::sleep_until(*deadlineOf(time));
    return 0;
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
