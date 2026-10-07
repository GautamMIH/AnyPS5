#include <array>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"

#ifndef _WIN32
#include <csignal>
#include <pthread.h>
#include <ucontext.h>
#endif

// Guest exception handlers receive (signal number, FreeBSD amd64 ucontext). Software exceptions
// raised with sceKernelRaiseException are delivered on the target thread through a host real-time
// signal allocated per guest signal number; the host context is presented in the guest layout and
// register changes made by the handler are applied on return. Hardware faults are not routed here.

namespace {

constexpr int kMaxSignal = 128;
constexpr int kErrorInvalid = static_cast<int>(0x80020016);
constexpr int kErrorNoThread = static_cast<int>(0x80020003);
constexpr int kErrorAgain = static_cast<int>(0x80020023);

using GuestHandler = void (APS5_VABI*)(int, void*);

// Handlers may be installed for SIGHUP, SIGILL, SIGFPE, SIGBUS, SIGSEGV and SIGUSR1 (as in shadPS4).
bool AllowedSignal(int signum) {
    for (const int allowed : {1, 4, 8, 10, 11, 30})
        if (allowed == signum) return true;
    return false;
}

std::mutex exceptionMutex;
std::array<std::atomic<void*>, kMaxSignal> exceptionHandlers{};

#ifndef _WIN32

// Guest ucontext: a 16-byte signal mask, 0x30 reserved bytes, then the FreeBSD amd64 mcontext_t
// (handlers read mc_rsp at ucontext + 0xf8).
constexpr std::size_t kUcontextSize = 0x400;
constexpr std::size_t kMcontext = 0x40;
constexpr std::size_t kMcRdi = 0x08, kMcRsi = 0x10, kMcRdx = 0x18, kMcRcx = 0x20, kMcR8 = 0x28, kMcR9 = 0x30;
constexpr std::size_t kMcRax = 0x38, kMcRbx = 0x40, kMcRbp = 0x48, kMcR10 = 0x50, kMcR11 = 0x58, kMcR12 = 0x60;
constexpr std::size_t kMcR13 = 0x68, kMcR14 = 0x70, kMcR15 = 0x78, kMcRip = 0xa0, kMcRflags = 0xb0, kMcRsp = 0xb8;
constexpr std::size_t kMcLen = 0xc8, kMcFpFormat = 0xd0, kMcOwnedFp = 0xd8, kMcFpState = 0xe0;
constexpr std::uint64_t kMcontextSize = 0x320, kFpFormatXmm = 0x10002, kFpOwnedFpu = 0x20001;

struct RegisterSlot {
    std::size_t guest;
    int host;
};
constexpr RegisterSlot kRegisters[] = {
    {kMcRdi, REG_RDI}, {kMcRsi, REG_RSI}, {kMcRdx, REG_RDX}, {kMcRcx, REG_RCX}, {kMcR8, REG_R8}, {kMcR9, REG_R9},
    {kMcRax, REG_RAX}, {kMcRbx, REG_RBX}, {kMcRbp, REG_RBP}, {kMcR10, REG_R10}, {kMcR11, REG_R11}, {kMcR12, REG_R12},
    {kMcR13, REG_R13}, {kMcR14, REG_R14}, {kMcR15, REG_R15}, {kMcRip, REG_RIP}, {kMcRflags, REG_EFL}, {kMcRsp, REG_RSP},
};

// Host real-time signal assigned to each guest signal number (0 = none yet) and the reverse map.
std::array<int, kMaxSignal> hostSignalFor{};
std::array<std::atomic<int>, 64> guestSignalFor{};
int nextHostSignal = 0;

void deliver(int hostSignal, siginfo_t*, void* hostContext) {
    const int offset = hostSignal - SIGRTMIN;
    if (offset < 0 || offset >= static_cast<int>(guestSignalFor.size())) return;
    const int guestSignal = guestSignalFor[static_cast<std::size_t>(offset)].load();
    auto* handler = reinterpret_cast<GuestHandler>(guestSignal > 0 ? exceptionHandlers[static_cast<std::size_t>(guestSignal)].load() : nullptr);
    if (handler == nullptr) return;

    auto* host = static_cast<ucontext_t*>(hostContext);
    alignas(16) unsigned char guest[kUcontextSize]{};
    std::memcpy(guest, &host->uc_sigmask, 16);
    unsigned char* mc = guest + kMcontext;
    for (const auto& slot : kRegisters) std::memcpy(mc + slot.guest, &host->uc_mcontext.gregs[slot.host], 8);
    std::memcpy(mc + kMcLen, &kMcontextSize, 8);
    if (host->uc_mcontext.fpregs != nullptr) {
        std::memcpy(mc + kMcFpState, host->uc_mcontext.fpregs, 512);
        std::memcpy(mc + kMcFpFormat, &kFpFormatXmm, 8);
        std::memcpy(mc + kMcOwnedFp, &kFpOwnedFpu, 8);
    }

    handler(guestSignal, guest);

    for (const auto& slot : kRegisters) std::memcpy(&host->uc_mcontext.gregs[slot.host], mc + slot.guest, 8);
    if (host->uc_mcontext.fpregs != nullptr) std::memcpy(host->uc_mcontext.fpregs, mc + kMcFpState, 512);
}

// Assigns a host real-time signal to the guest signal and installs the delivery handler.
int bindHostSignal(int guestSignal) {
    if (hostSignalFor[static_cast<std::size_t>(guestSignal)] != 0) return 0;
    // Leave the lowest real-time signals to the host runtime and debuggers.
    const int hostSignal = SIGRTMIN + 8 + nextHostSignal;
    if (hostSignal > SIGRTMAX || nextHostSignal >= static_cast<int>(guestSignalFor.size())) return kErrorAgain;
    struct sigaction action{};
    action.sa_sigaction = deliver;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    guestSignalFor[static_cast<std::size_t>(hostSignal - SIGRTMIN)] = guestSignal;
    if (sigaction(hostSignal, &action, nullptr) != 0) {
        guestSignalFor[static_cast<std::size_t>(hostSignal - SIGRTMIN)] = 0;
        return kErrorInvalid;
    }
    hostSignalFor[static_cast<std::size_t>(guestSignal)] = hostSignal;
    ++nextHostSignal;
    return 0;
}

#endif

}

extern "C" {

int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler) {
 if (!AllowedSignal(signum) || !handler) return kErrorInvalid;
 const std::lock_guard lock(exceptionMutex);
 // A second handler for a signal is refused (EAGAIN, as in shadPS4).
 if (exceptionHandlers[static_cast<std::size_t>(signum)].load() != nullptr) return kErrorAgain;
#ifndef _WIN32
 if (const int error = bindHostSignal(signum)) return error;
#endif
 exceptionHandlers[static_cast<std::size_t>(signum)] = handler;
 return 0;
}

int APS5_VABI sceKernelRemoveExceptionHandler(int signum) {
 if (!AllowedSignal(signum)) return kErrorInvalid;
 const std::lock_guard lock(exceptionMutex);
 exceptionHandlers[static_cast<std::size_t>(signum)] = nullptr;
 return 0;
}

int APS5_VABI sceKernelRaiseException(Pthread thread, int signum) {
 if (signum <= 0 || signum >= kMaxSignal) return kErrorInvalid;
#ifdef _WIN32
 (void)thread;
 NotImplemented_nid_no_patch(__func__);
 return 0;
#else
 if (thread == nullptr || !thread->hostRunning.load()) return kErrorNoThread;
 int hostSignal = 0;
 {
  const std::lock_guard lock(exceptionMutex);
  if (exceptionHandlers[static_cast<std::size_t>(signum)].load() == nullptr) return kErrorInvalid;
  hostSignal = hostSignalFor[static_cast<std::size_t>(signum)];
 }
 const int error = pthread_kill(thread->hostThread, hostSignal);
 return error == 0 ? 0 : static_cast<int>(0x80020000u | static_cast<unsigned>(error == ESRCH ? 3 : error));
#endif
}

void APS5_VABI sceKernelDebugRaiseException(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseException c1=%d c2=%d", c1, c2);
}

void APS5_VABI sceKernelDebugRaiseExceptionOnReleaseMode(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseExceptionOnReleaseMode c1=%d c2=%d", c1, c2);
}

}
