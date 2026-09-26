#include <cstdint>
#include <cstddef>
#include <cstring>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Ampr/include/AmprPackets.hpp"

namespace {

using AmprCommandBuffer = AmprPackets::CommandBuffer;

constexpr int AMPR_ERROR_PERMISSION = static_cast<int>(0x80020001);
constexpr int AMPR_ERROR_BUSY = static_cast<int>(0x80020010);
constexpr int AMPR_ERROR_INVALID = static_cast<int>(0x80020016);
constexpr std::uint32_t kMaxBufferSize = 0x4000000;
constexpr std::uint32_t kMaxCommands = 0x7fffffff;

int reserve(AmprCommandBuffer* cb, std::uint32_t size, std::uint32_t** out) {
    if (!cb) return AMPR_ERROR_INVALID;
    if (!cb->buffer) return AMPR_ERROR_PERMISSION;
    if (cb->bufferSize < size || cb->bufferSize - size < cb->currentOffset) return AMPR_ERROR_BUSY;
    if (cb->numCommands >= kMaxCommands) return AMPR_ERROR_INVALID;
    *out = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(cb->buffer) + cb->currentOffset);
    return 0;
}

void commit(AmprCommandBuffer* cb, std::uint32_t size, std::uint32_t flags) {
    cb->currentOffset += size;
    cb->numCommands += 1;
    cb->flags |= flags;
}

}

extern "C" {

int APS5_VABI sceAmprCommandBufferConstructor(AmprCommandBuffer* cb) {
    if (cb) *cb = AmprCommandBuffer{};
    return 0;
}

int APS5_VABI sceAmprCommandBufferDestructor(AmprCommandBuffer*) {
    return 0;
}

int APS5_VABI sceAmprCommandBufferSetBuffer(AmprCommandBuffer* cb, void* buffer, uint32_t size) {
    if (!cb || !buffer) return AMPR_ERROR_INVALID;
    if (cb->buffer) return AMPR_ERROR_BUSY;
    if ((reinterpret_cast<std::uintptr_t>(buffer) & 3) != 0 || size == 0 || (size & 3) != 0 || size > kMaxBufferSize) return AMPR_ERROR_INVALID;
    cb->buffer = buffer;
    cb->bufferSize = size;
    return 0;
}

int APS5_VABI sceAmprCommandBufferReset(AmprCommandBuffer* cb) {
    if (!cb || !cb->buffer) return AMPR_ERROR_PERMISSION;
    cb->currentOffset = 0;
    cb->numCommands = 0;
    return 0;
}

void* APS5_VABI sceAmprCommandBufferClearBuffer(AmprCommandBuffer* cb) {
    if (cb->bufferSize == 0 || !cb->buffer) return nullptr;
    void* previous = cb->buffer;
    cb->buffer = nullptr;
    cb->bufferSize = 0;
    return previous;
}

uint32_t APS5_VABI sceAmprCommandBufferGetSize(const AmprCommandBuffer* cb) {
    return cb->bufferSize;
}

uint32_t APS5_VABI sceAmprCommandBufferGetCurrentOffset(const AmprCommandBuffer* cb) {
    return cb->currentOffset;
}

uint32_t APS5_VABI sceAmprCommandBufferGetNumCommands(const AmprCommandBuffer* cb) {
    return cb->numCommands;
}

int64_t APS5_VABI sceAmprCommandBufferGetType(const AmprCommandBuffer* cb) {
    return static_cast<int64_t>(static_cast<int32_t>(cb->flags));
}

void* APS5_VABI sceAmprCommandBufferGetBufferBaseAddress(const AmprCommandBuffer* cb) {
    return cb->buffer;
}

int APS5_VABI sceAmprAprCommandBufferConstructor(AmprCommandBuffer*, uint64_t* gatherState, uint64_t* scatterState) {
    if (gatherState) *gatherState = 0;
    if (scatterState) *scatterState = 0;
    return 0;
}

int APS5_VABI sceAmprAprCommandBufferDestructor(AmprCommandBuffer*, uint64_t*, uint64_t*) {
    return 0;
}

int APS5_VABI sceAmprAprCommandBufferReadFile(AmprCommandBuffer* cb, uint64_t*, uint64_t*, uint32_t file_id, void* destination, uint64_t size, uint64_t file_offset) {
    const auto address = reinterpret_cast<std::uint64_t>(destination);
    if (!AmprPackets::ReadFileArgumentsValid(address, size, file_offset)) return AMPR_ERROR_INVALID;
    const std::uint32_t packetSize = AmprPackets::ReadFileSize(file_offset);
    std::uint32_t* packet = nullptr;
    const int result = reserve(cb, packetSize, &packet);
    if (result != 0) return result;
    AmprPackets::EncodeReadFile(packet, {file_id, address, size, file_offset});
    commit(cb, packetSize, AmprPackets::kFlagReadFile);
    return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeReadFile(uint32_t, void* destination, uint64_t size, uint64_t file_offset) {
    if (!AmprPackets::ReadFileArgumentsValid(reinterpret_cast<std::uint64_t>(destination), size, file_offset)) return AMPR_ERROR_INVALID;
    return static_cast<int>(AmprPackets::ReadFileSize(file_offset));
}

int APS5_VABI sceAmprCommandBufferWriteKernelEventQueue_04_00(AmprCommandBuffer* cb, KernelEqueue eq, int32_t id, uint64_t data, uint32_t flag) {
    if (eq == 0) return AMPR_ERROR_INVALID;
    if (flag == 0 && cb && (cb->flags & AmprPackets::kFlagMapBlock) != 0) return AMPR_ERROR_PERMISSION;
    std::uint32_t* packet = nullptr;
    const int result = reserve(cb, AmprPackets::kWriteKernelEventQueueSize, &packet);
    if (result != 0) return result;
    AmprPackets::EncodeWriteKernelEventQueue(packet, {static_cast<std::uint64_t>(eq), id, data}, flag);
    commit(cb, AmprPackets::kWriteKernelEventQueueSize, 0);
    return 0;
}

int APS5_VABI sceAmprCommandBufferWriteKernelEventQueueOnCompletion(AmprCommandBuffer* cb, KernelEqueue eq, int32_t id, uint64_t data) {
    return sceAmprCommandBufferWriteKernelEventQueue_04_00(cb, eq, id, data, 0);
}

int APS5_VABI sceAmprMeasureCommandSizeWriteKernelEventQueue_04_00(KernelEqueue, int32_t, uint64_t, uint32_t) {
    return static_cast<int>(AmprPackets::kWriteKernelEventQueueSize);
}

}
