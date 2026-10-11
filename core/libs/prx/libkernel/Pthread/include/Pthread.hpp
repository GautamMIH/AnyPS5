#ifndef CORE_LIBS_PRX_LIBKERNEL_PTHREAD_PTHREAD_HPP
#define CORE_LIBS_PRX_LIBKERNEL_PTHREAD_PTHREAD_HPP

#include <sched.h>
#include "SceTypes.hpp"
#include "prx/libkernel/Time/include/TimedWait.hpp"
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#ifndef _WIN32
#include <pthread.h>
#endif

enum class MutexType : std::uint32_t {
    ErrorCheck = 1,
    Recursive = 2,
    Normal = 3,
    Adaptive = 4,
};

struct PthreadMutexattrPrivate {
    MutexType type;
    int protocol = 0;
    int ceiling = 0;
};

struct PthreadMutexPrivate {
    std::recursive_timed_mutex _rmtx;
    std::timed_mutex _mtx;
    MutexType _type;
    std::atomic<std::thread::id> _owner;
    int _count;

    PthreadMutexPrivate() : _type(MutexType::ErrorCheck), _owner(std::thread::id{}), _count(0) {}
};

struct PthreadCondattrPrivate {
    int _clockid;
};

struct PthreadCondPrivate {
    std::condition_variable_any _cv;
    int _clockid = 0;
};

struct PthreadRwlockattrPrivate {
    int type = 0;
};

struct PthreadRwlockPrivate {
    // winpthreads initializes a static rwlock on first use and fails a concurrent first lock with
    // EINVAL, which shared_timed_mutex ignores; it is initialized here (upstream 69063dc1e).
    PthreadRwlockPrivate() {
        lock.lock();
        lock.unlock();
    }
    std::shared_timed_mutex lock;
    std::atomic<std::thread::id> writer;
    // Read locks held, so a held rwlock is not destroyed (upstream 718eb9cc3).
    std::atomic<int> readers{0};
};

inline constexpr KernelCpumask kDefaultThreadAffinity = 0x3fff;
inline constexpr int kDefaultThreadPriority = 700;

struct PthreadSemPrivate {
    std::mutex _mutex;
    TimedWait::Condition _cv;
    int _count = 0;

    explicit PthreadSemPrivate(unsigned int value) : _count(static_cast<int>(value)) {}
};

inline constexpr std::uint32_t CPU_CLOCK_BIT = 0x80000000u;
inline constexpr std::uint32_t CPU_CLOCK_PROCESS_BIT = 0x40000000u;
inline constexpr std::uint32_t CPU_CLOCK_ID_MASK = ~(CPU_CLOCK_BIT | CPU_CLOCK_PROCESS_BIT);

// The guest CPU a host CPU stands for: the host CPU's position among the thread's affinity CPUs.
inline int GuestCpuFromHost(unsigned hostCpu, KernelCpumask threadAffinity) {
    KernelCpumask mask = threadAffinity & kDefaultThreadAffinity;
    if (mask == 0) mask = kDefaultThreadAffinity;
    for (auto skip = hostCpu % static_cast<unsigned>(std::popcount(mask)); skip != 0; --skip) mask &= mask - 1;
    return std::countr_zero(mask);
}

struct PthreadAttrPrivate {
    void* stackAddress = nullptr;
    std::size_t _stacksize;
    int _detachstate;
    int _schedpriority;
    int _schedpolicy;
    int _inheritsched;
    KernelCpumask affinity = kDefaultThreadAffinity;
    std::size_t guardSize = 0x1000;
    int solosched = 0;
};

inline std::atomic<std::int64_t> nextThreadId{100000};

struct PthreadPrivate {
    std::int64_t tid = nextThreadId.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    void* nativeHandle = nullptr;
    std::thread::id threadId;
    std::atomic<unsigned> references{2};
#else
    pthread_t hostThread{};
    std::thread::id threadId;
    std::atomic<unsigned> references{2};
    // Host thread running this guest thread, valid while hostRunning is set (signal delivery).
    std::atomic<bool> hostRunning{false};
#endif
    void* stackAddress = nullptr;
    std::size_t stackSize = 0;
    // Alertable waits in progress (TimedWait), read by Windows exception delivery.
    std::atomic<int> waitCount{0};
    std::atomic<KernelCpumask> affinity{kDefaultThreadAffinity};
    std::atomic<int> priority{kDefaultThreadPriority};
    int policy = 1;
    int cpuClockThread = 0;
#ifndef _WIN32
    clockid_t hostCpuClock{};
    bool hostCpuClockBound = false;
#endif
    std::mutex nameLock;
    std::string name;
    std::atomic<bool> _finished;
    void* _retval;
    bool _detached;
    // A host thread adopted by scePthreadSelf; it owns its own PthreadPrivate.
    bool _adopted = false;
    std::mutex _join_mtx;
    TimedWait::Condition _join_cv;
    std::atomic<bool> cancelPending{false};
    std::mutex cancelLock;
    TimedWait::Condition* cancelWait = nullptr;
    // Our pthread condition variables (PthreadSync::CondWait) wait on a condition_variable_any.
    std::condition_variable_any* cancelWaitAny = nullptr;

    PthreadPrivate();
    ~PthreadPrivate();
    PthreadPrivate(const PthreadPrivate&) = delete;
    PthreadPrivate& operator=(const PthreadPrivate&) = delete;
};

bool GuestThreadStack(std::uintptr_t address, std::uintptr_t* start, std::uintptr_t* end);
int GuestThreadCpuClockId(const PthreadPrivate* thread);
bool GuestThreadCpuNanos(int cpuClockThread, std::uint64_t* nanos);

#endif
