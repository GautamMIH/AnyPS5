#ifndef CORE_LIBS_PRX_LIBKERNEL_PTHREAD_PTHREAD_HPP
#define CORE_LIBS_PRX_LIBKERNEL_PTHREAD_PTHREAD_HPP

#include <sched.h>
#include "SceTypes.hpp"
#include "prx/libkernel/Time/include/TimedWait.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
};

struct PthreadMutexattrPrivate {
    MutexType type;
    int protocol = 0;
};

struct PthreadMutexPrivate {
    std::recursive_timed_mutex _rmtx;
    std::timed_mutex _mtx;
    MutexType _type;
    std::atomic<std::thread::id> _owner;
    int _count;

    PthreadMutexPrivate() : _type(MutexType::Normal), _owner(std::thread::id{}), _count(0) {}
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
    std::shared_timed_mutex lock;
    std::atomic<std::thread::id> writer;
};

inline constexpr KernelCpumask kDefaultThreadAffinity = 0x3fff;
inline constexpr int kDefaultThreadPriority = 700;

struct PthreadSemPrivate {
    std::mutex _mutex;
    TimedWait::Condition _cv;
    int _count = 0;

    explicit PthreadSemPrivate(unsigned int value) : _count(static_cast<int>(value)) {}
};

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

struct PthreadPrivate {
#ifdef _WIN32
    void* nativeHandle = nullptr;
    std::thread::id threadId;
    std::atomic<unsigned> references{2};
#else
    std::thread _thr;
    // Host thread running this guest thread, valid while hostRunning is set (signal delivery).
    pthread_t hostThread{};
    std::atomic<bool> hostRunning{false};
#endif
    void* stackAddress = nullptr;
    std::size_t stackSize = 0;
    KernelCpumask affinity = kDefaultThreadAffinity;
    int priority = kDefaultThreadPriority;
    int policy = 1;
    std::string name;
    std::atomic<bool> _finished;
    void* _retval;
    bool _detached;
    std::mutex _join_mtx;
    std::condition_variable _join_cv;

    PthreadPrivate() : _finished(false), _retval(nullptr), _detached(false) {}
};

#endif
