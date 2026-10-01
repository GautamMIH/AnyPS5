#include <cerrno>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/File/include/FileErrors.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "prx/libc/include/GuestMemoryTracking.hpp"

#ifdef _WIN32
#include <io.h>
#include <limits>
#include <stdio.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr std::int32_t kStateCompleted = 3;
constexpr std::int32_t kStateAborted = 4;
constexpr std::uint32_t kWaitAnd = 1;
constexpr std::uint32_t kWaitOr = 2;

struct SchedulingParam {
    std::int32_t schedulingWindowSize;
    std::int32_t delay;
    std::uint32_t enableSplit;
    std::uint32_t splitSize;
    std::uint32_t splitChunkSize;
};

struct AioParam {
    SchedulingParam low;
    SchedulingParam mid;
    SchedulingParam high;
};

// Submitted requests run to completion before the submit call returns; the table keeps each
// request's final state until the guest deletes it.
std::mutex requestMutex;
std::unordered_map<std::int32_t, std::int32_t> requests;
std::int32_t nextRequest = 1;

#ifdef _WIN32
// Windows has no pread/pwrite: a duplicated descriptor keeps the caller's file position intact.
template <typename Transfer>
std::int64_t atOffset(const std::int32_t fd, const std::size_t nbyte, const std::int64_t offset, Transfer transfer) {
    if (nbyte > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max()))
        throw std::runtime_error("sceKernelAio: transfer size exceeds the platform limit");
    const int duped = ::_dup(fd);
    if (duped < 0) return -1;
    if (::_lseeki64(duped, offset, SEEK_SET) < 0) {
        const int error = errno;
        ::_close(duped);
        errno = error;
        return -1;
    }
    const int result = transfer(duped, static_cast<unsigned int>(nbyte));
    const int error = errno;
    ::_close(duped);
    errno = error;
    return result;
}
#endif

std::int64_t transfer(const KernelAioRwRequest& request, const bool write) {
    // As for any system call on guest memory: tracked pages are resolved first (see Open.cpp).
    if (request.buf != nullptr && request.nbyte != 0) GuestMemoryTracking::GuestMemoryTrackingResolve_nid_postfix(reinterpret_cast<std::uint64_t>(request.buf), request.nbyte, !write);
#ifdef _WIN32
    const auto result = atOffset(request.fd, request.nbyte, static_cast<std::int64_t>(request.offset), [&](const int fd, const unsigned int count) {
        return write ? ::_write(fd, request.buf, count) : ::_read(fd, request.buf, count);
    });
#else
    const auto result = write ? ::pwrite(request.fd, request.buf, request.nbyte, static_cast<off_t>(request.offset)) : ::pread(request.fd, request.buf, request.nbyte, static_cast<off_t>(request.offset));
#endif
    return result < 0 ? FileErrors::Sce(errno) : static_cast<std::int64_t>(result);
}

std::int32_t complete(KernelAioRwRequest* requestsToRun, const std::int32_t count, const bool write) {
    bool aborted = false;
    for (std::int32_t index = 0; index < count; ++index) {
        const std::int64_t result = transfer(requestsToRun[index], write);
        const bool failed = result < 0;
        requestsToRun[index].result->return_value = result;
        requestsToRun[index].result->state = static_cast<std::uint32_t>(failed ? kStateAborted : kStateCompleted);
        aborted = aborted || failed;
    }
    const std::lock_guard lock(requestMutex);
    const std::int32_t id = nextRequest++;
    requests.emplace(id, aborted ? kStateAborted : kStateCompleted);
    return id;
}

int validate(const KernelAioRwRequest* request, const std::int32_t size, const void* ids) {
    if (!request || !ids) return SCE_KERNEL_ERROR_EFAULT;
    if (size <= 0) return SCE_KERNEL_ERROR_EINVAL;
    for (std::int32_t index = 0; index < size; ++index)
        if (!request[index].result) return SCE_KERNEL_ERROR_EFAULT;
    return 0;
}

int submit(KernelAioRwRequest* request, const std::int32_t size, std::int32_t* id, const bool write) {
    if (const int error = validate(request, size, id)) return error;
    *id = complete(request, size, write);
    return 0;
}

int submitMultiple(KernelAioRwRequest* request, const std::int32_t size, std::int32_t* ids, const bool write) {
    if (const int error = validate(request, size, ids)) return error;
    for (std::int32_t index = 0; index < size; ++index)
        ids[index] = complete(&request[index], 1, write);
    return 0;
}

int stateOf(const std::int32_t id, std::int32_t* state) {
    if (!state) return SCE_KERNEL_ERROR_EFAULT;
    const std::lock_guard lock(requestMutex);
    const auto found = requests.find(id);
    if (found == requests.end()) return SCE_KERNEL_ERROR_EINVAL;
    *state = found->second;
    return 0;
}

}

extern "C" {

void APS5_VABI sceKernelAioInitializeParam(void* param) {
    if (!param) return;
    auto* value = static_cast<AioParam*>(param);
    const SchedulingParam defaults{32, 0, 1, 0x100000, 0x100000};
    value->low = defaults;
    value->mid = defaults;
    value->high = defaults;
}

// Scheduling parameters only tune the PS5 I/O scheduler; requests here complete synchronously.
int APS5_VABI sceKernelAioInitializeImpl(void* param, int32_t size) {
    (void)param;
    (void)size;
    return 0;
}

int APS5_VABI sceKernelAioSetParam(void* param, int32_t window, int32_t delay, int32_t enable_split, int32_t split_size, int32_t split_chunk_size) {
    if (!param) return SCE_KERNEL_ERROR_EINVAL;
    auto* value = static_cast<SchedulingParam*>(param);
    value->schedulingWindowSize = window;
    value->delay = delay;
    value->enableSplit = static_cast<std::uint32_t>(enable_split);
    value->splitSize = static_cast<std::uint32_t>(split_size);
    value->splitChunkSize = static_cast<std::uint32_t>(split_chunk_size);
    return 0;
}

int APS5_VABI sceKernelAioSubmitReadCommands(KernelAioRwRequest* req, int32_t size, int32_t prio, int32_t* id) {
    (void)prio;
    return submit(req, size, id, false);
}

int APS5_VABI sceKernelAioSubmitWriteCommands(KernelAioRwRequest* req, int32_t size, int32_t prio, int32_t* id) {
    (void)prio;
    return submit(req, size, id, true);
}

int APS5_VABI sceKernelAioSubmitReadCommandsMultiple(KernelAioRwRequest* req, int32_t size, int32_t prio, int32_t* ids) {
    (void)prio;
    return submitMultiple(req, size, ids, false);
}

int APS5_VABI sceKernelAioSubmitWriteCommandsMultiple(KernelAioRwRequest* req, int32_t size, int32_t prio, int32_t* ids) {
    (void)prio;
    return submitMultiple(req, size, ids, true);
}

int APS5_VABI sceKernelAioPollRequest(int32_t id, int32_t* state) {
    return stateOf(id, state);
}

int APS5_VABI sceKernelAioPollRequests(const int32_t* ids, int32_t count, int32_t* states) {
    if (!ids || !states) return SCE_KERNEL_ERROR_EFAULT;
    if (count <= 0) return SCE_KERNEL_ERROR_EINVAL;
    for (int32_t index = 0; index < count; ++index) {
        const int result = stateOf(ids[index], &states[index]);
        if (result != 0) return result;
    }
    return 0;
}

// Requests never stay in the processing state, so a wait returns the final state at once.
int APS5_VABI sceKernelAioWaitRequest(int32_t id, int32_t* state, uint32_t* usec) {
    (void)usec;
    return stateOf(id, state);
}

int APS5_VABI sceKernelAioWaitRequests(const int32_t* ids, int32_t count, int32_t* states, uint32_t mode, uint32_t* usec) {
    (void)usec;
    if (mode != kWaitAnd && mode != kWaitOr) return SCE_KERNEL_ERROR_EINVAL;
    return sceKernelAioPollRequests(ids, count, states);
}

int APS5_VABI sceKernelAioCancelRequest(int32_t id, int32_t* state) {
    return stateOf(id, state);
}

int APS5_VABI sceKernelAioDeleteRequest(int32_t id, int32_t* ret) {
    if (!ret) return SCE_KERNEL_ERROR_EFAULT;
    const std::lock_guard lock(requestMutex);
    if (requests.erase(id) == 0) return SCE_KERNEL_ERROR_EINVAL;
    *ret = 0;
    return 0;
}

int APS5_VABI sceKernelAioDeleteRequests(const int32_t* ids, int32_t count, int32_t* rets) {
    if (!ids || !rets) return SCE_KERNEL_ERROR_EFAULT;
    if (count <= 0) return SCE_KERNEL_ERROR_EINVAL;
    for (int32_t index = 0; index < count; ++index) {
        const int result = sceKernelAioDeleteRequest(ids[index], &rets[index]);
        if (result != 0) return result;
    }
    return 0;
}

}
