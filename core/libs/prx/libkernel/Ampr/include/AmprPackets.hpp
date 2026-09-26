#ifndef CORE_LIBS_PRX_LIBKERNEL_AMPR_INCLUDE_AMPRPACKETS_HPP
#define CORE_LIBS_PRX_LIBKERNEL_AMPR_INCLUDE_AMPRPACKETS_HPP

#include <cstdint>
#include <cstring>

namespace AmprPackets {

struct CommandBuffer {
    std::uint32_t flags;
    std::uint32_t currentOffset;
    std::uint32_t numCommands;
    std::uint32_t bufferSize;
    void* buffer;
};

inline constexpr std::uint32_t kFlagReadFile = 0x10000;
inline constexpr std::uint32_t kFlagMapBlock = 0x20000;
inline constexpr std::uint32_t kOpcodeMask = 0xff;
inline constexpr std::uint32_t kOpcodeReadFile = 0x28;
inline constexpr std::uint32_t kOpcodeWriteKernelEventQueue = 0x08;
inline constexpr std::uint32_t kOpcodeWriteKernelEventQueueFlagged = 0x78;
inline constexpr std::uint32_t kReadFileShortSize = 20;
inline constexpr std::uint32_t kReadFileLongSize = 24;
inline constexpr std::uint32_t kWriteKernelEventQueueSize = 20;
inline constexpr std::uint64_t kMaxReadSize = 0x100000000ull;
inline constexpr std::uint64_t kAddressLimit = 0xF00000000000ull;
inline constexpr std::uint64_t kFileOffsetLimit = 1ull << 40;
inline constexpr std::uint32_t kFileIdMask = 0x7fffffff;

struct ReadFile {
    std::uint32_t FileId;
    std::uint64_t Destination;
    std::uint64_t Size;
    std::uint64_t FileOffset;
};

struct WriteKernelEventQueue {
    std::uint64_t Queue;
    std::int32_t Id;
    std::uint64_t Data;
};

inline bool ReadFileArgumentsValid(std::uint64_t destination, std::uint64_t size, std::uint64_t fileOffset) {
    return size >= 1 && size <= kMaxReadSize && destination <= kAddressLimit && size <= kAddressLimit - destination && fileOffset < kFileOffsetLimit;
}

inline std::uint32_t ReadFileSize(std::uint64_t fileOffset) {
    return fileOffset >= (1ull << 32) ? kReadFileLongSize : kReadFileShortSize;
}

inline std::uint32_t PacketSize(std::uint32_t header) {
    return (((header >> 8) & 7) + 1) * 4;
}

inline void EncodeReadFile(std::uint32_t* out, const ReadFile& packet) {
    const std::uint32_t dwords = ReadFileSize(packet.FileOffset) / 4;
    out[0] = kOpcodeReadFile | (((dwords + 7) & 0xf) << 8) | static_cast<std::uint32_t>((packet.FileOffset & 0x3ffff) << 12);
    out[1] = static_cast<std::uint32_t>(packet.Size - 1);
    out[2] = packet.FileId & kFileIdMask;
    out[3] = static_cast<std::uint32_t>(packet.Destination);
    out[4] = (static_cast<std::uint32_t>(packet.FileOffset) & 0xfffc0000u) | static_cast<std::uint32_t>((packet.Destination >> 32) & 0xffff);
    if (dwords == 6)
        out[5] = static_cast<std::uint32_t>((packet.FileOffset >> 32) & 0xff);
}

inline ReadFile DecodeReadFile(const std::uint32_t* in) {
    ReadFile packet{};
    packet.Size = static_cast<std::uint64_t>(in[1]) + 1;
    packet.FileId = in[2] & kFileIdMask;
    packet.Destination = static_cast<std::uint64_t>(in[3]) | (static_cast<std::uint64_t>(in[4] & 0xffff) << 32);
    packet.FileOffset = ((in[0] >> 12) & 0x3ffff) | (in[4] & 0xfffc0000u);
    if (PacketSize(in[0]) == kReadFileLongSize)
        packet.FileOffset |= static_cast<std::uint64_t>(in[5] & 0xff) << 32;
    return packet;
}

inline void EncodeWriteKernelEventQueue(std::uint32_t* out, const WriteKernelEventQueue& packet, std::uint32_t flag) {
    const std::uint32_t opcode = flag != 0 ? kOpcodeWriteKernelEventQueueFlagged : kOpcodeWriteKernelEventQueue;
    out[0] = opcode | 0x400 | static_cast<std::uint32_t>((packet.Queue >> 16) & 0xffff0000u);
    out[1] = static_cast<std::uint32_t>(packet.Queue);
    out[2] = static_cast<std::uint32_t>(packet.Id);
    out[3] = static_cast<std::uint32_t>(packet.Data);
    out[4] = static_cast<std::uint32_t>(packet.Data >> 32);
}

inline WriteKernelEventQueue DecodeWriteKernelEventQueue(const std::uint32_t* in) {
    WriteKernelEventQueue packet{};
    packet.Queue = static_cast<std::uint64_t>(in[1]) | (static_cast<std::uint64_t>(in[0] >> 16) << 32);
    packet.Id = static_cast<std::int32_t>(in[2]);
    packet.Data = static_cast<std::uint64_t>(in[3]) | (static_cast<std::uint64_t>(in[4]) << 32);
    return packet;
}

}

#endif
