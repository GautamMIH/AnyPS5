#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <new>
#include <string>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

struct KernelEventFlagPrivate {
    std::mutex Mutex;
    std::condition_variable Changed;
    std::uint64_t Pattern = 0;
    std::uint32_t Attributes = 0;
    std::string Name;
    int Waiters = 0;
    std::uint64_t CancelGeneration = 0;
    bool Deleted = false;
};

namespace {

constexpr int kErrorPermission = static_cast<int>(0x80020001);
constexpr int kErrorNoSuchObject = static_cast<int>(0x80020003);
constexpr int kErrorNoMemory = static_cast<int>(0x8002000C);
constexpr int kErrorBusy = static_cast<int>(0x80020010);
constexpr int kErrorInvalid = static_cast<int>(0x80020016);
constexpr int kErrorTimedOut = static_cast<int>(0x8002003C);
constexpr int kErrorCanceled = static_cast<int>(0x80020055);

constexpr std::uint32_t kAttributeSingle = 0x10;
constexpr std::uint32_t kAttributeMulti = 0x20;
constexpr std::uint32_t kWaitAnd = 0x01;
constexpr std::uint32_t kWaitOr = 0x02;
constexpr std::uint32_t kWaitClearAll = 0x10;
constexpr std::uint32_t kWaitClearPattern = 0x20;

bool validWaitMode(const std::uint32_t mode) {
    const std::uint32_t condition = mode & (kWaitAnd | kWaitOr);
    const std::uint32_t clear = mode & (kWaitClearAll | kWaitClearPattern);
    return (condition == kWaitAnd || condition == kWaitOr) && clear != (kWaitClearAll | kWaitClearPattern) && (mode & ~(kWaitAnd | kWaitOr | kWaitClearAll | kWaitClearPattern)) == 0;
}

bool satisfied(const std::uint64_t pattern, const std::uint64_t wanted, const std::uint32_t mode) {
    return (mode & kWaitAnd) != 0 ? (pattern & wanted) == wanted : (pattern & wanted) != 0;
}

void consume(KernelEventFlagPrivate& flag, const std::uint64_t wanted, const std::uint32_t mode, std::uint64_t* result) {
    if (result != nullptr)
        *result = flag.Pattern;
    if ((mode & kWaitClearAll) != 0)
        flag.Pattern = 0;
    else if ((mode & kWaitClearPattern) != 0)
        flag.Pattern &= ~wanted;
}

}

extern "C" {

int APS5_VABI sceKernelCreateEventFlag(KernelEventFlag* ef, const char* name, uint32_t attr, uint64_t init_pattern, const void* param) {
    (void)param;
    if (!ef || !name || ((attr & kAttributeSingle) != 0 && (attr & kAttributeMulti) != 0)) return kErrorInvalid;
    auto* flag = new (std::nothrow) KernelEventFlagPrivate();
    if (!flag) return kErrorNoMemory;
    flag->Name = name;
    flag->Attributes = attr;
    flag->Pattern = init_pattern;
    *ef = flag;
    return 0;
}

int APS5_VABI sceKernelDeleteEventFlag(KernelEventFlag ef) {
    if (!ef) return kErrorNoSuchObject;
    {
        std::unique_lock lock(ef->Mutex);
        ef->Deleted = true;
        ++ef->CancelGeneration;
        ef->Changed.notify_all();
        ef->Changed.wait(lock, [ef] { return ef->Waiters == 0; });
    }
    delete ef;
    return 0;
}

int APS5_VABI sceKernelSetEventFlag(KernelEventFlag ef, uint64_t bit_pattern) {
    if (!ef) return kErrorNoSuchObject;
    const std::lock_guard lock(ef->Mutex);
    ef->Pattern |= bit_pattern;
    ef->Changed.notify_all();
    return 0;
}

int APS5_VABI sceKernelClearEventFlag(KernelEventFlag ef, uint64_t bit_pattern) {
    if (!ef) return kErrorNoSuchObject;
    const std::lock_guard lock(ef->Mutex);
    ef->Pattern &= bit_pattern;
    return 0;
}

int APS5_VABI sceKernelCancelEventFlag(KernelEventFlag ef, uint64_t set_pattern, int* num_wait_threads) {
    if (!ef) return kErrorNoSuchObject;
    const std::lock_guard lock(ef->Mutex);
    if (num_wait_threads) *num_wait_threads = ef->Waiters;
    ef->Pattern = set_pattern;
    ++ef->CancelGeneration;
    ef->Changed.notify_all();
    return 0;
}

int APS5_VABI sceKernelPollEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode, uint64_t* result_pat) {
    if (!ef) return kErrorNoSuchObject;
    if (bit_pattern == 0 || !validWaitMode(wait_mode)) return kErrorInvalid;
    const std::lock_guard lock(ef->Mutex);
    if ((ef->Attributes & kAttributeSingle) != 0 && ef->Waiters != 0) return kErrorPermission;
    if (!satisfied(ef->Pattern, bit_pattern, wait_mode)) return kErrorBusy;
    consume(*ef, bit_pattern, wait_mode, result_pat);
    return 0;
}

int APS5_VABI sceKernelWaitEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode, uint64_t* result_pat, KernelUseconds* timeout) {
    if (!ef) return kErrorNoSuchObject;
    if (bit_pattern == 0 || !validWaitMode(wait_mode)) return kErrorInvalid;
    std::unique_lock lock(ef->Mutex);
    if ((ef->Attributes & kAttributeSingle) != 0 && ef->Waiters != 0) return kErrorPermission;
    const auto start = std::chrono::steady_clock::now();
    const std::uint64_t generation = ef->CancelGeneration;
    const auto ready = [&] { return ef->CancelGeneration != generation || satisfied(ef->Pattern, bit_pattern, wait_mode); };
    ++ef->Waiters;
    bool completed = true;
    if (timeout)
        completed = ef->Changed.wait_until(lock, start + std::chrono::microseconds(*timeout), ready);
    else
        ef->Changed.wait(lock, ready);
    --ef->Waiters;
    const bool canceled = ef->CancelGeneration != generation;
    if (ef->Deleted)
        ef->Changed.notify_all();
    if (timeout) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        *timeout = elapsed >= static_cast<long long>(*timeout) ? 0 : *timeout - static_cast<KernelUseconds>(elapsed);
    }
    if (canceled) {
        if (result_pat) *result_pat = ef->Pattern;
        return kErrorCanceled;
    }
    if (!completed) {
        if (result_pat) *result_pat = ef->Pattern;
        return kErrorTimedOut;
    }
    consume(*ef, bit_pattern, wait_mode, result_pat);
    return 0;
}

}
