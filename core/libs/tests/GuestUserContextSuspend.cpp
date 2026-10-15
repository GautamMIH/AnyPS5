#include "SceTypes.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

// pthread_suspend_user_context_np and its companions (Mono's thread suspension for Unity's garbage
// collector): a suspended thread stops, its registers read back as a guest ucontext and changes to
// them take effect on resume; suspensions nest; a blocked thread can be suspended and still finishes
// its wait afterwards.

extern "C" {
int APS5_VABI pthread_suspend_user_context_np_nid_postfix(Pthread thread);
int APS5_VABI pthread_resume_user_context_np_nid_postfix(Pthread thread);
int APS5_VABI pthread_get_user_context_np_nid_postfix(Pthread thread, void* ucontext);
int APS5_VABI pthread_set_user_context_np_nid_postfix(Pthread thread, const void* ucontext);
int APS5_VABI pthread_suspend_np_nid_postfix(Pthread thread);
int APS5_VABI pthread_resume_np_nid_postfix(Pthread thread);
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
Pthread APS5_VABI scePthreadSelf();
int APS5_VABI sceKernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init, int max, void* opt);
int APS5_VABI sceKernelDeleteSema(KernelSema sem);
int APS5_VABI sceKernelSignalSema(KernelSema sem, int count);
int APS5_VABI sceKernelWaitSema(KernelSema sem, int need, KernelUseconds* time);
}

static constexpr int GuestEdeadlk = 11;
static constexpr int GuestEinval = 22;
static constexpr std::size_t UcontextBytes = 0x500;
static constexpr std::size_t R12Offset = 0x40 + 0x60;
static constexpr std::size_t R13Offset = 0x40 + 0x68;
static constexpr std::size_t RipOffset = 0x40 + 0xa0;
static constexpr std::size_t RspOffset = 0xf8;
static constexpr std::uint64_t Marker = 0x0123456789abcdefULL;
static constexpr std::uint64_t PatchedR12 = 0x5a5a1234abcd0001ULL;

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "user context suspend: %s\n", what);
    std::abort();
}

struct SpinState {
    std::atomic<std::uint32_t> started{0};
    std::uint32_t padding = 0;
    std::uint64_t r12 = 0;
    std::atomic<std::uint64_t> rounds{0};
};
static_assert(offsetof(SpinState, r12) == 8 && offsetof(SpinState, rounds) == 16);

// Holds Marker in r13 and counts rounds until r12 becomes non-zero, then stores r12.
extern "C" void APS5_VABI UserContextSpin(SpinState* state);
asm(".text\n"
    ".globl UserContextSpin\n"
    "UserContextSpin:\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    xorl %r12d, %r12d\n"
    "    movabsq $0x0123456789abcdef, %r13\n"
    "    movl $1, (%rdi)\n"
    "1:\n"
    "    pause\n"
    "    lock incq 16(%rdi)\n"
    "    testq %r12, %r12\n"
    "    jz 1b\n"
    "    movq %r12, 8(%rdi)\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    ret\n");

static void* APS5_VABI Spinning(void* arg) {
    UserContextSpin(static_cast<SpinState*>(arg));
    return nullptr;
}

struct Blocked {
    KernelSema sem = nullptr;
    std::atomic<bool> waiting{false};
    int result = -1;
};

static void* APS5_VABI Waiting(void* arg) {
    auto* blocked = static_cast<Blocked*>(arg);
    blocked->waiting.store(true);
    blocked->result = sceKernelWaitSema(blocked->sem, 1, nullptr);
    return nullptr;
}

static bool Stopped(const SpinState& spin) {
    const auto before = spin.rounds.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    return spin.rounds.load() == before;
}

static std::uint64_t Word(const unsigned char* ucontext, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, ucontext + offset, sizeof(value));
    return value;
}

int main() {
    Require(pthread_suspend_user_context_np_nid_postfix(scePthreadSelf()) == GuestEdeadlk, "a thread suspended itself");
    Require(pthread_suspend_user_context_np_nid_postfix(nullptr) == GuestEinval, "a null thread was suspended");

    SpinState spin;
    Pthread spinner = nullptr;
    Require(scePthreadCreate(&spinner, nullptr, Spinning, &spin, "spinning") == 0, "cannot create the spinning thread");
    while (spin.started.load() == 0) std::this_thread::yield();
    Require(pthread_resume_user_context_np_nid_postfix(spinner) == GuestEinval, "a running thread was resumed");

    Require(pthread_suspend_user_context_np_nid_postfix(spinner) == 0, "the spinning thread was not suspended");
    Require(Stopped(spin), "a suspended thread kept running");
    alignas(16) unsigned char ucontext[UcontextBytes]{};
    Require(pthread_get_user_context_np_nid_postfix(spinner, ucontext) == 0, "the suspended thread's context was not read");
    Require(Word(ucontext, R13Offset) == Marker, "the context does not hold the thread's r13");
    Require(Word(ucontext, RipOffset) != 0 && Word(ucontext, RspOffset) != 0, "the context has no rip or rsp");

    // Suspensions nest: the thread runs again only after the last resume.
    Require(pthread_suspend_np_nid_postfix(spinner) == 0, "a second suspension failed");
    Require(pthread_resume_np_nid_postfix(spinner) == 0, "the inner resume failed");
    Require(Stopped(spin), "the thread ran after the inner of two resumes");

    // A changed register takes effect on resume: r12 ends the spin and is stored.
    std::uint64_t patched = PatchedR12;
    std::memcpy(ucontext + R12Offset, &patched, sizeof(patched));
    Require(pthread_set_user_context_np_nid_postfix(spinner, ucontext) == 0, "the context was not set");
    Require(pthread_resume_user_context_np_nid_postfix(spinner) == 0, "the outer resume failed");
    Require(scePthreadJoin(spinner, nullptr) == 0, "cannot join the spinning thread");
    Require(spin.r12 == PatchedR12, "the thread did not resume with the changed r12");
    Require(pthread_get_user_context_np_nid_postfix(spinner, ucontext) == GuestEinval, "a resumed thread's context was readable");

    // A thread blocked in a wait is suspended and resumed, then completes its wait.
    Blocked blocked;
    Require(sceKernelCreateSema(&blocked.sem, "suspend", 0, 0, 1, nullptr) == 0, "cannot create the semaphore");
    Pthread waiter = nullptr;
    Require(scePthreadCreate(&waiter, nullptr, Waiting, &blocked, "waiting") == 0, "cannot create the waiting thread");
    while (!blocked.waiting.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Require(pthread_suspend_user_context_np_nid_postfix(waiter) == 0, "the waiting thread was not suspended");
    Require(pthread_resume_user_context_np_nid_postfix(waiter) == 0, "the waiting thread was not resumed");
    Require(sceKernelSignalSema(blocked.sem, 1) == 0, "cannot signal the semaphore");
    Require(scePthreadJoin(waiter, nullptr) == 0, "cannot join the waiting thread");
    Require(blocked.result == 0, "the wait did not complete after the suspension");
    Require(sceKernelDeleteSema(blocked.sem) == 0, "cannot delete the semaphore");

    std::puts("user context suspend tests passed");
    return 0;
}
