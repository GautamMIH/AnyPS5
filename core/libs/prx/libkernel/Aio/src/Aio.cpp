#include <array>
#include <cerrno>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
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

constexpr std::int32_t kStateProcessing = 2;
constexpr std::int32_t kStateCompleted = 3;
constexpr std::int32_t kStateAborted = 4;
constexpr std::uint32_t kWaitAnd = 1;
constexpr std::uint32_t kWaitOr = 2;
// Request ids cycle through a fixed table, as the PS5 AIO queue ids do.
constexpr std::int32_t kMaxRequests = 512;
constexpr std::int32_t kRequestNumMax = 128;
constexpr std::int32_t kIdNumMax = 128;

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
// request's final state. A deleted or cancelled request reads as aborted until its id is reused.
std::mutex requestMutex;
std::array<std::int32_t, kMaxRequests> requests{};
std::int32_t nextRequest = 1;

bool validId(const std::int32_t id) {
    return id > 0 && id < kMaxRequests;
}

std::int32_t allocate(const std::int32_t state) {
    const std::lock_guard lock(requestMutex);
    const std::int32_t id = nextRequest;
    nextRequest = nextRequest + 1 == kMaxRequests ? 1 : nextRequest + 1;
    requests[id] = state;
    return id;
}

void setState(const std::int32_t id, const std::int32_t state) {
    const std::lock_guard lock(requestMutex);
    requests[id] = state;
}

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
    return allocate(aborted ? kStateAborted : kStateCompleted);
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

int submitMultiple(KernelAioRwRequest* request, const std::int32_t size, std::int32_t* ids, const bool write, const char* name) {
    if (const int error = validate(request, size, ids)) return error;
    if (size > kRequestNumMax) throw std::runtime_error(std::string(name) + ": more than 128 requests per batch is not modelled");
    for (std::int32_t index = 0; index < size; ++index)
        ids[index] = complete(&request[index], 1, write);
    return 0;
}

// Batch calls check every id before touching any output.
int validateIds(const std::int32_t* ids, const std::int32_t count, const void* out, const char* name, const bool allowZero) {
    if (!ids || !out) return SCE_KERNEL_ERROR_EFAULT;
    if (count < 0) return SCE_KERNEL_ERROR_EINVAL;
    if (count > kIdNumMax) throw std::runtime_error(std::string(name) + ": more than 128 ids per batch is not modelled");
    for (std::int32_t index = 0; index < count; ++index)
        if (!(allowZero && ids[index] == 0) && !validId(ids[index])) return SCE_KERNEL_ERROR_EINVAL;
    return 0;
}

int stateOf(const std::int32_t id, std::int32_t* state) {
    if (!state) return SCE_KERNEL_ERROR_EFAULT;
    if (!validId(id)) return SCE_KERNEL_ERROR_EINVAL;
    const std::lock_guard lock(requestMutex);
    *state = requests[id];
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
    return submitMultiple(req, size, ids, false, "sceKernelAioSubmitReadCommandsMultiple");
}

int APS5_VABI sceKernelAioSubmitWriteCommandsMultiple(KernelAioRwRequest* req, int32_t size, int32_t prio, int32_t* ids) {
    (void)prio;
    return submitMultiple(req, size, ids, true, "sceKernelAioSubmitWriteCommandsMultiple");
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
    if (const int error = validateIds(ids, count, states, "sceKernelAioWaitRequests", false)) return error;
    if (mode != kWaitAnd && mode != kWaitOr) throw std::runtime_error("sceKernelAioWaitRequests: unsupported wait mode " + std::to_string(mode));
    const std::lock_guard lock(requestMutex);
    for (int32_t index = 0; index < count; ++index)
        states[index] = requests[ids[index]];
    return 0;
}

// Id 0 names no request: it reports processing and changes nothing.
int APS5_VABI sceKernelAioCancelRequest(int32_t id, int32_t* state) {
    if (!state) return SCE_KERNEL_ERROR_EFAULT;
    if (id == 0) {
        *state = kStateProcessing;
        return 0;
    }
    if (!validId(id)) return SCE_KERNEL_ERROR_EINVAL;
    setState(id, kStateAborted);
    *state = kStateAborted;
    return 0;
}

int APS5_VABI sceKernelAioCancelRequests(const int32_t* ids, int32_t count, int32_t* states) {
    if (const int error = validateIds(ids, count, states, "sceKernelAioCancelRequests", true)) return error;
    for (int32_t index = 0; index < count; ++index) {
        if (ids[index] == 0) {
            states[index] = kStateProcessing;
            continue;
        }
        setState(ids[index], kStateAborted);
        states[index] = kStateAborted;
    }
    return 0;
}

int APS5_VABI sceKernelAioDeleteRequest(int32_t id, int32_t* ret) {
    if (!ret) return SCE_KERNEL_ERROR_EFAULT;
    if (!validId(id)) return SCE_KERNEL_ERROR_EINVAL;
    setState(id, kStateAborted);
    *ret = 0;
    return 0;
}

int APS5_VABI sceKernelAioDeleteRequests(const int32_t* ids, int32_t count, int32_t* rets) {
    if (const int error = validateIds(ids, count, rets, "sceKernelAioDeleteRequests", false)) return error;
    for (int32_t index = 0; index < count; ++index) {
        setState(ids[index], kStateAborted);
        rets[index] = 0;
    }
    return 0;
}

}
