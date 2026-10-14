#include "prx/libkernel/Time/include/StallWatch.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "SceTypes.hpp"
#include "../include/ThreadLifecycle.hpp"
#include "../include/Cancel.hpp"
#include "prx/libc/include/General.hpp"
#include "../include/Pthread.hpp"
#include "Common.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include <chrono>
#include <mutex>
#include <thread>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "../include/PthreadSync.hpp"

namespace {

constexpr std::int32_t kOnceNeverDone = 0;
constexpr std::int32_t kOnceDone = 1;
constexpr std::int32_t kOnceInProgress = 2;

struct OnceControl {
    std::int32_t state;
    std::int32_t reserved;
    void* mutex;
};

using OnceRoutine = void (APS5_VABI *)();

std::mutex& onceMutex() {
    static std::mutex instance;
    return instance;
}

std::condition_variable& onceCondition() {
    static std::condition_variable instance;
    return instance;
}

int posixFromSce(const int result) {
    return result == 0 ? 0 : (result & 0xffff);
}

constexpr int GUEST_SCHED_FIFO = 1;
constexpr int GUEST_SCHED_RR = 3;
constexpr std::size_t GUEST_CPUSET_BYTES = 32;
constexpr int GUEST_ERANGE = 34;

bool AcceptsSchedParam(int policy, int priority) {
    if (policy != GUEST_SCHED_FIFO && policy != GUEST_SCHED_RR) return false;
    return (priority >= 256 && priority <= 767) || (priority >= 896 && priority <= 959);
}

// A once routine that is cancelled (its thread exits) or throws leaves the control not done, so a
// later caller runs it again instead of waiting forever (upstream).
void APS5_VABI resetOnce(void* control) {
    std::lock_guard lock(onceMutex());
    std::atomic_ref<std::int32_t>(static_cast<OnceControl*>(control)->state).store(kOnceNeverDone, std::memory_order_release);
    onceCondition().notify_all();
}

}

extern "C" {

int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
int APS5_VABI scePthreadDetach(Pthread thread);
void APS5_VABI scePthreadExit(void* retval);
Pthread APS5_VABI scePthreadSelf();
int APS5_VABI scePthreadRename(Pthread thread, const char* name);
int APS5_VABI scePthreadGetname(Pthread thread, char* name);
int APS5_VABI scePthreadCancel(Pthread thread);
int APS5_VABI scePthreadSetcancelstate(int state, int* old_state);
int APS5_VABI scePthreadSetcanceltype(int type, int* old_type);
void APS5_VABI scePthreadTestcancel();
int APS5_VABI scePthreadSetprio(Pthread thread, int prio);
int APS5_VABI scePthreadGetprio(Pthread thread, int* prio);

int APS5_VABI scePthreadOnce(OnceControl* once, OnceRoutine routine) {
    if (!once || !routine) return PthreadSync::SceError(PthreadSync::kErrorInvalid);
    std::atomic_ref<std::int32_t> state(once->state);
    if (state.load(std::memory_order_acquire) == kOnceDone) return 0;
    std::unique_lock lock(onceMutex());
    onceCondition().wait(lock, [&state] { return state.load(std::memory_order_acquire) != kOnceInProgress; });
    if (state.load(std::memory_order_acquire) == kOnceDone) return 0;
    state.store(kOnceInProgress, std::memory_order_release);
    lock.unlock();
    CleanupHandler reset{};
    void* const mark = ThreadCancel::CleanupMark();
    __pthread_cleanup_push_imp_nid_postfix(resetOnce, once, &reset);
    try {
        routine();
    } catch (...) {
        ThreadCancel::CleanupRestore(mark);
        resetOnce(once);
        throw;
    }
    ThreadCancel::CleanupRestore(mark);
    lock.lock();
    state.store(kOnceDone, std::memory_order_release);
    onceCondition().notify_all();
    return 0;
}

int* APS5_VABI __error_nid_postfix();
int APS5_VABI scePthreadGetaffinity(Pthread thread, KernelCpumask* mask);
int APS5_VABI scePthreadSetaffinity(Pthread thread, KernelCpumask mask);
KernelCpumask APS5_VABI sceKernelGetAvailableCpumask(void);
int APS5_VABI getpid_nid_postfix(void);

int APS5_VABI cpuset_getaffinity_nid_postfix(int level, int which, std::int64_t id, std::size_t size, void* mask) {
    constexpr int LevelWhich = 3, WhichThread = 1, WhichProcess = 2;
    constexpr int GuestEsrch = 3, GuestEfault = 14, GuestEinval = 22, GuestErange = 34;
    constexpr std::size_t MaximumSize = 256 / 8;
    const auto fail = [](int error) {
        *__error_nid_postfix() = error;
        return -1;
    };
    if (size < sizeof(KernelCpumask) || size > MaximumSize) return fail(GuestErange);
    if (!mask) return fail(GuestEfault);
    auto* self = scePthreadSelf();
    if (which == WhichThread) {
        if (id != -1 && id != static_cast<std::int64_t>(self->tid)) {
            if (LiveGuestThreadExists(id)) NotImplemented_nid_no_patch("cpuset_getaffinity other than the calling thread or process");
            return fail(GuestEsrch);
        }
    } else if (which == WhichProcess) {
        if (id != -1 && id != getpid_nid_postfix()) return fail(GuestEsrch);
    } else if (which >= 3 && which <= 6) {
        NotImplemented_nid_no_patch("cpuset_getaffinity other than the calling thread or process");
    } else {
        return fail(GuestEinval);
    }
    if (level < 1 || level > 3) return fail(GuestEinval);
    if (level != LevelWhich) NotImplemented_nid_no_patch("cpuset_getaffinity other than the calling thread or process");
    KernelCpumask affinity = 0;
    if (which == WhichThread) scePthreadGetaffinity(self, &affinity);
    else affinity = LiveGuestThreadsAffinity();
    std::memset(mask, 0, size);
    std::memcpy(mask, &affinity, sizeof(affinity));
    return 0;
}

int APS5_VABI cpuset_setaffinity_nid_postfix(int level, int which, std::int64_t id, std::size_t size, const void* mask) {
    constexpr int LevelWhich = 3, WhichThread = 1, WhichProcess = 2;
    constexpr int GuestEsrch = 3, GuestEdeadlk = 11, GuestEfault = 14, GuestEinval = 22, GuestErange = 34;
    constexpr std::size_t MaximumSize = 256 / 8;
    const auto fail = [](int error) {
        *__error_nid_postfix() = error;
        return -1;
    };
    if (size < sizeof(KernelCpumask) || size > MaximumSize) return fail(GuestErange);
    if (!mask) return fail(GuestEfault);
    const auto* bytes = static_cast<const std::uint8_t*>(mask);
    for (std::size_t index = sizeof(KernelCpumask); index < size; ++index) {
        if (bytes[index] != 0) return fail(GuestEinval);
    }
    auto* self = scePthreadSelf();
    if (which == WhichThread) {
        if (id != -1 && id != static_cast<std::int64_t>(self->tid)) {
            if (LiveGuestThreadExists(id)) NotImplemented_nid_no_patch("cpuset_setaffinity other than the calling thread or process");
            return fail(GuestEsrch);
        }
    } else if (which == WhichProcess) {
        if (id != -1 && id != getpid_nid_postfix()) return fail(GuestEsrch);
    } else if (which >= 3 && which <= 6) {
        NotImplemented_nid_no_patch("cpuset_setaffinity other than the calling thread or process");
    } else {
        return fail(GuestEinval);
    }
    if (level < 1 || level > 3) return fail(GuestEinval);
    if (level != LevelWhich) NotImplemented_nid_no_patch("cpuset_setaffinity other than the calling thread or process");
    KernelCpumask requested = 0;
    std::memcpy(&requested, bytes, sizeof(requested));
    const KernelCpumask affinity = requested & sceKernelGetAvailableCpumask();
    if (affinity == 0) return fail(GuestEdeadlk);
    if (which == WhichThread) scePthreadSetaffinity(self, affinity);
    else SetLiveGuestThreadsAffinity(affinity);
    return 0;
}

int APS5_VABI pthread_create_nid_postfix(Pthread* thread, const PthreadAttr* attr, pthread_entry_func_t entry, void* arg) {
    return posixFromSce(scePthreadCreate(thread, attr, reinterpret_cast<PthreadEntry>(entry), arg, nullptr));
}

int APS5_VABI pthread_create_name_np_nid_postfix(Pthread* thread, const PthreadAttr* attr, pthread_entry_func_t entry, void* arg, const char* name) {
    return posixFromSce(scePthreadCreate(thread, attr, reinterpret_cast<PthreadEntry>(entry), arg, name));
}

int APS5_VABI pthread_detach_nid_postfix(Pthread thread) {
    return posixFromSce(scePthreadDetach(thread));
}

void APS5_VABI pthread_exit_nid_postfix(void* value) {
    scePthreadExit(value);
}

int APS5_VABI pthread_join_nid_postfix(Pthread thread, void** value) {
    APS5_STALL_WATCH("join", reinterpret_cast<const void*>(thread));
    return posixFromSce(scePthreadJoin(thread, value));
}

int APS5_VABI pthread_rename_np_nid_postfix(Pthread thread, const char* name) {
    if (thread && name && std::strlen(name) >= 32) return posixFromSce(SCE_KERNEL_ERROR_ENAMETOOLONG);
    return posixFromSce(scePthreadRename(thread, name));
}

int APS5_VABI pthread_getname_np_nid_postfix(Pthread thread, char* name) {
    if (!thread) return PosixThread::GUEST_ESRCH;
    if (!name) return PosixThread::GUEST_EFAULT;
    return PosixThread::ToErrno(scePthreadGetname(thread, name));
}

Pthread APS5_VABI pthread_self_nid_postfix(void) {
    return scePthreadSelf();
}

int APS5_VABI pthread_equal_nid_postfix(Pthread thread1, Pthread thread2) {
    return thread1 == thread2 ? 1 : 0;
}

int APS5_VABI pthread_setcancelstate_nid_postfix(int state, int* old_state) {
    return posixFromSce(scePthreadSetcancelstate(state, old_state));
}

int APS5_VABI pthread_setprio_nid_postfix(Pthread thread, int prio) {
    return posixFromSce(scePthreadSetprio(thread, prio));
}

int APS5_VABI scePthreadGetschedparam(Pthread thread, int* policy, KernelSchedParam* param) {
    if (!thread || !policy || !param) return SCE_KERNEL_ERROR_EINVAL;
    const int result = scePthreadGetprio(thread, &param->sched_priority);
    if (result == 0) *policy = thread->policy.load(std::memory_order_relaxed);
    return result;
}

// The console accepts SCHED_FIFO and SCHED_RR with a priority in the user or the high range.
int APS5_VABI scePthreadSetschedparam(Pthread thread, int policy, const KernelSchedParam* param) {
    if (!thread || !param || !AcceptsSchedParam(policy, param->sched_priority)) return SCE_KERNEL_ERROR_EINVAL;
    const int result = scePthreadSetprio(thread, param->sched_priority);
    if (result == 0) thread->policy.store(policy, std::memory_order_relaxed);
    return result;
}

int APS5_VABI pthread_getschedparam_nid_postfix(Pthread thread, int* policy, KernelSchedParam* param) {
    return posixFromSce(scePthreadGetschedparam(thread, policy, param));
}

int APS5_VABI pthread_setschedparam_nid_postfix(Pthread thread, int policy, const KernelSchedParam* param) {
    return posixFromSce(scePthreadSetschedparam(thread, policy, param));
}

int APS5_VABI pthread_getaffinity_np_nid_postfix(Pthread thread, std::size_t cpusetSize, void* cpuset) {
    if (!thread) return PosixThread::GUEST_EINVAL;
    if (cpusetSize != GUEST_CPUSET_BYTES) return GUEST_ERANGE;
    if (!cpuset) return PosixThread::GUEST_EFAULT;
    KernelCpumask mask = 0;
    const int result = scePthreadGetaffinity(thread, &mask);
    if (result != 0) return PosixThread::ToErrno(result);
    std::memset(cpuset, 0, GUEST_CPUSET_BYTES);
    std::memcpy(cpuset, &mask, sizeof(mask));
    return 0;
}

int APS5_VABI pthread_setaffinity_np_nid_postfix(Pthread thread, std::size_t cpusetSize, const void* cpuset) {
    constexpr int GuestEdeadlk = 11;
    if (thread != scePthreadSelf()) {
        if (!thread) return PosixThread::GUEST_EINVAL;
        if (!GuestThreadIsLive(thread)) return PosixThread::GUEST_ESRCH;
    }
    if (cpusetSize != GUEST_CPUSET_BYTES) return GUEST_ERANGE;
    if (!cpuset) return PosixThread::GUEST_EFAULT;
    KernelCpumask requested = 0;
    std::memcpy(&requested, cpuset, sizeof(requested));
    const KernelCpumask affinity = requested & sceKernelGetAvailableCpumask();
    if (affinity == 0) return GuestEdeadlk;
    return PosixThread::ToErrno(scePthreadSetaffinity(thread, affinity));
}

void APS5_VABI pthread_set_name_np_nid_postfix(Pthread thread, const char* name) {
    scePthreadRename(thread, name != nullptr ? name : "");
}

int APS5_VABI scePthreadGetthreadid(void);

int APS5_VABI pthread_getthreadid_np_nid_postfix(void) {
    return scePthreadGetthreadid();
}

void APS5_VABI pthread_yield_nid_postfix(void) {
    std::this_thread::yield();
}

int APS5_VABI pthread_getcpuclockid_nid_postfix(Pthread thread, int* clockId) {
    constexpr int guestFault = 14;
    if (!thread) return PosixThread::GUEST_EINVAL;
    if (!clockId) return guestFault;
    *clockId = GuestThreadCpuClockId(thread);
    return 0;
}

int APS5_VABI sched_yield_nid_postfix(void) {
    std::this_thread::yield();
    return 0;
}

int APS5_VABI pthread_cancel_nid_postfix(Pthread thread) {
    return posixFromSce(scePthreadCancel(thread));
}

int APS5_VABI pthread_setcanceltype_nid_postfix(int type, int* old_type) {
    return posixFromSce(scePthreadSetcanceltype(type, old_type));
}
void APS5_VABI pthread_testcancel_nid_postfix(void) {
    scePthreadTestcancel();
}

unsigned int APS5_VABI sleep_nid_postfix(unsigned int seconds) {
    ThreadCancel::Check();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    ThreadCancel::Check();
    return 0;
}

}
