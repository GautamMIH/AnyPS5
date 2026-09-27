#include <cerrno>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/File/include/FileErrors.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr int kErrorNoSuchRequest = static_cast<int>(0x80020003);
constexpr int kErrorInvalid = static_cast<int>(0x80020016);
constexpr std::uint32_t kStateCompleted = 3;
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

std::mutex requestMutex;
std::unordered_map<std::int32_t, std::uint32_t> requests;
std::int32_t nextRequest = 1;

std::int64_t transfer(const KernelAioRwRequest& request, const bool write) {
#ifdef _WIN32
    (void)request;
    (void)write;
    NotImplemented_nid_no_patch("sceKernelAio transfer");
    return 0;
#else
    const auto result = write ? ::pwrite(request.fd, request.buf, request.nbyte, static_cast<off_t>(request.offset)) : ::pread(request.fd, request.buf, request.nbyte, static_cast<off_t>(request.offset));
    return result < 0 ? FileErrors::Sce(errno) : static_cast<std::int64_t>(result);
#endif
}

std::int32_t complete(KernelAioRwRequest* requestsToRun, const std::int32_t count, const bool write) {
    for (std::int32_t index = 0; index < count; ++index) {
        const std::int64_t result = transfer(requestsToRun[index], write);
        if (requestsToRun[index].result != nullptr) {
            requestsToRun[index].result->return_value = result;
            requestsToRun[index].result->state = kStateCompleted;
        }
    }
    const std::lock_guard lock(requestMutex);
    const std::int32_t id = nextRequest++;
    requests.emplace(id, kStateCompleted);
    return id;
}

int submit(KernelAioRwRequest* request, const std::int32_t size, std::int32_t* id, const bool write) {
    if (!request || size <= 0 || !id) return kErrorInvalid;
    *id = complete(request, size, write);
    return 0;
}

int submitMultiple(KernelAioRwRequest* request, const std::int32_t size, std::int32_t* ids, const bool write) {
    if (!request || size <= 0 || !ids) return kErrorInvalid;
    for (std::int32_t index = 0; index < size; ++index)
        ids[index] = complete(&request[index], 1, write);
    return 0;
}

int stateOf(const std::int32_t id, std::int32_t* state) {
    const std::lock_guard lock(requestMutex);
    const auto found = requests.find(id);
    if (found == requests.end()) return kErrorNoSuchRequest;
    if (state) *state = static_cast<std::int32_t>(found->second);
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

int APS5_VABI sceKernelAioInitializeImpl(void* param, int32_t size) {
    if (!param || size < static_cast<int32_t>(sizeof(AioParam))) return kErrorInvalid;
    return 0;
}

int APS5_VABI sceKernelAioSetParam(void* param, int32_t window, int32_t delay, int32_t enable_split, int32_t split_size, int32_t split_chunk_size) {
    if (!param) return kErrorInvalid;
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
    if (!ids || count <= 0) return kErrorInvalid;
    for (int32_t index = 0; index < count; ++index) {
        const int result = stateOf(ids[index], states ? &states[index] : nullptr);
        if (result != 0) return result;
    }
    return 0;
}

int APS5_VABI sceKernelAioWaitRequest(int32_t id, int32_t* state, uint32_t* usec) {
    (void)usec;
    return stateOf(id, state);
}

int APS5_VABI sceKernelAioWaitRequests(const int32_t* ids, int32_t count, int32_t* states, uint32_t mode, uint32_t* usec) {
    (void)usec;
    if (mode != kWaitAnd && mode != kWaitOr) return kErrorInvalid;
    return sceKernelAioPollRequests(ids, count, states);
}

int APS5_VABI sceKernelAioCancelRequest(int32_t id, int32_t* state) {
    return stateOf(id, state);
}

int APS5_VABI sceKernelAioDeleteRequest(int32_t id, int32_t* ret) {
    const std::lock_guard lock(requestMutex);
    if (requests.erase(id) == 0) return kErrorNoSuchRequest;
    if (ret) *ret = 0;
    return 0;
}

int APS5_VABI sceKernelAioDeleteRequests(const int32_t* ids, int32_t count, int32_t* rets) {
    if (!ids || count <= 0) return kErrorInvalid;
    for (int32_t index = 0; index < count; ++index) {
        const int result = sceKernelAioDeleteRequest(ids[index], rets ? &rets[index] : nullptr);
        if (result != 0) return result;
    }
    return 0;
}

}
