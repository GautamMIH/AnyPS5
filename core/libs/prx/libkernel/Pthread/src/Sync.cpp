#include "../include/PthreadSync.hpp"
#include "../include/Cancel.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>

namespace PthreadSync {

namespace {

constexpr std::uintptr_t kAdaptiveMutexInitializer = 1;
// A destroyed mutex holds this marker (FreeBSD's THR_MUTEX_DESTROYED), so a later use reports EINVAL
// instead of initializing it again (upstream 709c12e13).
constexpr std::uintptr_t kDestroyedMutex = 2;

bool isDestroyedMutex(PthreadMutexPrivate* value) {
    return reinterpret_cast<std::uintptr_t>(value) == kDestroyedMutex;
}

std::mutex& staticInitializationMutex() {
    static std::mutex instance;
    return instance;
}

bool isStaticMutexInitializer(PthreadMutexPrivate* value) {
    const auto raw = reinterpret_cast<std::uintptr_t>(value);
    return raw == 0 || raw == kAdaptiveMutexInitializer;
}

PthreadMutexPrivate* resolveMutex(PthreadMutex* mutex) {
    if (mutex == nullptr)
        throw std::runtime_error("pthread mutex: null pointer");
    std::atomic_ref<PthreadMutexPrivate*> slot(*mutex);
    PthreadMutexPrivate* current = slot.load(std::memory_order_acquire);
    if (isDestroyedMutex(current))
        return nullptr;
    if (!isStaticMutexInitializer(current))
        return current;
    const std::lock_guard lock(staticInitializationMutex());
    current = slot.load(std::memory_order_acquire);
    if (isStaticMutexInitializer(current)) {
        // FreeBSD: PTHREAD_MUTEX_INITIALIZER (0) is an error-checking mutex, the adaptive
        // initializer (1) an adaptive one.
        const bool adaptive = reinterpret_cast<std::uintptr_t>(current) == kAdaptiveMutexInitializer;
        current = new PthreadMutexPrivate();
        current->_type = adaptive ? MutexType::Adaptive : MutexType::ErrorCheck;
        slot.store(current, std::memory_order_release);
    }
    return current;
}

PthreadCondPrivate* resolveCond(PthreadCond* cond) {
    if (cond == nullptr)
        throw std::runtime_error("pthread cond: null pointer");
    std::atomic_ref<PthreadCondPrivate*> slot(*cond);
    PthreadCondPrivate* current = slot.load(std::memory_order_acquire);
    if (current != nullptr)
        return current;
    const std::lock_guard lock(staticInitializationMutex());
    current = slot.load(std::memory_order_acquire);
    if (current == nullptr) {
        current = new PthreadCondPrivate();
        slot.store(current, std::memory_order_release);
    }
    return current;
}

// FreeBSD's THR_RWLOCK_DESTROYED: a destroyed rwlock reports EINVAL instead of initializing again.
constexpr std::uintptr_t kDestroyedRwlock = 1;

bool isDestroyedRwlock(PthreadRwlockPrivate* value) {
    return reinterpret_cast<std::uintptr_t>(value) == kDestroyedRwlock;
}

// Read locks the calling thread holds on any rwlock (libthr's curthread->rdlock_count): such a
// thread may take another read lock past a waiting writer, which would otherwise deadlock it.
thread_local int heldReadLocks = 0;

PthreadRwlockPrivate* resolveRwlock(PthreadRwlock* rwlock) {
    if (rwlock == nullptr)
        throw std::runtime_error("pthread rwlock: null pointer");
    std::atomic_ref<PthreadRwlockPrivate*> slot(*rwlock);
    PthreadRwlockPrivate* current = slot.load(std::memory_order_acquire);
    if (isDestroyedRwlock(current))
        return nullptr;
    if (current != nullptr)
        return current;
    const std::lock_guard lock(staticInitializationMutex());
    current = slot.load(std::memory_order_acquire);
    if (current == nullptr) {
        current = new PthreadRwlockPrivate();
        slot.store(current, std::memory_order_release);
    }
    return isDestroyedRwlock(current) ? nullptr : current;
}

// Deadlines stay within about 136 years of now, so far-off times (INT64_MIN, INT64_MAX seconds)
// neither overflow the clocks nor wrap around.
constexpr std::int64_t kDeadlineLimitSeconds = std::int64_t{1} << 32;

std::int64_t clampSeconds(const std::int64_t value) {
    return std::clamp(value, -kDeadlineLimitSeconds, kDeadlineLimitSeconds);
}

}

Deadline DeadlineAfterMicroseconds(const std::uint64_t microseconds) {
    const auto bounded = std::min<std::uint64_t>(microseconds, static_cast<std::uint64_t>(kDeadlineLimitSeconds) * 1000000u);
    return std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<std::int64_t>(bounded));
}

Deadline DeadlineAt(const int clockId, std::int64_t seconds, const std::int64_t nanoseconds) {
    // Whole seconds in nanoseconds carry into seconds (C++ xtime is not normalized; POSIX callers
    // reject an out-of-range tv_nsec before this). Saturate so the sum cannot overflow.
    const std::int64_t carry = nanoseconds / 1000000000;
    seconds = carry > 0 && seconds > std::numeric_limits<std::int64_t>::max() - carry ? std::numeric_limits<std::int64_t>::max()
        : carry < 0 && seconds < std::numeric_limits<std::int64_t>::min() - carry ? std::numeric_limits<std::int64_t>::min()
        : seconds + carry;
    const auto nanos = std::chrono::nanoseconds(nanoseconds % 1000000000);
    if (clockId == kClockMonotonic) {
        const auto target = std::chrono::seconds(clampSeconds(seconds)) + nanos;
        return std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(target));
    }
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    const std::int64_t current = nowSeconds.count();
    // seconds - now without overflow: now is far inside the range, so only the far ends saturate.
    const std::int64_t deltaSeconds = seconds < -kDeadlineLimitSeconds ? -kDeadlineLimitSeconds : seconds > kDeadlineLimitSeconds + current ? kDeadlineLimitSeconds : seconds - current;
    const auto remaining = std::chrono::seconds(clampSeconds(deltaSeconds)) + nanos - std::chrono::duration_cast<std::chrono::nanoseconds>(now - nowSeconds);
    return std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(remaining);
}

int MutexCreate(PthreadMutex* mutex, const MutexType type) {
    if (mutex == nullptr)
        throw std::runtime_error("pthread mutex: null pointer");
    auto* created = new (std::nothrow) PthreadMutexPrivate();
    if (created == nullptr)
        return kErrorNoMemory;
    created->_type = type;
    *mutex = created;
    return 0;
}

int MutexDestroy(PthreadMutex* mutex) {
    if (mutex == nullptr)
        throw std::runtime_error("pthread mutex: null pointer");
    if (isDestroyedMutex(*mutex))
        return kErrorInvalid;
    // A locked mutex cannot be destroyed (upstream d77bc7c36).
    if (!isStaticMutexInitializer(*mutex) && (*mutex)->_owner.load(std::memory_order_acquire) != std::thread::id{})
        return kErrorBusy;
    if (!isStaticMutexInitializer(*mutex))
        delete *mutex;
    *mutex = reinterpret_cast<PthreadMutex>(kDestroyedMutex);
    return 0;
}

int MutexLock(PthreadMutex* mutex, const Deadline& deadline) {
    auto* m = resolveMutex(mutex);
    if (m == nullptr)
        return kErrorInvalid;
    const auto self = std::this_thread::get_id();
    if (m->_type == MutexType::Recursive) {
        if (deadline) {
            if (!m->_rmtx.try_lock_until(*deadline))
                return kErrorTimedOut;
        } else {
            m->_rmtx.lock();
        }
        m->_owner.store(self, std::memory_order_relaxed);
        ++m->_count;
        return 0;
    }
    if (m->_owner.load(std::memory_order_acquire) == self)
        return kErrorDeadlock;
    if (deadline) {
        if (!m->_mtx.try_lock_until(*deadline))
            return kErrorTimedOut;
    } else {
        m->_mtx.lock();
    }
    m->_owner.store(self, std::memory_order_relaxed);
    return 0;
}

int MutexTryLock(PthreadMutex* mutex) {
    auto* m = resolveMutex(mutex);
    if (m == nullptr)
        return kErrorInvalid;
    const auto self = std::this_thread::get_id();
    if (m->_type == MutexType::Recursive) {
        if (!m->_rmtx.try_lock())
            return kErrorBusy;
        m->_owner.store(self, std::memory_order_relaxed);
        ++m->_count;
        return 0;
    }
    if (m->_owner.load(std::memory_order_acquire) == self || !m->_mtx.try_lock())
        return kErrorBusy;
    m->_owner.store(self, std::memory_order_relaxed);
    return 0;
}

int MutexUnlock(PthreadMutex* mutex) {
    auto* m = resolveMutex(mutex);
    if (m == nullptr)
        return kErrorInvalid;
    if (m->_owner.load(std::memory_order_acquire) != std::this_thread::get_id())
        return kErrorPermission;
    if (m->_type == MutexType::Recursive) {
        if (--m->_count == 0)
            m->_owner.store(std::thread::id{}, std::memory_order_relaxed);
        m->_rmtx.unlock();
        return 0;
    }
    m->_owner.store(std::thread::id{}, std::memory_order_relaxed);
    m->_mtx.unlock();
    return 0;
}

int CondCreate(PthreadCond* cond, const int clockId) {
    if (cond == nullptr)
        throw std::runtime_error("pthread cond: null pointer");
    auto* created = new (std::nothrow) PthreadCondPrivate();
    if (created == nullptr)
        return kErrorNoMemory;
    created->_clockid = clockId;
    *cond = created;
    return 0;
}

int CondDestroy(PthreadCond* cond) {
    if (cond == nullptr)
        throw std::runtime_error("pthread cond: null pointer");
    delete *cond;
    *cond = nullptr;
    return 0;
}

// Waits once, registered with the thread's cancel state so scePthreadCancel can wake it; the
// registration ends before the mutex is taken back. Returns false on a timeout.
template <class TLock>
bool cancellableWait(std::condition_variable_any& condition, TLock& lock, const Deadline& deadline) {
    ThreadCancel::BeginWait(&condition);
    if (ThreadCancel::Requested()) {
        ThreadCancel::EndWait();
        return true;
    }
    ThreadCancel::WaitLock<TLock> waitLock{lock};
    if (deadline)
        return condition.wait_until(waitLock, *deadline) != std::cv_status::timeout;
    condition.wait(waitLock);
    return true;
}

int CondWait(PthreadCond* cond, PthreadMutex* mutex, const Deadline& deadline) {
    auto* c = resolveCond(cond);
    auto* m = resolveMutex(mutex);
    if (m == nullptr)
        return kErrorInvalid;
    const auto self = std::this_thread::get_id();
    if (m->_owner.load(std::memory_order_acquire) != self)
        return kErrorPermission;
    // A cond wait is a cancellation point: a pending cancel is acted on before the wait and after
    // it (with the mutex held again); a blocked waiter is woken by the cancel (upstream 3acca27f).
    ThreadCancel::Check();

    bool timedOut = false;
    if (m->_type == MutexType::Recursive) {
        const int depth = m->_count;
        for (int index = 1; index < depth; ++index)
            m->_rmtx.unlock();
        m->_count = 0;
        m->_owner.store(std::thread::id{}, std::memory_order_relaxed);
        std::unique_lock lock(m->_rmtx, std::adopt_lock);
        timedOut = !cancellableWait(c->_cv, lock, deadline);
        lock.release();
        for (int index = 1; index < depth; ++index)
            m->_rmtx.lock();
        m->_count = depth;
    } else {
        m->_owner.store(std::thread::id{}, std::memory_order_relaxed);
        std::unique_lock lock(m->_mtx, std::adopt_lock);
        timedOut = !cancellableWait(c->_cv, lock, deadline);
        lock.release();
    }
    m->_owner.store(self, std::memory_order_relaxed);
    ThreadCancel::Check();
    return timedOut ? kErrorTimedOut : 0;
}

int CondSignal(PthreadCond* cond) {
    resolveCond(cond)->_cv.notify_one();
    return 0;
}

int CondBroadcast(PthreadCond* cond) {
    resolveCond(cond)->_cv.notify_all();
    return 0;
}

int CondClock(PthreadCond* cond) {
    return resolveCond(cond)->_clockid;
}

int RwlockCreate(PthreadRwlock* rwlock) {
    if (rwlock == nullptr)
        throw std::runtime_error("pthread rwlock: null pointer");
    auto* created = new (std::nothrow) PthreadRwlockPrivate();
    if (created == nullptr)
        return kErrorNoMemory;
    *rwlock = created;
    return 0;
}

// Like libthr, destroying a held rwlock succeeds and marks it destroyed. The object is only freed
// when nothing holds or waits on it; a held one is left allocated, so a late unlock or a waiter
// never touches freed memory.
int RwlockDestroy(PthreadRwlock* rwlock) {
    if (rwlock == nullptr)
        throw std::runtime_error("pthread rwlock: null pointer");
    PthreadRwlockPrivate* current = nullptr;
    {
        const std::lock_guard lock(staticInitializationMutex());
        current = *rwlock;
        if (current == nullptr)
            return 0;
        if (isDestroyedRwlock(current))
            return kErrorInvalid;
        std::atomic_ref<PthreadRwlockPrivate*>(*rwlock).store(reinterpret_cast<PthreadRwlockPrivate*>(kDestroyedRwlock), std::memory_order_release);
    }
    bool idle = false;
    {
        const std::lock_guard lock(current->mutex);
        idle = current->writer == std::thread::id{} && current->readers == 0 && current->waitingReaders == 0 && current->waitingWriters == 0;
    }
    if (idle)
        delete current;
    return 0;
}

namespace {

template <class TPredicate>
bool waitRwlock(std::condition_variable& wake, std::unique_lock<std::mutex>& lock, const Deadline& deadline, TPredicate ready) {
    if (!deadline) {
        wake.wait(lock, ready);
        return true;
    }
    return wake.wait_until(lock, *deadline, ready);
}

// A thread that holds no read lock waits behind a writer that is waiting (writer preference).
bool canRead(const PthreadRwlockPrivate& r) {
    return r.writer == std::thread::id{} && (r.waitingWriters == 0 || heldReadLocks > 0);
}

bool canWrite(const PthreadRwlockPrivate& r) {
    return r.writer == std::thread::id{} && r.readers == 0;
}

void wakeWaiters(PthreadRwlockPrivate& r) {
    if (r.writer != std::thread::id{})
        return;
    if (r.readers == 0 && r.waitingWriters > 0)
        r.writersWake.notify_all();
    if (r.waitingReaders > 0)
        r.readersWake.notify_all();
}

}

int RwlockLock(PthreadRwlock* rwlock, const bool exclusive, const Deadline& deadline) {
    auto* r = resolveRwlock(rwlock);
    if (r == nullptr)
        return kErrorInvalid;
    const auto self = std::this_thread::get_id();
    std::unique_lock lock(r->mutex);
    if (r->writer == self)
        return kErrorDeadlock;
    if (exclusive) {
        if (!canWrite(*r)) {
            ++r->waitingWriters;
            const bool acquired = waitRwlock(r->writersWake, lock, deadline, [r] { return canWrite(*r); });
            --r->waitingWriters;
            if (!acquired) {
                // Readers held back by this writer may go now.
                wakeWaiters(*r);
                return kErrorTimedOut;
            }
        }
        r->writer = self;
        return 0;
    }
    if (!canRead(*r)) {
        ++r->waitingReaders;
        const bool acquired = waitRwlock(r->readersWake, lock, deadline, [r] { return canRead(*r); });
        --r->waitingReaders;
        if (!acquired)
            return kErrorTimedOut;
    }
    ++r->readers;
    ++heldReadLocks;
    return 0;
}

int RwlockTryLock(PthreadRwlock* rwlock, const bool exclusive) {
    auto* r = resolveRwlock(rwlock);
    if (r == nullptr)
        return kErrorInvalid;
    const std::lock_guard lock(r->mutex);
    if (exclusive) {
        if (!canWrite(*r))
            return kErrorBusy;
        r->writer = std::this_thread::get_id();
        return 0;
    }
    if (!canRead(*r))
        return kErrorBusy;
    ++r->readers;
    ++heldReadLocks;
    return 0;
}

// libthr: the write owner releases its write lock; otherwise a read lock is released, and an
// rwlock that is unheld, or write-held by another thread, reports EPERM.
int RwlockUnlock(PthreadRwlock* rwlock) {
    auto* r = resolveRwlock(rwlock);
    if (r == nullptr)
        return kErrorInvalid;
    const std::lock_guard lock(r->mutex);
    if (r->writer == std::this_thread::get_id()) {
        r->writer = std::thread::id{};
    } else if (r->writer != std::thread::id{} || r->readers == 0) {
        return kErrorPermission;
    } else {
        --r->readers;
        if (heldReadLocks > 0)
            --heldReadLocks;
    }
    wakeWaiters(*r);
    return 0;
}

}

