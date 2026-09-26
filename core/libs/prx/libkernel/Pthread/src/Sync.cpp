#include "../include/PthreadSync.hpp"
#include <atomic>
#include <mutex>
#include <new>
#include <stdexcept>

namespace PthreadSync {

namespace {

constexpr std::uintptr_t kAdaptiveMutexInitializer = 1;

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
    if (!isStaticMutexInitializer(current))
        return current;
    const std::lock_guard lock(staticInitializationMutex());
    current = slot.load(std::memory_order_acquire);
    if (isStaticMutexInitializer(current)) {
        current = new PthreadMutexPrivate();
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

PthreadRwlockPrivate* resolveRwlock(PthreadRwlock* rwlock) {
    if (rwlock == nullptr)
        throw std::runtime_error("pthread rwlock: null pointer");
    std::atomic_ref<PthreadRwlockPrivate*> slot(*rwlock);
    PthreadRwlockPrivate* current = slot.load(std::memory_order_acquire);
    if (current != nullptr)
        return current;
    const std::lock_guard lock(staticInitializationMutex());
    current = slot.load(std::memory_order_acquire);
    if (current == nullptr) {
        current = new PthreadRwlockPrivate();
        slot.store(current, std::memory_order_release);
    }
    return current;
}

}

Deadline DeadlineAfterMicroseconds(const std::uint64_t microseconds) {
    return std::chrono::steady_clock::now() + std::chrono::microseconds(microseconds);
}

Deadline DeadlineAt(const int clockId, const std::int64_t seconds, const std::int64_t nanoseconds) {
    const auto target = std::chrono::seconds(seconds) + std::chrono::nanoseconds(nanoseconds);
    if (clockId == kClockMonotonic)
        return std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(target));
    const auto remaining = target - std::chrono::system_clock::now().time_since_epoch();
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
    if (!isStaticMutexInitializer(*mutex))
        delete *mutex;
    *mutex = nullptr;
    return 0;
}

int MutexLock(PthreadMutex* mutex, const Deadline& deadline) {
    auto* m = resolveMutex(mutex);
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

int CondWait(PthreadCond* cond, PthreadMutex* mutex, const Deadline& deadline) {
    auto* c = resolveCond(cond);
    auto* m = resolveMutex(mutex);
    const auto self = std::this_thread::get_id();
    if (m->_owner.load(std::memory_order_acquire) != self)
        return kErrorPermission;

    bool timedOut = false;
    if (m->_type == MutexType::Recursive) {
        const int depth = m->_count;
        for (int index = 1; index < depth; ++index)
            m->_rmtx.unlock();
        m->_count = 0;
        m->_owner.store(std::thread::id{}, std::memory_order_relaxed);
        std::unique_lock lock(m->_rmtx, std::adopt_lock);
        if (deadline)
            timedOut = c->_cv.wait_until(lock, *deadline) == std::cv_status::timeout;
        else
            c->_cv.wait(lock);
        lock.release();
        for (int index = 1; index < depth; ++index)
            m->_rmtx.lock();
        m->_count = depth;
    } else {
        m->_owner.store(std::thread::id{}, std::memory_order_relaxed);
        std::unique_lock lock(m->_mtx, std::adopt_lock);
        if (deadline)
            timedOut = c->_cv.wait_until(lock, *deadline) == std::cv_status::timeout;
        else
            c->_cv.wait(lock);
        lock.release();
    }
    m->_owner.store(self, std::memory_order_relaxed);
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

int RwlockDestroy(PthreadRwlock* rwlock) {
    if (rwlock == nullptr)
        throw std::runtime_error("pthread rwlock: null pointer");
    delete *rwlock;
    *rwlock = nullptr;
    return 0;
}

int RwlockLock(PthreadRwlock* rwlock, const bool exclusive, const Deadline& deadline) {
    auto* r = resolveRwlock(rwlock);
    const auto self = std::this_thread::get_id();
    if (r->writer.load(std::memory_order_acquire) == self)
        return kErrorDeadlock;
    if (exclusive) {
        if (deadline) {
            if (!r->lock.try_lock_until(*deadline))
                return kErrorTimedOut;
        } else {
            r->lock.lock();
        }
        r->writer.store(self, std::memory_order_release);
        return 0;
    }
    if (deadline) {
        if (!r->lock.try_lock_shared_until(*deadline))
            return kErrorTimedOut;
    } else {
        r->lock.lock_shared();
    }
    return 0;
}

int RwlockTryLock(PthreadRwlock* rwlock, const bool exclusive) {
    auto* r = resolveRwlock(rwlock);
    const auto self = std::this_thread::get_id();
    if (r->writer.load(std::memory_order_acquire) == self)
        return kErrorBusy;
    if (exclusive) {
        if (!r->lock.try_lock())
            return kErrorBusy;
        r->writer.store(self, std::memory_order_release);
        return 0;
    }
    return r->lock.try_lock_shared() ? 0 : kErrorBusy;
}

int RwlockUnlock(PthreadRwlock* rwlock) {
    auto* r = resolveRwlock(rwlock);
    if (r->writer.load(std::memory_order_acquire) == std::this_thread::get_id()) {
        r->writer.store(std::thread::id{}, std::memory_order_release);
        r->lock.unlock();
        return 0;
    }
    r->lock.unlock_shared();
    return 0;
}

}
