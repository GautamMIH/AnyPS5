#ifndef CORE_LIBS_PRX_LIBKERNEL_PTHREAD_PTHREADSYNC_HPP
#define CORE_LIBS_PRX_LIBKERNEL_PTHREAD_PTHREADSYNC_HPP

#include "Pthread.hpp"
#include <chrono>
#include <cstdint>
#include <optional>

namespace PthreadSync {

inline constexpr int kErrorPermission = 1;
inline constexpr int kErrorDeadlock = 11;
inline constexpr int kErrorNoMemory = 12;
inline constexpr int kErrorBusy = 16;
inline constexpr int kErrorInvalid = 22;
inline constexpr int kErrorTimedOut = 60;
inline constexpr int kClockRealtime = 0;
inline constexpr int kClockMonotonic = 4;

using Deadline = std::optional<std::chrono::steady_clock::time_point>;

inline int SceError(const int errorNumber) {
    return errorNumber == 0 ? 0 : static_cast<int>(0x80020000u | static_cast<unsigned>(errorNumber));
}

Deadline DeadlineAfterMicroseconds(std::uint64_t microseconds);
Deadline DeadlineAt(int clockId, std::int64_t seconds, std::int64_t nanoseconds);

int MutexCreate(PthreadMutex* mutex, MutexType type);
int MutexDestroy(PthreadMutex* mutex);
int MutexLock(PthreadMutex* mutex, const Deadline& deadline);
int MutexTryLock(PthreadMutex* mutex);
int MutexUnlock(PthreadMutex* mutex);

int CondCreate(PthreadCond* cond, int clockId);
int CondDestroy(PthreadCond* cond);
int CondWait(PthreadCond* cond, PthreadMutex* mutex, const Deadline& deadline);
int CondSignal(PthreadCond* cond);
int CondBroadcast(PthreadCond* cond);
int CondClock(PthreadCond* cond);

int RwlockCreate(PthreadRwlock* rwlock);
int RwlockDestroy(PthreadRwlock* rwlock);
int RwlockLock(PthreadRwlock* rwlock, bool exclusive, const Deadline& deadline);
int RwlockTryLock(PthreadRwlock* rwlock, bool exclusive);
int RwlockUnlock(PthreadRwlock* rwlock);

int KeyCreate(PthreadKey* key, pthread_key_destructor_func_t destructor);
int KeyDelete(PthreadKey key);
void* KeyGet(PthreadKey key);
int KeySet(PthreadKey key, void* value);

}

#endif
