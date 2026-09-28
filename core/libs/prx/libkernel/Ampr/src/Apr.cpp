#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Ampr/include/AmprPackets.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"
#include "prx/libkernel/File/include/FileErrors.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using AmprCommandBuffer = AmprPackets::CommandBuffer;

constexpr int kBsdPermission = 1;
constexpr int kBsdNoEntry = 2;
constexpr int kBsdBadDescriptor = 9;
constexpr int kBsdFault = 14;
constexpr int kBsdIsDirectory = 21;
constexpr int kBsdInvalid = 22;
constexpr std::uint32_t kMaxResolveCount = 1024;
constexpr std::uint32_t kPriorityCount = 7;
constexpr std::uint32_t kResultIo = 0x80020005;
constexpr std::uint32_t kResultBadFile = 0x80020009;
constexpr std::uint32_t kResultInvalid = 0x80020016;

struct AprResult {
    std::uint32_t result;
    std::uint32_t errorOffset;
};

struct OpenFile {
    int Descriptor;
    std::uint64_t Size;
};

std::mutex fileMutex;
std::map<std::string, std::uint32_t> idsByPath;
std::map<std::uint32_t, OpenFile> filesById;
std::uint32_t nextFileId = 1;
std::atomic<std::uint32_t> nextSubmitId{1};

int failure(const int bsdError) {
    errno = bsdError;
    return -1;
}

int resolve(const char* path, std::uint32_t& id, std::uint64_t& size) {
#if defined(__linux__)
    const std::string native = ResolvePath_nid_no_patch(path).string();
    const std::lock_guard lock(fileMutex);
    const auto known = idsByPath.find(native);
    if (known != idsByPath.end()) {
        id = known->second;
        size = filesById.at(id).Size;
        return 0;
    }
    const int descriptor = ::open(native.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return FileErrors::ToBsd(errno);
    struct stat info{};
    if (::fstat(descriptor, &info) != 0 || S_ISDIR(info.st_mode)) {
        const int error = S_ISDIR(info.st_mode) ? kBsdIsDirectory : FileErrors::ToBsd(errno);
        ::close(descriptor);
        return error;
    }
    if (nextFileId > AmprPackets::kFileIdMask) {
        ::close(descriptor);
        return kBsdInvalid;
    }
    id = nextFileId++;
    size = static_cast<std::uint64_t>(info.st_size);
    idsByPath.emplace(native, id);
    filesById.emplace(id, OpenFile{descriptor, size});
    return 0;
#else
    (void)path;
    (void)id;
    (void)size;
    NotImplemented_nid_no_patch("sceKernelApr resolve");
    return kBsdInvalid;
#endif
}

bool traceFiles() {
    static const bool enabled = [] {
        const char* value = std::getenv("ANYPS5_TRACE_FILES");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

int resolveMany(const char* const* paths, std::uint32_t count, std::uint32_t* ids, std::uint64_t* sizes, std::uint32_t* errorIndex) {
    if (errorIndex) *errorIndex = 0;
    if (count == 0 || count > kMaxResolveCount) return failure(kBsdInvalid);
    if (!paths || !ids) return failure(kBsdFault);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint64_t size = 0;
        const int error = paths[index] ? resolve(paths[index], ids[index], size) : kBsdFault;
        if (traceFiles()) std::fprintf(stderr, "[AnyPS5 file] apr-resolve %s = %d (id %u, %llu bytes)\n", paths[index] ? paths[index] : "(null)", error, error == 0 ? ids[index] : 0u, static_cast<unsigned long long>(size));
        if (error != 0) {
            if (errorIndex) *errorIndex = index;
            return failure(error);
        }
        if (sizes) sizes[index] = size;
    }
    return 0;
}

std::uint32_t readFile(const AmprPackets::ReadFile& packet) {
#if defined(__linux__)
    int descriptor;
    {
        const std::lock_guard lock(fileMutex);
        const auto file = filesById.find(packet.FileId);
        if (file == filesById.end()) return kResultBadFile;
        descriptor = file->second.Descriptor;
    }
    auto* destination = reinterpret_cast<std::uint8_t*>(packet.Destination);
    std::uint64_t done = 0;
    while (done < packet.Size) {
        const auto result = ::pread(descriptor, destination + done, packet.Size - done, static_cast<off_t>(packet.FileOffset + done));
        if (result < 0) {
            if (errno == EINTR) continue;
            return static_cast<std::uint32_t>(FileErrors::Sce(errno));
        }
        if (result == 0) return kResultIo;
        done += static_cast<std::uint64_t>(result);
    }
    return 0;
#else
    (void)packet;
    NotImplemented_nid_no_patch("sceKernelApr read");
    return kResultInvalid;
#endif
}

AprResult execute(const AmprCommandBuffer& cb) {
    const auto* bytes = static_cast<const std::uint8_t*>(cb.buffer);
    for (std::uint32_t offset = 0; offset < cb.currentOffset;) {
        if (cb.currentOffset - offset < 4) return {kResultInvalid, offset};
        const auto* packet = reinterpret_cast<const std::uint32_t*>(bytes + offset);
        const std::uint32_t size = AmprPackets::PacketSize(packet[0]);
        if (cb.currentOffset - offset < size) return {kResultInvalid, offset};
        switch (packet[0] & AmprPackets::kOpcodeMask) {
        case AmprPackets::kOpcodeReadFile: {
            const std::uint32_t result = readFile(AmprPackets::DecodeReadFile(packet));
            if (result != 0) return {result, offset};
            break;
        }
        case AmprPackets::kOpcodeWriteKernelEventQueue:
        case AmprPackets::kOpcodeWriteKernelEventQueueFlagged: {
            const auto event = AmprPackets::DecodeWriteKernelEventQueue(packet);
            EqueueTriggerEvent_nid_postfix(static_cast<KernelEqueue>(event.Queue), static_cast<uintptr_t>(static_cast<std::uint32_t>(event.Id)), EVFILT_AMPR, reinterpret_cast<void*>(static_cast<uintptr_t>(event.Data)));
            break;
        }
        case AmprPackets::kOpcodeWriteAddress: {
            const auto write = AmprPackets::DecodeWriteAddress(packet);
            if (write.Flags != 0) {
                NotImplemented_nid_no_patch("sceKernelApr WriteAddress flags");
                return {kResultInvalid, offset};
            }
            if (write.Address == 0 || (write.Address & 7) != 0) return {kResultInvalid, offset};
            std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(write.Address)).store(write.Value, std::memory_order_release);
            break;
        }
        default:
            return {kResultInvalid, offset};
        }
        offset += size;
    }
    return {0, 0};
}

int submit(AmprCommandBuffer* cb, std::uint32_t priority, AprResult* result, std::uint32_t* submitId) {
    if (priority >= kPriorityCount) {
        if (result) *result = {kResultInvalid, 0};
        return failure(kBsdInvalid);
    }
    if (!cb || !cb->buffer) return failure(kBsdPermission);
    if (cb->currentOffset == 0) return failure(kBsdInvalid);
    const AprResult outcome = execute(*cb);
    if (result) *result = outcome;
    if (submitId) *submitId = nextSubmitId.fetch_add(1);
    return 0;
}

}

extern "C" {

int APS5_VABI sceKernelAprResolveFilepathsToIdsAndFileSizes(const char* const* paths, uint32_t count, uint32_t* ids, uint64_t* sizes, uint32_t* error_index) {
    if (!sizes) return failure(kBsdFault);
    return resolveMany(paths, count, ids, sizes, error_index);
}

int APS5_VABI sceKernelAprResolveFilepathsToIds(const char* const* paths, uint32_t count, uint32_t* ids, uint32_t* error_index) {
    return resolveMany(paths, count, ids, nullptr, error_index);
}

int APS5_VABI sceKernelAprGetFileSize(uint32_t file_id, uint64_t* size) {
    if (!size) return failure(kBsdFault);
    const std::lock_guard lock(fileMutex);
    const auto file = filesById.find(file_id);
    if (file == filesById.end()) return failure(kBsdBadDescriptor);
    *size = file->second.Size;
    return 0;
}

int APS5_VABI sceKernelAprSubmitCommandBuffer(AmprCommandBuffer* cb, uint32_t priority) {
    return submit(cb, priority, nullptr, nullptr);
}

int APS5_VABI sceKernelAprSubmitCommandBufferAndGetId(AmprCommandBuffer* cb, uint32_t priority, uint32_t* submit_id) {
    return submit(cb, priority, nullptr, submit_id);
}

int APS5_VABI sceKernelAprSubmitCommandBufferAndGetResult(AmprCommandBuffer* cb, uint32_t priority, AprResult* result, uint32_t* submit_id) {
    return submit(cb, priority, result, submit_id);
}

int APS5_VABI sceKernelAprWaitCommandBuffer(uint32_t submit_id) {
    if (submit_id == 0 || submit_id >= nextSubmitId.load()) return failure(kBsdNoEntry);
    return 0;
}

}
