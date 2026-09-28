#include "prx/libkernel/Semaphore/include/Semaphore.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include <chrono>

#include <stdexcept>
#include <string>
#include <utility>

KernelSemaPrivate::KernelSemaPrivate(std::int32_t initCount, std::int32_t maxCount, std::string name, bool isFifo)
 : name(std::move(name)), initCount(initCount), tokenCount(initCount), maxCount(maxCount), isFifo(isFifo) {
}

extern "C" {

int APS5_VABI sceKernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init, int max, void* opt) {
 (void)opt;
 if (sem == nullptr || name == nullptr || attr > 2 || init < 0 || max <= 0 || init > max) {
  APS5_INVALID_ARG_EX;
 }

 *sem = new KernelSemaPrivate(init, max, std::string(name), attr == 1);
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelPollSema(KernelSema sem, int need) {
 if (sem == nullptr || need <= 0) {
  APS5_INVALID_ARG_EX;
 }

 std::lock_guard<std::mutex> lock(sem->mutex);
 if (sem->tokenCount < need) {
  return KERNEL_SEMA_ERROR_EBUSY;
 }
 sem->tokenCount -= need;
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelSignalSema(KernelSema sem, int count) {
 if (sem == nullptr || count <= 0) {
  APS5_INVALID_ARG_EX;
 }

 std::lock_guard<std::mutex> lock(sem->mutex);
 if (sem->tokenCount + count > sem->maxCount) {
  return KERNEL_SEMA_ERROR_EINVAL;
 }
 sem->tokenCount += count;
 sem->condition.NotifyAll();
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelWaitSema(KernelSema sem, int need, KernelUseconds* time) {
 if (sem == nullptr || need <= 0) {
  APS5_INVALID_ARG_EX;
 }

 std::unique_lock<std::mutex> lock(sem->mutex);
 if (need > sem->maxCount) {
  return KERNEL_SEMA_ERROR_EINVAL;
 }
 const auto generation = sem->cancelGeneration;
 const auto ready = [&] { return sem->deleted || sem->cancelGeneration != generation || sem->tokenCount >= need; };
 ++sem->waiters;
 const auto waitStart = std::chrono::steady_clock::now();
 bool acquired = true;
 if (time == nullptr) {
  sem->condition.Wait(lock, ready);
 } else {
  acquired = sem->condition.WaitUntil(lock, TimedWait::DeadlineNanos(*time), ready);
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - waitStart).count();
  *time = elapsed >= static_cast<std::int64_t>(*time) ? 0 : static_cast<KernelUseconds>(*time - elapsed);
 }
 KernelTraceWait_nid_postfix("sema", __builtin_return_address(0), static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count()), !acquired);
 --sem->waiters;
 if (sem->deleted) {
  sem->condition.NotifyAll();
  return KERNEL_SEMA_ERROR_EACCES;
 }
 if (sem->cancelGeneration != generation) {
  return KERNEL_SEMA_ERROR_ECANCELED;
 }
 if (!acquired) {
  return KERNEL_SEMA_ERROR_ETIMEDOUT;
 }
 sem->tokenCount -= need;
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelCancelSema(KernelSema sem, int count, int* threads) {
 if (sem == nullptr) {
  APS5_INVALID_ARG_EX;
 }

 std::lock_guard<std::mutex> lock(sem->mutex);
 if (count > sem->maxCount) {
  return KERNEL_SEMA_ERROR_EINVAL;
 }
 if (threads != nullptr) {
  *threads = sem->waiters;
 }
 sem->tokenCount = count < 0 ? sem->initCount : count;
 ++sem->cancelGeneration;
 sem->condition.NotifyAll();
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelDeleteSema(KernelSema sem) {
 if (sem == nullptr) {
  APS5_INVALID_ARG_EX;
 }

 {
  std::unique_lock<std::mutex> lock(sem->mutex);
  sem->deleted = true;
  sem->condition.NotifyAll();
  sem->condition.Wait(lock, [&] { return sem->waiters == 0; });
 }
 delete sem;
 return KERNEL_SEMA_OK;
}

}
