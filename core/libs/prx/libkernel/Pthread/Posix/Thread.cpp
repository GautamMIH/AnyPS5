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
    routine();
    lock.lock();
    state.store(kOnceDone, std::memory_order_release);
    onceCondition().notify_all();
    return 0;
}

int* APS5_VABI __error_nid_postfix();
int APS5_VABI scePthreadGetaffinity(Pthread thread, KernelCpumask* mask);

int APS5_VABI cpuset_getaffinity_nid_postfix(int level, int which, std::int64_t id, std::size_t size, void* mask) {
    constexpr int LevelWhich = 3, WhichThread = 1;
    constexpr int GuestEfault = 14, GuestErange = 34;
    constexpr std::size_t MaximumSize = 256 / 8;
    if (size < sizeof(KernelCpumask) || size > MaximumSize) {
        *__error_nid_postfix() = GuestErange;
        return -1;
    }
    if (level != LevelWhich || which != WhichThread || id != -1) NotImplemented_nid_no_patch("cpuset_getaffinity other than the calling thread");
    if (!mask) {
        *__error_nid_postfix() = GuestEfault;
        return -1;
    }
    KernelCpumask affinity = 0;
    scePthreadGetaffinity(scePthreadSelf(), &affinity);
    std::memset(mask, 0, size);
    std::memcpy(mask, &affinity, sizeof(affinity));
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

int APS5_VABI pthread_getschedparam_nid_postfix(Pthread thread, int* policy, KernelSchedParam* param) {
    if (!thread || !policy || !param) return PthreadSync::kErrorInvalid;
    *policy = thread->policy;
    param->sched_priority = thread->priority;
    return 0;
}

int APS5_VABI pthread_setschedparam_nid_postfix(Pthread thread, int policy, const KernelSchedParam* param) {
    if (!thread || !param || policy < 1 || policy > 3) return PthreadSync::kErrorInvalid;
    thread->policy = policy;
    thread->priority = param->sched_priority;
    return 0;
}

int APS5_VABI scePthreadGetschedparam(Pthread thread, int* policy, KernelSchedParam* param) {
    return PthreadSync::SceError(pthread_getschedparam_nid_postfix(thread, policy, param));
}

int APS5_VABI scePthreadSetschedparam(Pthread thread, int policy, const KernelSchedParam* param) {
    return PthreadSync::SceError(pthread_setschedparam_nid_postfix(thread, policy, param));
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
