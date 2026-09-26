#include <array>
#include <mutex>
#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr int kMaxSignal = 128;
constexpr int kErrorInvalid = static_cast<int>(0x80020016);
constexpr int kErrorAlreadyExists = static_cast<int>(0x80020011);
std::mutex exceptionMutex;
std::array<void*, kMaxSignal> exceptionHandlers{};

}

extern "C" {

int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler) {
 if (signum <= 0 || signum >= kMaxSignal || !handler) return kErrorInvalid;
 const std::lock_guard lock(exceptionMutex);
 if (exceptionHandlers[signum] != nullptr) return kErrorAlreadyExists;
 exceptionHandlers[signum] = handler;
 return 0;
}

int APS5_VABI sceKernelRemoveExceptionHandler(int signum) {
 if (signum <= 0 || signum >= kMaxSignal) return kErrorInvalid;
 const std::lock_guard lock(exceptionMutex);
 exceptionHandlers[signum] = nullptr;
 return 0;
}

int APS5_VABI sceKernelRaiseException(Pthread thread, int signum) {
 (void)thread;
 (void)signum;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

void APS5_VABI sceKernelDebugRaiseException(int c1, int c2) {
 (void)c1;
 (void)c2;
 NotImplemented_nid_no_patch(__func__);
}

void APS5_VABI sceKernelDebugRaiseExceptionOnReleaseMode(int c1, int c2) {
 (void)c1;
 (void)c2;
 NotImplemented_nid_no_patch(__func__);
}

}
