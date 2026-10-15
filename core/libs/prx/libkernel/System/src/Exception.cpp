#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include <array>
#include <chrono>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <system_error>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#endif

extern "C" Pthread APS5_VABI scePthreadSelf();
#ifdef _WIN32
extern "C" void Aps5RedirectedEntryStub();
extern "C" [[noreturn]] void Aps5RedirectedExit(CONTEXT* context, const void* vectors, void (*restore)(CONTEXT*, EXCEPTION_RECORD*));
#endif

namespace {

struct GuestMcontext {
    std::uint64_t onstack;
    std::uint64_t rdi;
    std::uint64_t rsi;
    std::uint64_t rdx;
    std::uint64_t rcx;
    std::uint64_t r8;
    std::uint64_t r9;
    std::uint64_t rax;
    std::uint64_t rbx;
    std::uint64_t rbp;
    std::uint64_t r10;
    std::uint64_t r11;
    std::uint64_t r12;
    std::uint64_t r13;
    std::uint64_t r14;
    std::uint64_t r15;
    std::uint32_t trapno;
    std::uint16_t fs;
    std::uint16_t gs;
    std::uint64_t addr;
    std::uint32_t flags;
    std::uint16_t es;
    std::uint16_t ds;
    std::uint64_t err;
    std::uint64_t rip;
    std::uint64_t cs;
    std::uint64_t rflags;
    std::uint64_t rsp;
    std::uint64_t ss;
    std::uint64_t len;
    std::uint64_t fpformat;
    std::uint64_t ownedfp;
    std::uint64_t lbrfrom;
    std::uint64_t lbrto;
    std::uint64_t aux1;
    std::uint64_t aux2;
    std::uint64_t fpstate[104];
    std::uint64_t fsbase;
    std::uint64_t gsbase;
    std::uint64_t spare[6];
};

struct alignas(16) GuestUcontext {
    std::uint32_t sigmask[4];
    std::int32_t reserved[12];
    GuestMcontext mcontext;
    GuestUcontext* link;
    void* stackPointer;
    std::uint64_t stackSize;
    std::int32_t stackFlags;
    std::int32_t stackAlign;
    std::int32_t flags;
    std::int32_t spare[4];
    std::int32_t tail[3];
};

static_assert(offsetof(GuestUcontext, mcontext) == 0x40);
static_assert(offsetof(GuestUcontext, mcontext) + offsetof(GuestMcontext, rsp) == 0xf8);

using GuestExceptionHandler = void (APS5_VABI *)(int, void*);

constexpr std::array<int, 6> AllowedSignals{1, 4, 8, 10, 11, 30};
constexpr int RaisedSignal = 30;

std::array<std::atomic<void*>, 32> handlers{};
static_assert(std::atomic<void*>::is_always_lock_free);

bool Allowed(int signum) {
    for (const int allowed : AllowedSignals)
        if (allowed == signum) return true;
    return false;
}

GuestExceptionHandler Handler(int signum) {
    return reinterpret_cast<GuestExceptionHandler>(handlers[signum].load(std::memory_order_acquire));
}

constexpr std::size_t MxcsrOffset = 24;
constexpr std::size_t MxcsrMaskOffset = 28;
constexpr std::uint32_t DefaultMxcsrMask = 0xffbf;

std::uint32_t FpWord(const GuestMcontext& m, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, reinterpret_cast<const unsigned char*>(m.fpstate) + offset, sizeof(value));
    return value;
}

void SetFpWord(GuestMcontext& m, std::size_t offset, std::uint32_t value) {
    std::memcpy(reinterpret_cast<unsigned char*>(m.fpstate) + offset, &value, sizeof(value));
}

void CallHandler(GuestExceptionHandler handler, int signum, GuestUcontext& ucontext) {
    auto& m = ucontext.mcontext;
    const std::uint32_t mxcsrMask = FpWord(m, MxcsrMaskOffset);
    handler(signum, &ucontext);
    SetFpWord(m, MxcsrOffset, FpWord(m, MxcsrOffset) & (mxcsrMask != 0 ? mxcsrMask : DefaultMxcsrMask));
    SetFpWord(m, MxcsrMaskOffset, mxcsrMask);
}

#ifdef _WIN32
constexpr std::size_t RedZone = 128;
constexpr std::size_t HomeArea = 32;
constexpr auto RetryLimit = std::chrono::seconds(1);
constexpr std::size_t VectorBytes = 16 * 32;

struct Delivery {
    GuestExceptionHandler handler;
    int signum;
    CONTEXT context;
    std::uint8_t vectors[VectorBytes];
    std::uint64_t saveVectors;
};

const bool SaveVectors = [] {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx") != 0;
}();

void Deliver(GuestExceptionHandler handler, int signum, CONTEXT& context) {
    GuestUcontext ucontext{};
    auto& m = ucontext.mcontext;
    m.rdi = context.Rdi;
    m.rsi = context.Rsi;
    m.rdx = context.Rdx;
    m.rcx = context.Rcx;
    m.r8 = context.R8;
    m.r9 = context.R9;
    m.rax = context.Rax;
    m.rbx = context.Rbx;
    m.rbp = context.Rbp;
    m.r10 = context.R10;
    m.r11 = context.R11;
    m.r12 = context.R12;
    m.r13 = context.R13;
    m.r14 = context.R14;
    m.r15 = context.R15;
    m.rip = context.Rip;
    m.rsp = context.Rsp;
    m.rflags = context.EFlags;
    m.cs = context.SegCs;
    m.ss = context.SegSs;
    m.len = sizeof(GuestMcontext);
    static_assert(sizeof(context.FltSave) <= sizeof(m.fpstate));
    std::memcpy(m.fpstate, &context.FltSave, sizeof(context.FltSave));
    CallHandler(handler, signum, ucontext);
    context.Rdi = m.rdi;
    context.Rsi = m.rsi;
    context.Rdx = m.rdx;
    context.Rcx = m.rcx;
    context.R8 = m.r8;
    context.R9 = m.r9;
    context.Rax = m.rax;
    context.Rbx = m.rbx;
    context.Rbp = m.rbp;
    context.R10 = m.r10;
    context.R11 = m.r11;
    context.R12 = m.r12;
    context.R13 = m.r13;
    context.R14 = m.r14;
    context.R15 = m.r15;
    context.Rip = m.rip;
    context.Rsp = m.rsp;
    context.EFlags = static_cast<DWORD>(m.rflags);
    std::memcpy(&context.FltSave, m.fpstate, sizeof(context.FltSave));
    context.MxCsr = context.FltSave.MxCsr;
}

[[noreturn]] void RedirectedEntry(Delivery* delivery) {
    CONTEXT context = delivery->context;
    Deliver(delivery->handler, delivery->signum, context);
    Aps5RedirectedExit(&context, delivery->saveVectors != 0 ? delivery->vectors : nullptr, RtlRestoreContext);
}

bool StackWritable(DWORD64 low, DWORD64 high) {
    for (DWORD64 address = low; address < high;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) != sizeof(info)) return false;
        if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0 || (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) == 0) return false;
        address = reinterpret_cast<DWORD64>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

void CALLBACK WaitingEntry(ULONG_PTR parameter) {
    auto* delivery = reinterpret_cast<Delivery*>(parameter);
    const auto handler = delivery->handler;
    const int signum = delivery->signum;
    delete delivery;
    CONTEXT context{};
    RtlCaptureContext(&context);
    Deliver(handler, signum, context);
}

static_assert(HomeArea + 8 == 40, "Aps5RedirectedEntryStub finds the delivery 40 bytes above its stack pointer");
static_assert(offsetof(Delivery, context) == 16 && offsetof(CONTEXT, Rax) == 0x78 && offsetof(CONTEXT, Rbp) == 0xa0 && offsetof(CONTEXT, R15) == 0xf0, "Aps5RedirectedEntryStub stores the live registers into the delivery's context");
static_assert(offsetof(Delivery, vectors) == 1248 && offsetof(Delivery, saveVectors) == 1760, "Aps5RedirectedEntryStub stores ymm0-ymm15 into the delivery when saveVectors is set");

bool Exited(HANDLE native) {
    return WaitForSingleObject(native, 0) == WAIT_OBJECT_0;
}

bool RestoringContext(DWORD64 rip) {
    static const std::array<DWORD64, 2> stubs = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return std::array<DWORD64, 2>{reinterpret_cast<DWORD64>(GetProcAddress(ntdll, "NtContinue")), reinterpret_cast<DWORD64>(GetProcAddress(ntdll, "NtContinueEx"))};
    }();
    for (const DWORD64 stub : stubs)
        if (stub != 0 && rip - stub < 0x20) return true;
    return false;
}

bool InWinpthread(DWORD64 rip) {
    static const HMODULE winpthread = GetModuleHandleW(L"libwinpthread-1.dll");
    MEMORY_BASIC_INFORMATION info{};
    return winpthread != nullptr && VirtualQuery(reinterpret_cast<const void*>(rip), &info, sizeof(info)) == sizeof(info) && info.AllocationBase == winpthread;
}

bool RaiseOn(Pthread thread, GuestExceptionHandler handler, int signum) {
    if (thread == scePthreadSelf()) {
        CONTEXT context{};
        RtlCaptureContext(&context);
        Deliver(handler, signum, context);
        return true;
    }
    const auto native = static_cast<HANDLE>(thread->nativeHandle);
    auto queued = std::make_unique<Delivery>(Delivery{handler, signum, {}, {}, 0});
    alignas(16) Delivery delivery{handler, signum, {}, {}, SaveVectors ? 1u : 0u};
    const auto retryDeadline = std::chrono::steady_clock::now() + RetryLimit;
    for (;;) {
        if (SuspendThread(native) == static_cast<DWORD>(-1)) {
            if (Exited(native)) return false;
            throw std::runtime_error("sceKernelRaiseException: cannot suspend the target thread");
        }
        if (Exited(native)) {
            ResumeThread(native);
            return false;
        }
        delivery.context.ContextFlags = CONTEXT_FULL | CONTEXT_SEGMENTS;
        if (!GetThreadContext(native, &delivery.context)) {
            ResumeThread(native);
            throw std::runtime_error("sceKernelRaiseException: cannot read the target thread context");
        }
        if (thread->waitCount.load(std::memory_order_seq_cst) > 0) {
            const bool accepted = QueueUserAPC(WaitingEntry, native, reinterpret_cast<ULONG_PTR>(queued.get())) != 0;
            ResumeThread(native);
            if (!accepted) throw std::runtime_error("sceKernelRaiseException: cannot queue delivery to the waiting thread");
            queued.release();
            return true;
        }
        if (!RestoringContext(delivery.context.Rip) && !InWinpthread(delivery.context.Rip)) break;
        ResumeThread(native);
        if (std::chrono::steady_clock::now() >= retryDeadline) throw std::runtime_error("sceKernelRaiseException: the target thread stayed inside NtContinue or winpthreads for 1 s");
        SwitchToThread();
    }
    const DWORD64 slot = (delivery.context.Rsp - RedZone - sizeof(Delivery)) & ~static_cast<DWORD64>(15);
    if (!StackWritable(slot - HomeArea - 8, delivery.context.Rsp - RedZone)) {
        ResumeThread(native);
        throw std::runtime_error("sceKernelRaiseException: the target thread stack below its red zone is not committed");
    }
    std::memcpy(reinterpret_cast<void*>(slot), &delivery, sizeof(Delivery));
    CONTEXT redirected = delivery.context;
    redirected.Rsp = slot - HomeArea - 8;
    redirected.Rip = reinterpret_cast<DWORD64>(&Aps5RedirectedEntryStub);
    if (!SetThreadContext(native, &redirected)) {
        ResumeThread(native);
        throw std::runtime_error("sceKernelRaiseException: cannot redirect the target thread");
    }
    ResumeThread(native);
    return true;
}
#else
constexpr int HostSignal = SIGUSR1;
constexpr std::size_t FxsaveRegisterBytes = 416;

void FromHost(const mcontext_t& host, GuestMcontext& m) {
    const auto* g = host.gregs;
    m.rdi = g[REG_RDI];
    m.rsi = g[REG_RSI];
    m.rdx = g[REG_RDX];
    m.rcx = g[REG_RCX];
    m.r8 = g[REG_R8];
    m.r9 = g[REG_R9];
    m.rax = g[REG_RAX];
    m.rbx = g[REG_RBX];
    m.rbp = g[REG_RBP];
    m.r10 = g[REG_R10];
    m.r11 = g[REG_R11];
    m.r12 = g[REG_R12];
    m.r13 = g[REG_R13];
    m.r14 = g[REG_R14];
    m.r15 = g[REG_R15];
    m.rip = g[REG_RIP];
    m.rsp = g[REG_RSP];
    m.rflags = g[REG_EFL];
    m.cs = static_cast<std::uint64_t>(g[REG_CSGSFS]) & 0xffffu;
    m.ss = static_cast<std::uint64_t>(g[REG_CSGSFS]) >> 48;
    m.len = sizeof(GuestMcontext);
    static_assert(sizeof(*host.fpregs) <= sizeof(m.fpstate) && FxsaveRegisterBytes <= sizeof(*host.fpregs));
    if (host.fpregs != nullptr) std::memcpy(m.fpstate, host.fpregs, sizeof(*host.fpregs));
}

void ToHost(const GuestMcontext& m, mcontext_t& host) {
    auto* g = host.gregs;
    g[REG_RDI] = static_cast<greg_t>(m.rdi);
    g[REG_RSI] = static_cast<greg_t>(m.rsi);
    g[REG_RDX] = static_cast<greg_t>(m.rdx);
    g[REG_RCX] = static_cast<greg_t>(m.rcx);
    g[REG_R8] = static_cast<greg_t>(m.r8);
    g[REG_R9] = static_cast<greg_t>(m.r9);
    g[REG_RAX] = static_cast<greg_t>(m.rax);
    g[REG_RBX] = static_cast<greg_t>(m.rbx);
    g[REG_RBP] = static_cast<greg_t>(m.rbp);
    g[REG_R10] = static_cast<greg_t>(m.r10);
    g[REG_R11] = static_cast<greg_t>(m.r11);
    g[REG_R12] = static_cast<greg_t>(m.r12);
    g[REG_R13] = static_cast<greg_t>(m.r13);
    g[REG_R14] = static_cast<greg_t>(m.r14);
    g[REG_R15] = static_cast<greg_t>(m.r15);
    g[REG_RIP] = static_cast<greg_t>(m.rip);
    g[REG_RSP] = static_cast<greg_t>(m.rsp);
    g[REG_EFL] = static_cast<greg_t>(m.rflags);
    if (host.fpregs != nullptr) std::memcpy(host.fpregs, m.fpstate, FxsaveRegisterBytes);
}

void Deliver(int, siginfo_t*, void* context) {
    const auto handler = Handler(RaisedSignal);
    if (handler == nullptr) {
        static constexpr char message[] = "sceKernelRaiseException: the handler was removed before the signal was delivered\n";
        const auto written = write(STDERR_FILENO, message, sizeof(message) - 1);
        (void)written;
        std::abort();
    }
    auto& host = static_cast<ucontext_t*>(context)->uc_mcontext;
    GuestUcontext ucontext{};
    FromHost(host, ucontext.mcontext);
    CallHandler(handler, RaisedSignal, ucontext);
    ToHost(ucontext.mcontext, host);
}

void InstallDelivery() {
    static const bool installed = [] {
        struct sigaction action{};
        action.sa_sigaction = Deliver;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        if (sigaction(HostSignal, &action, nullptr) != 0)
            throw std::system_error(errno, std::generic_category(), "sceKernelRaiseException: cannot install the host signal handler");
        return true;
    }();
    (void)installed;
}

bool Send(Pthread thread) {
    const int result = pthread_kill(thread->hostThread, HostSignal);
    if (result == ESRCH) return false;
    if (result != 0) throw std::system_error(result, std::generic_category(), "sceKernelRaiseException: cannot signal the target thread");
    return true;
}

class HostSignalBlock {
public:
    HostSignalBlock() {
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, HostSignal);
        pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    }
    ~HostSignalBlock() { pthread_sigmask(SIG_SETMASK, &previous, nullptr); }
    HostSignalBlock(const HostSignalBlock&) = delete;
    HostSignalBlock& operator=(const HostSignalBlock&) = delete;

private:
    sigset_t previous;
};

bool RaiseOn(Pthread thread) {
    InstallDelivery();
    if (thread == scePthreadSelf()) return Send(thread);
    HostSignalBlock block;
    std::lock_guard lock(thread->_join_mtx);
    if (thread->_finished.load(std::memory_order_acquire)) return false;
    return Send(thread);
}
#endif

}

#ifdef _WIN32
extern "C" [[noreturn]] void Aps5RedirectedEntry(void* delivery) {
    RedirectedEntry(static_cast<Delivery*>(delivery));
}
asm(".text\n"
    ".globl Aps5RedirectedEntryStub\n"
    "Aps5RedirectedEntryStub:\n"
    "    movq %rax, 176(%rsp)\n"
    "    movq %rcx, 184(%rsp)\n"
    "    movq %rdx, 192(%rsp)\n"
    "    movq %rbx, 200(%rsp)\n"
    "    movq %rbp, 216(%rsp)\n"
    "    movq %rsi, 224(%rsp)\n"
    "    movq %rdi, 232(%rsp)\n"
    "    movq %r8, 240(%rsp)\n"
    "    movq %r9, 248(%rsp)\n"
    "    movq %r10, 256(%rsp)\n"
    "    movq %r11, 264(%rsp)\n"
    "    movq %r12, 272(%rsp)\n"
    "    movq %r13, 280(%rsp)\n"
    "    movq %r14, 288(%rsp)\n"
    "    movq %r15, 296(%rsp)\n"
    "    cmpq $0, 1800(%rsp)\n"
    "    je 1f\n"
    "    vmovdqu %ymm0, 1288(%rsp)\n"
    "    vmovdqu %ymm1, 1320(%rsp)\n"
    "    vmovdqu %ymm2, 1352(%rsp)\n"
    "    vmovdqu %ymm3, 1384(%rsp)\n"
    "    vmovdqu %ymm4, 1416(%rsp)\n"
    "    vmovdqu %ymm5, 1448(%rsp)\n"
    "    vmovdqu %ymm6, 1480(%rsp)\n"
    "    vmovdqu %ymm7, 1512(%rsp)\n"
    "    vmovdqu %ymm8, 1544(%rsp)\n"
    "    vmovdqu %ymm9, 1576(%rsp)\n"
    "    vmovdqu %ymm10, 1608(%rsp)\n"
    "    vmovdqu %ymm11, 1640(%rsp)\n"
    "    vmovdqu %ymm12, 1672(%rsp)\n"
    "    vmovdqu %ymm13, 1704(%rsp)\n"
    "    vmovdqu %ymm14, 1736(%rsp)\n"
    "    vmovdqu %ymm15, 1768(%rsp)\n"
    "1:\n"
    "    leaq 40(%rsp), %rcx\n"
    "    jmp Aps5RedirectedEntry\n"
    ".globl Aps5RedirectedExit\n"
    "Aps5RedirectedExit:\n"
    "    testq %rdx, %rdx\n"
    "    je 1f\n"
    "    vmovdqu 0(%rdx), %ymm0\n"
    "    vmovdqu 32(%rdx), %ymm1\n"
    "    vmovdqu 64(%rdx), %ymm2\n"
    "    vmovdqu 96(%rdx), %ymm3\n"
    "    vmovdqu 128(%rdx), %ymm4\n"
    "    vmovdqu 160(%rdx), %ymm5\n"
    "    vmovdqu 192(%rdx), %ymm6\n"
    "    vmovdqu 224(%rdx), %ymm7\n"
    "    vmovdqu 256(%rdx), %ymm8\n"
    "    vmovdqu 288(%rdx), %ymm9\n"
    "    vmovdqu 320(%rdx), %ymm10\n"
    "    vmovdqu 352(%rdx), %ymm11\n"
    "    vmovdqu 384(%rdx), %ymm12\n"
    "    vmovdqu 416(%rdx), %ymm13\n"
    "    vmovdqu 448(%rdx), %ymm14\n"
    "    vmovdqu 480(%rdx), %ymm15\n"
    "1:\n"
    "    xorl %edx, %edx\n"
    "    jmp *%r8\n");
#endif

// pthread_suspend_user_context_np and its companions: Mono's (Unity's) way of stopping a thread for its
// garbage collector, the set shadPS4 provides in core/libraries/kernel/threads/pthread.cpp. Suspending
// another thread returns once it is stopped; its registers are then readable and writable as a guest
// ucontext until the matching resume. Suspensions nest; a thread cannot suspend itself (EDEADLK, as
// libthr's pthread_suspend_np).
namespace {

constexpr int GuestEsrch = 3;
constexpr int GuestEdeadlk = 11;
constexpr int GuestEinval = 22;

struct UserSuspend {
    std::mutex lock;  // serialises suspend and resume on the thread
    int count = 0;    // under lock
    GuestUcontext context{};
    bool contextChanged = false;  // set by set_user_context, applied on resume
#ifdef _WIN32
    CONTEXT native{};
#else
    std::atomic<std::uint32_t> parked{0};   // 1 while the thread waits in its signal handler
    std::atomic<std::uint32_t> release{0};  // bumped by resume; the parked handler waits for a change
#endif
};

UserSuspend& SuspendState(Pthread thread) {
    if (auto* state = static_cast<UserSuspend*>(thread->userSuspend.load(std::memory_order_acquire))) return *state;
    auto* created = new UserSuspend;
    void* expected = nullptr;
    if (thread->userSuspend.compare_exchange_strong(expected, created, std::memory_order_acq_rel)) return *created;
    delete created;
    return *static_cast<UserSuspend*>(expected);
}

bool Alive(Pthread thread) {
    return thread != nullptr && !thread->_finished.load(std::memory_order_acquire);
}

#ifdef _WIN32
void FromNative(const CONTEXT& context, GuestMcontext& m) {
    m.rdi = context.Rdi;
    m.rsi = context.Rsi;
    m.rdx = context.Rdx;
    m.rcx = context.Rcx;
    m.r8 = context.R8;
    m.r9 = context.R9;
    m.rax = context.Rax;
    m.rbx = context.Rbx;
    m.rbp = context.Rbp;
    m.r10 = context.R10;
    m.r11 = context.R11;
    m.r12 = context.R12;
    m.r13 = context.R13;
    m.r14 = context.R14;
    m.r15 = context.R15;
    m.rip = context.Rip;
    m.rsp = context.Rsp;
    m.rflags = context.EFlags;
    m.cs = context.SegCs;
    m.ss = context.SegSs;
    m.len = sizeof(GuestMcontext);
    std::memcpy(m.fpstate, &context.FltSave, sizeof(context.FltSave));
}

void ToNative(const GuestMcontext& m, CONTEXT& context) {
    context.Rdi = m.rdi;
    context.Rsi = m.rsi;
    context.Rdx = m.rdx;
    context.Rcx = m.rcx;
    context.R8 = m.r8;
    context.R9 = m.r9;
    context.Rax = m.rax;
    context.Rbx = m.rbx;
    context.Rbp = m.rbp;
    context.R10 = m.r10;
    context.R11 = m.r11;
    context.R12 = m.r12;
    context.R13 = m.r13;
    context.R14 = m.r14;
    context.R15 = m.r15;
    context.Rip = m.rip;
    context.Rsp = m.rsp;
    context.EFlags = static_cast<DWORD>(m.rflags);
    std::memcpy(&context.FltSave, m.fpstate, sizeof(context.FltSave));
    context.MxCsr = context.FltSave.MxCsr;
}

int Stop(Pthread thread, UserSuspend& state) {
    auto* native = static_cast<HANDLE>(thread->nativeHandle);
    if (native == nullptr || SuspendThread(native) == static_cast<DWORD>(-1)) return GuestEsrch;
    state.native = {};
    state.native.ContextFlags = CONTEXT_FULL | CONTEXT_FLOATING_POINT;
    if (!GetThreadContext(native, &state.native)) {
        ResumeThread(native);
        return GuestEsrch;
    }
    state.context = {};
    FromNative(state.native, state.context.mcontext);
    return 0;
}

void Start(Pthread thread, UserSuspend& state) {
    auto* native = static_cast<HANDLE>(thread->nativeHandle);
    if (state.contextChanged) {
        ToNative(state.context.mcontext, state.native);
        SetThreadContext(native, &state.native);
    }
    ResumeThread(native);
}
#else
constexpr int SuspendSignal = SIGUSR2;

long Futex(std::atomic<std::uint32_t>& word, int operation, std::uint32_t value, const timespec* timeout = nullptr) {
    return syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), operation, value, timeout, nullptr, 0);
}

// Runs on the suspended thread: saves its registers, reports it stopped, and waits for the resume.
// Only async-signal-safe work: atomics and futex calls.
void Park(int, siginfo_t*, void* context) {
    const int savedErrno = errno;
    auto* state = static_cast<UserSuspend*>(scePthreadSelf()->userSuspend.load(std::memory_order_acquire));
    if (state != nullptr) {
        auto& host = static_cast<ucontext_t*>(context)->uc_mcontext;
        state->context = {};
        FromHost(host, state->context.mcontext);
        const auto ticket = state->release.load(std::memory_order_acquire);
        state->parked.store(1, std::memory_order_release);
        Futex(state->parked, FUTEX_WAKE_PRIVATE, INT32_MAX);
        while (state->release.load(std::memory_order_acquire) == ticket) Futex(state->release, FUTEX_WAIT_PRIVATE, ticket);
        if (state->contextChanged) ToHost(state->context.mcontext, host);
        state->parked.store(0, std::memory_order_release);
        Futex(state->parked, FUTEX_WAKE_PRIVATE, INT32_MAX);
    }
    errno = savedErrno;
}

void InstallPark() {
    static const bool installed = [] {
        struct sigaction action{};
        action.sa_sigaction = Park;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        // An exception raised at a parked thread is delivered once it resumes.
        sigaddset(&action.sa_mask, HostSignal);
        if (sigaction(SuspendSignal, &action, nullptr) != 0)
            throw std::system_error(errno, std::generic_category(), "pthread_suspend_user_context_np: cannot install the host signal handler");
        return true;
    }();
    (void)installed;
}

// Waits until parked holds the value, giving up if the thread finishes.
bool WaitParked(Pthread thread, UserSuspend& state, std::uint32_t value) {
    const timespec slice{0, 10'000'000};
    while (state.parked.load(std::memory_order_acquire) != value) {
        if (!Alive(thread)) return false;
        Futex(state.parked, FUTEX_WAIT_PRIVATE, value ^ 1u, &slice);
    }
    return true;
}

int Stop(Pthread thread, UserSuspend& state) {
    InstallPark();
    state.contextChanged = false;
    {
        std::lock_guard join(thread->_join_mtx);
        if (!Alive(thread)) return GuestEsrch;
        const int result = pthread_kill(thread->hostThread, SuspendSignal);
        if (result == ESRCH) return GuestEsrch;
        if (result != 0) throw std::system_error(result, std::generic_category(), "pthread_suspend_user_context_np: cannot signal the target thread");
    }
    return WaitParked(thread, state, 1) ? 0 : GuestEsrch;
}

void Start(Pthread thread, UserSuspend& state) {
    state.release.fetch_add(1, std::memory_order_acq_rel);
    Futex(state.release, FUTEX_WAKE_PRIVATE, INT32_MAX);
    WaitParked(thread, state, 0);
}
#endif

int Suspend(Pthread thread) {
    if (thread == nullptr) return GuestEinval;
    if (thread == scePthreadSelf()) return GuestEdeadlk;
    if (!Alive(thread)) return GuestEsrch;
    auto& state = SuspendState(thread);
    std::lock_guard lock(state.lock);
    if (state.count > 0) {
        ++state.count;
        return 0;
    }
    const int result = Stop(thread, state);
    if (result == 0) state.count = 1;
    return result;
}

int Resume(Pthread thread) {
    if (thread == nullptr) return GuestEinval;
    auto& state = SuspendState(thread);
    std::lock_guard lock(state.lock);
    if (state.count == 0) return GuestEinval;
    if (--state.count == 0) Start(thread, state);
    return 0;
}

}

void FreeUserSuspend(void* state) {
    delete static_cast<UserSuspend*>(state);
}

extern "C" {

int APS5_VABI pthread_suspend_user_context_np_nid_postfix(Pthread thread) {
 return Suspend(thread);
}

int APS5_VABI pthread_resume_user_context_np_nid_postfix(Pthread thread) {
 return Resume(thread);
}

int APS5_VABI pthread_suspend_np_nid_postfix(Pthread thread) {
 return Suspend(thread);
}

int APS5_VABI pthread_resume_np_nid_postfix(Pthread thread) {
 return Resume(thread);
}

// The registers of a thread this caller suspended, as a guest ucontext.
int APS5_VABI pthread_get_user_context_np_nid_postfix(Pthread thread, void* ucontext) {
 if (thread == nullptr || ucontext == nullptr) return GuestEinval;
 auto& state = SuspendState(thread);
 std::lock_guard lock(state.lock);
 if (state.count == 0) return GuestEinval;
 std::memcpy(ucontext, &state.context, sizeof(GuestUcontext));
 return 0;
}

// Replaces the registers a suspended thread resumes with.
int APS5_VABI pthread_set_user_context_np_nid_postfix(Pthread thread, const void* ucontext) {
 if (thread == nullptr || ucontext == nullptr) return GuestEinval;
 auto& state = SuspendState(thread);
 std::lock_guard lock(state.lock);
 if (state.count == 0) return GuestEinval;
 std::memcpy(&state.context, ucontext, sizeof(GuestUcontext));
 state.contextChanged = true;
 return 0;
}

int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler) {
 if (!Allowed(signum) || handler == nullptr) return SCE_KERNEL_ERROR_EINVAL;
 void* expected = nullptr;
 if (!handlers[signum].compare_exchange_strong(expected, handler, std::memory_order_acq_rel)) return SCE_KERNEL_ERROR_EAGAIN;
 return 0;
}

int APS5_VABI sceKernelRemoveExceptionHandler(int signum) {
 if (!Allowed(signum)) return SCE_KERNEL_ERROR_EINVAL;
 handlers[signum].store(nullptr, std::memory_order_release);
 return 0;
}

int APS5_VABI sceKernelRaiseException(Pthread thread, int signum) {
 if (signum != RaisedSignal) return SCE_KERNEL_ERROR_EINVAL;
 if (thread == nullptr || thread->_finished.load(std::memory_order_acquire)) return SCE_KERNEL_ERROR_ESRCH;
 const auto handler = Handler(signum);
 if (handler == nullptr) throw std::runtime_error("sceKernelRaiseException: no handler installed for the signal");
#ifdef _WIN32
 return RaiseOn(thread, handler, signum) ? 0 : SCE_KERNEL_ERROR_ESRCH;
#else
 return RaiseOn(thread) ? 0 : SCE_KERNEL_ERROR_ESRCH;
#endif
}

void APS5_VABI sceKernelDebugRaiseException(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseException c1=%d c2=%d", c1, c2);
}

void APS5_VABI sceKernelDebugRaiseExceptionOnReleaseMode(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseExceptionOnReleaseMode c1=%d c2=%d", c1, c2);
}

}
