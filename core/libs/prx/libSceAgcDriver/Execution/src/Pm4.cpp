#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/General.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace AgcDriver::Pm4 {
namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::uint64_t address(std::uint32_t low, std::uint32_t high) {
    return low | (static_cast<std::uint64_t>(high) << 32u);
}

std::uint32_t registerOffset(std::uint32_t value) {
    require(value != 0xffffffffu, "indirect register sentinel semantics are not implemented");
    const auto offset = value & ~0x70000000u;
    if (offset > 0xffffu) {
        char what[80];
        std::snprintf(what, sizeof(what), "extended register semantics are not implemented (offset dword 0x%08x)", value);
        throw std::runtime_error(what);
    }
    return offset;
}

Registers& registersFor(QueueState& queue, std::uint32_t opcode) {
    if (opcode == 0x69 || opcode == 0x9f) return queue.context;
    if (opcode == 0x76 || opcode == 0x63) return queue.shader;
    return queue.userConfig;
}

void writeRegister(QueueState& queue, std::uint32_t opcode, std::uint32_t offset, std::uint32_t value) {
    registersFor(queue, opcode).insert_or_assign(offset, value);
    if ((opcode == 0x64 || opcode == 0x79 || opcode == 0x7a) && offset == 0x243) queue.indexType = value & 3u;
}

bool memorySelector(std::uint32_t selector) {
    return selector == 0 || selector == 3;
}

std::uint32_t dmaSource(std::span<const std::uint32_t> packet) {
    return ((packet[1] >> 29u) & 3u) | ((packet[6] >> 24u) & 4u) | ((packet[6] >> 25u) & 8u);
}

std::uint32_t dmaDestination(std::span<const std::uint32_t> packet) {
    return ((packet[1] >> 20u) & 3u) | ((packet[6] >> 25u) & 4u) | ((packet[6] >> 26u) & 8u);
}

void copyMemory(std::uint64_t source, std::uint64_t destination, std::size_t bytes, bool immediate) {
    if (bytes == 0) return;
    GuestMemory::CheckRange(reinterpret_cast<void*>(destination), bytes, 1, true);
    std::vector<std::byte> data(bytes);
    if (immediate) {
        const auto value = static_cast<std::uint32_t>(source);
        for (std::size_t i = 0; i < bytes; ++i) data[i] = static_cast<std::byte>(value >> ((i % 4) * 8));
    } else {
        GuestMemory::Read(source, data);
    }
    GuestMemory::Write(destination, data);
}

// DMA_DATA selector 1: the 64 KiB global data share, shared by every queue.
constexpr std::uint32_t DmaSelectGds = 1;
constexpr std::size_t GdsBytes = 0x10000;

struct GdsStorage {
    std::mutex mutex;
    std::array<std::byte, GdsBytes> bytes{};
};

GdsStorage& Gds() {
    static GdsStorage storage;
    return storage;
}

bool gdsRange(std::uint64_t offset, std::size_t bytes) {
    return offset <= GdsBytes && bytes <= GdsBytes - offset;
}

void dmaData(std::span<const std::uint32_t> packet) {
    const std::size_t bytes = packet[6] & 0x3ffffffu;
    if (bytes == 0) return;
    const bool fromGds = dmaSource(packet) == DmaSelectGds;
    const bool toGds = dmaDestination(packet) == DmaSelectGds;
    if (!fromGds && !toGds) {
        copyMemory(address(packet[2], packet[3]), address(packet[4], packet[5]), bytes, dmaSource(packet) == 2);
        return;
    }
    std::vector<std::byte> data(bytes);
    if (fromGds) {
        auto& gds = Gds();
        std::lock_guard lock(gds.mutex);
        std::copy_n(gds.bytes.begin() + packet[2], bytes, data.begin());
    } else if (dmaSource(packet) == 2) {
        for (std::size_t i = 0; i < bytes; ++i) data[i] = static_cast<std::byte>(packet[2] >> ((i % 4) * 8));
    } else {
        GuestMemory::Read(address(packet[2], packet[3]), data);
    }
    if (toGds) {
        auto& gds = Gds();
        std::lock_guard lock(gds.mutex);
        std::copy(data.begin(), data.end(), gds.bytes.begin() + packet[4]);
        return;
    }
    const auto destination = address(packet[4], packet[5]);
    GuestMemory::CheckRange(reinterpret_cast<void*>(destination), bytes, 1, true);
    GuestMemory::Write(destination, data);
}

}

std::optional<DmaCopy> DecodeDmaCopy(std::span<const std::uint32_t> packet) {
    if (packet.size() < 7) return std::nullopt;
    const auto source = dmaSource(packet);
    const auto destination = dmaDestination(packet);
    if (source == DmaSelectGds || destination == DmaSelectGds || !memorySelector(destination)) return std::nullopt;
    const bool immediate = source == 2;
    if (!immediate && !memorySelector(source)) return std::nullopt;
    return DmaCopy{immediate ? 0 : address(packet[2], packet[3]), address(packet[4], packet[5]), packet[6] & 0x3ffffffu, immediate, packet[2]};
}

std::optional<DmaCopy> DecodeWriteData(std::span<const std::uint32_t> packet) {
    if (packet.size() < 5) return std::nullopt;
    const auto selector = (packet[1] >> 8u) & 0xfu;
    if (selector != 1 && selector != 2 && selector != 5) return std::nullopt;
    DmaCopy write{0, address(packet[2], packet[3]), 0, false, 0, {}};
    if ((packet[1] & 0x10000u) != 0) write.data.push_back(packet.back());
    else write.data.assign(packet.begin() + 4, packet.end());
    write.bytes = write.data.size() * 4;
    return write;
}

std::string Name(std::uint32_t header) {
    const auto opcode = (header >> 8u) & 0xffu;
    if (opcode == 0x10 && (header & 0xfcu) != 0) {
        switch ((header >> 2u) & 0x3fu) {
            case 0x05: return "DRAW_RESET";
            case 0x06: return "WAIT_FLIP_DONE";
            case 0x09: return "DISPATCH_RESET";
            case 0x0b: return "PUSH_MARKER";
            case 0x0c: return "POP_MARKER";
            case 0x14: return "ACQUIRE_MEM_CUSTOM";
            case 0x15: return "WRITE_DATA_CUSTOM";
            case 0x17: return "FLIP";
            case 0x18: return "RELEASE_MEM_CUSTOM";
            case 0x19: return "DMA_DATA_CUSTOM";
            case 0x1a: return "CONTEXT_STATE";
            default: return "UNKNOWN_CUSTOM";
        }
    }
    for (const auto& entry : Opcodes) if (entry.value == opcode) return std::string(entry.name);
    char text[24]{};
    std::snprintf(text, sizeof(text), "UNKNOWN_0x%02x", opcode);
    return text;
}

std::string_view UnsupportedReason(std::uint32_t header) {
    const auto opcode = (header >> 8u) & 0xffu;
    if (opcode == 0x10) {
        switch ((header >> 2u) & 0x3fu) {
            case 0: case 0x06: case 0x09: case 0x0b: case 0x0c: case 0x17: case 0x1a: return {};
            case 0x14: case 0x18: return "guest cache actions and GPU release events are not implemented";
            default: return "custom packet has no implemented contract in the reference dispatch table";
        }
    }
    switch (opcode) {
        case 0x11: case 0x12: case 0x13: case 0x15: case 0x16: case 0x26:
        case 0x27: case 0x2a: case 0x2d: case 0x2f: case 0x35: case 0x37: case 0x40: case 0x42: case 0x46: case 0x50:
        case 0x58: case 0x63: case 0x64: case 0x69: case 0x76: case 0x79: case 0x7a:
        case 0x81: case 0x83: case 0x9f: return {};
        case 0x24: case 0x25: case 0x2c: case 0x38: return {};
        case 0x3a: case 0x8d:
            return "graphics draw, shader stages and guest render-target materialization are not implemented";
        case 0x20: return {};
        case 0x22: return {};
        case 0x3f: return {};
        case 0x33: return "command-buffer branching is not implemented";
        case 0x3c: case 0x93: return {};
        case 0x39:
            return "cooperative command-queue waits are not implemented";
        case 0x59: return {};
        case 0x84: case 0x85: case 0x86: case 0x88:
            return "separate CE/DE execution and counter synchronization are not implemented";
        case 0x49: return {};
        case 0x43: case 0x47: case 0x48:
            return "guest cache actions, GPU events and interrupt delivery are not implemented";
        case 0x8e: return "GPU LOD statistics are not implemented; synthetic results are forbidden";
        case 0x28: case 0x41: case 0x68: case 0x78:
            return "opcode is named but has no handler in the reference dispatch table";
        default: return "opcode is not known in the reference";
    }
}

// COND_EXEC (upstream 9df0de29): the cache policy is defined on compute queues only (MEC).
constexpr std::uint32_t ConditionCachePolicy = 3u << 25u;
constexpr std::uint32_t ConditionalWordsMask = 0x3fffu;

void Validate(std::span<const std::uint32_t> packet, std::uint32_t queue) {
    require(packet.size() >= 2, "truncated PM4 header or payload");
    const auto header = packet[0];
    require((header & 0xc0000000u) == 0xc0000000u, "unsupported PM4 packet type");
    require(packet.size() == ((header >> 16u) & 0x3fffu) + 2u, "invalid PM4 packet size");
    const auto opcode = (header >> 8u) & 0xffu;
    const auto size = [&](std::size_t count) { require(packet.size() == count, "invalid packet size"); };
    const auto graphics = [&] { require(queue == 0, "graphics packet in compute queue"); };
    const auto reason = UnsupportedReason(header);
    if (!reason.empty()) throw std::runtime_error(std::string(reason));
    if (opcode == 0x10) {
        require((header & 3u) == 0, "unsupported NOP header flags");
        switch ((header >> 2u) & 0x3fu) {
            case 0:
                require((packet[1] & 0xffff0000u) != 0x68750000u, "typed user-data and legacy flip markers are not implemented");
                break;
            case 0x09: size(2); break;
            case 0x06: graphics(); size(4); require(packet[3] == 0, "unsupported rendering wait mode"); break;
            case 0x0b: {
                const auto data = std::as_bytes(packet.subspan(1));
                require(std::find(data.begin(), data.end(), std::byte{}) != data.end(), "unterminated marker text");
                break;
            }
            case 0x0c: break;
            case 0x17: graphics(); size(6); break;
            case 0x1a:
                graphics();
                require(packet.size() == 3 || packet.size() == 5, "invalid context-state packet size");
                require(packet[1] <= 3, "unknown context-state operation");
                require(std::all_of(packet.begin() + 2, packet.end(), [](auto value) { return value == 0; }), "context-state trailing fields are not implemented");
                break;
        }
        return;
    }
    // Bit 0 is PREDICATE: the executor skips such packets while SET_PREDICATION's predicate is set.
    if ((header & 0xfeu) != 0 && !(opcode == 0x11 && (header & 0xfeu) == 2))
        throw std::runtime_error("PM4 header flags are not implemented for " + Name(header) + " (header 0x" + [&] { char text[12]{}; std::snprintf(text, sizeof(text), "%08x", header); return std::string(text); }() + ")");
    switch (opcode) {
        case 0x11:
            size(4);
            require(packet[1] == 1 && (packet[2] & 7u) == 0 && packet[3] <= 0xffffu, "unsupported indirect base index, alignment or address bits");
            if ((header & 2u) == 0) graphics();
            break;
        case 0x12: graphics(); size(2); require((packet[1] & ~0xfu) == 0, "unsupported CLEAR_STATE payload bits"); break;
        case 0x13: case 0x2f: graphics(); size(2); break;
        case 0x26: graphics(); size(3); break;
        case 0x2a: graphics(); size(2); require(packet[1] <= 3, "unsupported index-type modifiers"); break;
        case 0x2d:
            graphics();
            size(3);
            require((packet[2] & ~0x20u) == 2u, "unsupported auto draw flags");
            break;
        case 0x27:
            graphics();
            size(6);
            require(packet[3] <= 0xffffu, "unsupported index address bits");
            require(packet[4] <= packet[1], "index count exceeds maximum index size");
            require((packet[5] & ~0x20u) == 0, "unsupported indexed draw flags");
            break;
        case 0x35:
            graphics();
            size(5);
            require(packet[3] <= packet[1], "index count exceeds maximum index size");
            require((packet[4] & ~0x20u) == 0, "unsupported indexed draw flags");
            break;
        // Indirect draws (KytyPS5 CpOpDrawIndirect/CpOpDrawIndirectMulti): DW2 holds the base
        // vertex SGPR (and, indexed, the start index SGPR in bits 16-31), DW3 the start instance
        // SGPR (bit 27 enables the start index SGPR); 0x280 means no SGPR.
        case 0x24: case 0x25: case 0x2c: case 0x38: {
            graphics();
            const bool multi = opcode == 0x2c || opcode == 0x38;
            const bool indexed = opcode == 0x25 || opcode == 0x38;
            size(multi ? 10 : 5);
            require((packet[1] & 3u) == 0, "misaligned indirect draw argument offset");
            require(indexed ? (packet[3] & ~0x0800ffffu) == 0 && ((packet[3] & 0x08000000u) != 0 || (packet[2] >> 16u) == 0) : (packet[2] >> 16u) == 0 && (packet[3] >> 16u) == 0, "unsupported indirect draw patch locations");
            // Source select: 2 (auto index) for DRAW_INDIRECT; 0 (DMA) for the indexed forms, which
            // earlier AnyPS5 builders encoded as 2.
            require((packet.back() & ~0x20u) == 2u || (indexed && (packet.back() & ~0x20u) == 0u), "unsupported indirect draw initiator");
            if (multi) {
                require((packet[4] & ~(opcode == 0x2c ? 0xc000ffffu : 0x4000ffffu)) == 0, "unsupported indirect multi-draw control bits");
                require((packet[8] & 3u) == 0 && packet[8] >= (indexed ? 20u : 16u), "invalid indirect multi-draw stride");
                require((packet[4] & 0x40000000u) == 0 ? packet[6] == 0 && packet[7] == 0 : (packet[6] & 3u) == 0 && address(packet[6], packet[7]) != 0, "invalid indirect multi-draw count address");
            }
            break;
        }
        // Initiator bits accepted besides COMPUTE_SHADER_EN | ORDER_MODE: CS_W32_EN (0x8000) and
        // TUNNEL_ENABLE (0x2000, a wave launch priority hint with no effect on results); direct
        // dispatches also USE_THREAD_DIMENSIONS (0x20: the packet counts threads, Driver::dispatch).
        case 0x15: size(5); require((packet[4] & ~0xa020u) == 0x41u, "dispatch modifiers are not implemented"); break;
        case 0x16:
            require(packet.size() == 3 || packet.size() == 4, "invalid indirect dispatch size");
            require((packet.back() & ~0xa000u) == 0x41u, "indirect dispatch modifiers are not implemented");
            break;
        case 0x42: size(2); require(packet[1] == 0, "unsupported PFP_SYNC_ME payload"); break;
        case 0x22:
            size(5);
            require((packet[1] & 3u) == 0, "COND_EXEC reserved address bits are not implemented");
            require(packet[2] <= 0xffffu, "COND_EXEC address bits above 48 are not implemented");
            require((packet[3] & ~(queue == 0 ? 0u : ConditionCachePolicy)) == 0, "COND_EXEC reserved control fields are not implemented");
            require((packet[4] & ~ConditionalWordsMask) == 0, "COND_EXEC reserved count bits are not implemented");
            break;
        case 0x20: {
            size(4);
            require((packet[1] & ~0x00071100u) == 0, "unsupported SET_PREDICATION fields");
            const auto operation = (packet[1] >> 16u) & 7u;
            require(operation == 0 || operation == 1 || operation == 3 || operation == 4, "only clear, occlusion and boolean predication are implemented");
            require(operation == 0 || ((packet[2] & 0xfu) == 0 && (packet[2] != 0 || packet[3] != 0)), "predication requires an aligned address");
            break;
        }
        case 0x3f:
            if (packet.size() == 14) {
                require((packet[1] & ~0x703u) == 0 && (packet[1] & 3u) != 0 && (packet[1] & 3u) != 3 && ((packet[1] >> 8u) & 7u) <= 6, "unsupported conditional branch mode or compare function");
                require((packet[2] & 7u) == 0 && address(packet[2], packet[3]) != 0, "null or misaligned conditional branch compare address");
                require((packet[8] & 3u) == 0 && (packet[11] & 3u) == 0, "misaligned conditional branch buffer");
                require((packet[10] & ~0x300fffffu) == 0 && (packet[13] & ~0x300fffffu) == 0, "unsupported conditional branch buffer fields");
                break;
            }
            size(4);
            require((packet[1] & 3u) == 0, "misaligned nested command buffer");
            require((packet[3] & 0x0fe00000u) == 0x0f200000u, "unsupported INDIRECT_BUFFER control fields");
            break;
        // REWIND: the CP re-reads the command buffer from this point (for packets the CPU writes
        // while it runs); packets here are read from memory as they are executed (upstream PR #258).
        case 0x59:
            size(2);
            require((packet[1] & 0x7fffffffu) == 0, "unsupported REWIND payload bits");
            break;
        case 0x3c: case 0x93: {
            size(opcode == 0x3c ? 7 : 9);
            require((packet[1] & ~0x060003f7u) == 0, "unsupported WAIT_REG_MEM control fields");
            require((packet[1] & 0x30u) == 0x10u, "register WAIT_REG_MEM is not implemented");
            require((packet[1] & 0xc0u) == 0, "WAIT_REG_MEM write operations are not implemented");
            require((packet[1] & 7u) <= 6, "invalid WAIT_REG_MEM comparison");
            const auto target = address(packet[2], packet[3]);
            require(target != 0 && (target & (opcode == 0x3c ? 3u : 7u)) == 0, "null or misaligned WAIT_REG_MEM address");
            require(packet.back() <= 0xffffu, "invalid WAIT_REG_MEM poll interval");
            break;
        }
        case 0x49: {
            size(8);
            require((packet[1] & ~0x07fff73fu) == 0, "unsupported RELEASE_MEM event fields");
            const auto eventIndex = (packet[1] >> 8u) & 7u;
            require(eventIndex == 5 || eventIndex == 6, "invalid RELEASE_MEM event index");
            require((packet[2] & ~0xe7030000u) == 0, "unsupported RELEASE_MEM control fields");
            const auto interrupt = (packet[2] >> 24u) & 7u;
            const auto dataSelect = packet[2] >> 29u;
            require(interrupt <= 4, "invalid RELEASE_MEM interrupt selector");
            require(dataSelect <= 3, "GDS and system-clock RELEASE_MEM data sources are not implemented");
            require(packet[7] <= 0x7ffffffu, "invalid RELEASE_MEM interrupt context");
            if (dataSelect != 0) require((address(packet[3], packet[4]) & (dataSelect == 1 ? 3u : 7u)) == 0, "misaligned RELEASE_MEM destination");
            break;
        }
        case 0x46: {
            require((packet[1] & ~0x73fu) == 0, "unsupported EVENT_WRITE flags or reserved bits");
            const auto eventType = packet[1] & 0x3fu;
            const auto eventIndex = (packet[1] >> 8u) & 7u;
            switch (eventType) {
                case 0x07: case 0x0f: case 0x10:
                    size(2);
                    require(eventIndex == 4, "invalid partial-flush event index");
                    if (eventType != 0x07) graphics();
                    break;
                case 0x16: case 0x31: case 0x2a: case 0x2c: case 0x2e:
                    graphics();
                    size(2);
                    require(eventIndex == 0 || eventIndex == 7, "invalid cache-flush event index");
                    break;
                case 0x39:
                    // PIXEL_PIPE_STAT_DUMP: the occlusion counters (see Driver::dumpSampleCounters).
                    graphics();
                    size(4);
                    require(eventIndex == 1, "invalid occlusion counter dump event index");
                    require(address(packet[2], packet[3]) != 0 && (packet[2] & 7u) == 0, "null or misaligned occlusion counter dump address");
                    break;
                default: throw std::runtime_error("EVENT_WRITE event type " + std::to_string(eventType) + " is not implemented");
            }
            break;
        }
        case 0x58: {
            require(packet.size() == 7 || packet.size() == 8, "invalid ACQUIRE_MEM packet size");
            const auto controlMask = packet.size() == 8 ? 0x86287fc3u : 0xfeecfffbu;
            require((packet[1] & ~controlMask) == 0, "unsupported ACQUIRE_MEM control flags");
            require(queue == 0 || (packet[1] & 0x06287fc3u) == 0, "graphics cache operation in compute queue");
            require(packet[3] == 0 && packet[5] == 0, "ACQUIRE_MEM ranges above 40 bits are not implemented");
            require(packet[6] <= 0xffffu, "invalid ACQUIRE_MEM poll interval");
            const auto base = static_cast<std::uint64_t>(packet[4]) << 8u;
            const auto bytes = static_cast<std::uint64_t>(packet[2]) << 8u;
            require(bytes <= (1ull << 40u) - base, "ACQUIRE_MEM range exceeds 40-bit address space");
            if (packet.size() == 8) {
                require((packet[7] & ~0x3ffffu) == 0, "unsupported ACQUIRE_MEM GCR flags");
                require((packet[7] & 0x2000u) == 0, "ACQUIRE_MEM cache discard is not implemented");
            }
            break;
        }
        case 0x63: case 0x64: case 0x9f:
            if (opcode != 0x63) graphics();
            size(5);
            require((packet[1] & 3u) == 0 && packet[3] == 0x80000000u && packet[4] <= 0x3fffu, "unsupported indirect-register address or control fields");
            break;
        case 0x69: case 0x76: case 0x79: case 0x7a: {
            require(packet.size() >= 3, "register packet has no values");
            // AGC tags memory waits with writes to its internal data register (UCONFIG 0x342),
            // which has no hardware effect; compute queues carry those tags too.
            const auto waitTag = opcode == 0x79 && registerOffset(packet[1]) == 0x342u;
            if (opcode != 0x76 && queue != 0 && !waitTag) {
                char message[64];
                std::snprintf(message, sizeof(message), "graphics packet in compute queue (register 0x%x)", registerOffset(packet[1]));
                require(false, message);
            }
            if (opcode == 0x7a) require((packet[1] & 0xf0000000u) == 0 || (packet.size() == 3 && packet[1] == 0x20000243u), "indexed register bank selection is not implemented");
            const auto offset = registerOffset(packet[1]);
            require(packet.size() - 2 <= 0x10000u - offset, "register range overflow");
            break;
        }
        case 0x81:
            graphics();
            require(packet[1] <= 0xbffcu && (packet[1] & 3u) == 0 && packet.size() - 2 <= 0x3000u - packet[1] / 4u, "constant RAM write range overflow or misalignment");
            break;
        case 0x83:
            graphics(); size(5);
            require(packet[1] <= 0xbffcu && (packet[1] & 3u) == 0 && packet[2] <= 0x3000u - packet[1] / 4u, "constant RAM dump range overflow or misalignment");
            break;
        case 0x37: {
            require(packet.size() >= 5, "WRITE_DATA has no data");
            // Cache policy (bits 25-26) only steers GPU cache allocation and PFP engine selection (bit 30)
            // only picks which micro-engine performs the same write; neither is observable here.
            if ((packet[1] & ~0x46110f00u) != 0) {
                char text[96]{};
                std::snprintf(text, sizeof(text), "WRITE_DATA engine, cache or reserved fields are not implemented (control 0x%08x)", packet[1]);
                throw std::runtime_error(text);
            }
            const auto destination = (packet[1] >> 8u) & 0xfu;
            require(destination == 1 || destination == 2 || (queue != 0 && destination == 5), "WRITE_DATA register or GDS destination is not implemented");
            require((packet[2] & 3u) == 0, "misaligned WRITE_DATA destination");
            break;
        }
        case 0x40: {
            size(6);
            require((packet[1] & ~0x40110f0fu) == 0, "COPY_DATA engine, cache or reserved fields are not implemented");
            const auto source = ((packet[1] & 0xfu) << 1u) | ((packet[1] >> 30u) & 1u);
            const auto destination = ((packet[1] >> 8u) & 0xfu) << 1u;
            require(destination == 2 || destination == 4, "COPY_DATA register or GDS destination is not implemented");
            require(source == 2 || source == 4 || source == 5 || source == 10 || source == 11, "COPY_DATA register, GDS or reference-clock source is not implemented");
            require(source < 10 || ((packet[1] & 0x10000u) == 0 && packet[3] == 0), "64-bit immediate COPY_DATA is not implemented");
            break;
        }
        case 0x50:
            size(7);
            // Source/destination cache policy (bits 13-14, 25-26) and volatile hints (15, 27) do not
            // change results because copies go through coherent host memory.
            require((packet[1] & ~0xee30e001u) == 0, "DMA_DATA reserved control fields are not implemented");
            require(memorySelector(dmaDestination(packet)) || dmaDestination(packet) == DmaSelectGds, "DMA_DATA register or prefetch destination is not implemented");
            require(memorySelector(dmaSource(packet)) || dmaSource(packet) == 2 || dmaSource(packet) == DmaSelectGds, "DMA_DATA register source is not implemented");
            require(dmaSource(packet) != DmaSelectGds || (packet[3] == 0 && gdsRange(packet[2], packet[6] & 0x3ffffffu)), "DMA_DATA GDS source range exceeds the GDS");
            require(dmaDestination(packet) != DmaSelectGds || (packet[5] == 0 && gdsRange(packet[4], packet[6] & 0x3ffffffu)), "DMA_DATA GDS destination range exceeds the GDS");
            require(dmaSource(packet) != 2 || packet[3] == 0, "DMA_DATA immediate exceeds 32 bits");
            break;
        default: throw std::runtime_error("known packet has no validator");
    }
}

bool UsesGpuCacheBarrier(std::span<const std::uint32_t> packet) {
    require(!packet.empty() && ((packet[0] >> 8u) & 0xffu) == 0x58, "cache barrier requires ACQUIRE_MEM");
    Validate(packet, 0);
    return packet.size() == 8 && (packet[7] & 0xfc00u) == 0;
}

bool WaitSatisfied(std::span<const std::uint32_t> packet) {
    Validate(packet, 0x20);
    const auto wide = ((packet[0] >> 8u) & 0xffu) == 0x93;
    const auto target = address(packet[2], packet[3]);
    std::uint64_t value = 0;
    GuestMemory::Read(target, std::as_writable_bytes(std::span(&value, 1)).first(wide ? 8 : 4), wide ? 8 : 4);
    const auto reference = wide ? address(packet[4], packet[5]) : packet[4];
    const auto mask = wide ? address(packet[6], packet[7]) : packet[5];
    const auto masked = value & mask;
    switch (packet[1] & 7u) {
        case 0: return true;
        case 1: return masked < reference;
        case 2: return masked <= reference;
        case 3: return masked == reference;
        case 4: return masked != reference;
        case 5: return masked >= reference;
        default: return masked > reference;
    }
}

bool ReleaseSatisfiesWait(std::span<const std::uint32_t> wait, std::span<const std::uint32_t> release) {
    if (wait.size() < 6 || release.size() != 8) return false;
    const auto dataSelect = release[2] >> 29u;
    if (dataSelect != 1 && dataSelect != 2) return false;
    const auto wide = ((wait[0] >> 8u) & 0xffu) == 0x93;
    if (wide && (wait.size() < 9 || dataSelect != 2)) return false;
    if (address(wait[2], wait[3]) != address(release[3], release[4])) return false;
    return ValueSatisfiesWait(wait, dataSelect == 1 ? release[5] : address(release[5], release[6]));
}

bool ValueSatisfiesWait(std::span<const std::uint32_t> wait, std::uint64_t value) {
    const auto wide = ((wait[0] >> 8u) & 0xffu) == 0x93;
    const auto reference = wide ? address(wait[4], wait[5]) : wait[4];
    const auto mask = wide ? address(wait[6], wait[7]) : wait[5];
    const auto masked = (wide ? value : value & 0xffffffffu) & mask;
    switch (wait[1] & 7u) {
        case 0: return true;
        case 1: return masked < reference;
        case 2: return masked <= reference;
        case 3: return masked == reference;
        case 4: return masked != reference;
        case 5: return masked >= reference;
        default: return masked > reference;
    }
}

std::size_t ConditionalWords(std::span<const std::uint32_t> packet) {
    require(packet.size() == 5 && ((packet[0] >> 8u) & 0xffu) == 0x22u, "expected COND_EXEC packet");
    return packet[4] & ConditionalWordsMask;
}

std::uint32_t ReadCondition(std::span<const std::uint32_t> packet) {
    require(packet.size() == 5 && ((packet[0] >> 8u) & 0xffu) == 0x22u, "expected COND_EXEC packet");
    std::uint32_t value = 0;
    GuestMemory::Read(address(packet[1], packet[2]), std::as_writable_bytes(std::span(&value, 1)), 4);
    return value;
}

std::optional<BranchTarget> ResolveBranch(std::span<const std::uint32_t> packet) {
    Validate(packet, 0x20);
    require(packet.size() == 14, "expected a conditional INDIRECT_BUFFER");
    std::uint64_t value = 0;
    GuestMemory::Read(address(packet[2], packet[3]), std::as_writable_bytes(std::span(&value, 1)), 8);
    const auto masked = value & address(packet[4], packet[5]);
    const auto reference = address(packet[6], packet[7]);
    bool taken = true;
    switch ((packet[1] >> 8u) & 7u) {
        case 0: taken = true; break;
        case 1: taken = masked < reference; break;
        case 2: taken = masked <= reference; break;
        case 3: taken = masked == reference; break;
        case 4: taken = masked != reference; break;
        case 5: taken = masked >= reference; break;
        default: taken = masked > reference; break;
    }
    if (taken) return BranchTarget{address(packet[8], packet[9]), packet[10] & 0xfffffu};
    if ((packet[1] & 3u) == 2 && (packet[13] & 0xfffffu) != 0) return BranchTarget{address(packet[11], packet[12]), packet[13] & 0xfffffu};
    return std::nullopt;
}

std::string DescribeWait(std::span<const std::uint32_t> packet) {
    const auto wide = ((packet[0] >> 8u) & 0xffu) == 0x93;
    const auto target = address(packet[2], packet[3]);
    std::uint64_t value = 0;
    GuestMemory::Read(target, std::as_writable_bytes(std::span(&value, 1)).first(wide ? 8 : 4), wide ? 8 : 4);
    static constexpr const char* comparisons[] = {"always", "<", "<=", "==", "!=", ">=", ">"};
    char text[160]{};
    std::snprintf(text, sizeof(text), "(value 0x%llx at 0x%llx) & 0x%llx %s 0x%llx",
        static_cast<unsigned long long>(value), static_cast<unsigned long long>(target),
        static_cast<unsigned long long>(wide ? address(packet[6], packet[7]) : packet[5]), comparisons[packet[1] & 7u],
        static_cast<unsigned long long>(wide ? address(packet[4], packet[5]) : packet[4]));
    return text;
}

std::uint64_t GpuClock() {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 10);
}

bool AccessesMemory(std::uint32_t header) {
    switch ((header >> 8u) & 0xffu) {
        case 0x16: case 0x20: case 0x22: case 0x24: case 0x25: case 0x27: case 0x2c: case 0x2d: case 0x35: case 0x38: case 0x37: case 0x3c: case 0x40: case 0x49: case 0x50: case 0x63: case 0x64: case 0x83: case 0x93: case 0x9f: return true;
        default: return false;
    }
}

std::array<std::uint32_t, 5> ResolveDispatch(std::span<const std::uint32_t> packet, const QueueState& queue) {
    std::uint64_t source = 0;
    if (packet.size() == 4) source = address(packet[1], packet[2]);
    else {
        require(queue.dispatchIndirectBase != 0, "indirect dispatch base has not been set");
        require(packet[1] <= std::numeric_limits<std::uint64_t>::max() - queue.dispatchIndirectBase, "indirect dispatch address overflow");
        source = queue.dispatchIndirectBase + packet[1];
    }
    std::array<std::uint32_t, 5> result{0xc0031500u, 0, 0, 0, packet.back()};
    GuestMemory::Read(source, std::as_writable_bytes(std::span(result).subspan(1, 3)), 4);
    return result;
}

DrawParameters ResolveDraw(std::span<const std::uint32_t> packet, const QueueState& queue) {
    Validate(packet, 0);
    if (((packet[0] >> 8u) & 0xffu) == 0x2d) {
        const auto offset = queue.userConfig.find(0x24a);
        require(offset != queue.userConfig.end(), "missing GE_INDX_OFFSET register");
        const auto firstVertex = offset->second;
        require(packet[1] == 0 || firstVertex <= std::numeric_limits<std::uint32_t>::max() - (packet[1] - 1u), "auto draw vertex range overflow");
        return {0, packet[1], 0, queue.instanceCount, packet[2] & 0x20u, false, firstVertex, 0};
    }
    const auto opcode = (packet[0] >> 8u) & 0xffu;
    require(opcode == 0x35 || opcode == 0x27, "expected DRAW_INDEX_OFFSET_2 or DRAW_INDEX_2 packet");
    require(queue.indexType <= 2, "unsupported index type");
    const std::uint32_t indexSize = queue.indexType == 0 ? 2 : queue.indexType == 1 ? 4 : 1;
    std::uint64_t indexAddress = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t initiator = 0;
    if (opcode == 0x27) {
        // DRAW_INDEX_2 carries its own index buffer address instead of an offset into INDEX_BASE.
        indexAddress = address(packet[2], packet[3]);
        require(indexAddress != 0 && indexAddress % indexSize == 0, "null or misaligned index buffer");
        indexCount = packet[4];
        initiator = packet[5];
    } else {
        require(queue.indexBase != 0 && queue.indexBase % indexSize == 0, "null or misaligned index base");
        const auto offset = static_cast<std::uint64_t>(packet[2]) * indexSize;
        require(offset <= std::numeric_limits<std::uint64_t>::max() - queue.indexBase, "index address overflow");
        indexAddress = queue.indexBase + offset;
        indexCount = packet[3];
        initiator = packet[4];
    }
    const auto bytes = static_cast<std::uint64_t>(indexCount) * indexSize;
    require(bytes <= std::numeric_limits<std::size_t>::max(), "index range size overflow");
    GuestMemory::CheckRange(reinterpret_cast<const void*>(indexAddress), static_cast<std::size_t>(bytes), indexSize);
    // The GE adds GE_INDX_OFFSET to every index it fetches: the base vertex (upstream f4143d61).
    const auto indexOffset = queue.userConfig.find(0x24a);
    require(indexOffset != queue.userConfig.end(), "missing GE_INDX_OFFSET register");
    return {indexAddress, indexCount, indexSize, queue.instanceCount, initiator, true, indexOffset->second, 0};
}

std::vector<IndirectDraw> ResolveIndirectDraws(std::span<const std::uint32_t> packet, const QueueState& queue) {
    Validate(packet, 0);
    const auto opcode = (packet[0] >> 8u) & 0xffu;
    const bool multi = opcode == 0x2c || opcode == 0x38;
    const bool indexed = opcode == 0x25 || opcode == 0x38;
    require(queue.drawIndirectBase != 0, "indirect draw base has not been set");
    constexpr std::uint32_t noRegister = 0x280;
    const auto baseVertexRegister = packet[2] & 0xffffu;
    const auto startIndexRegister = indexed && (packet[3] & 0x08000000u) != 0 ? packet[2] >> 16u : noRegister;
    const auto startInstanceRegister = packet[3] & 0xffffu;
    // DRAW_INDIRECT_MULTI enables its draw-index SGPR with bit 31; the indexed form always carries a
    // location, where 0x280 (or 0, from earlier AnyPS5 builders) means none.
    const auto indexedDrawIndex = packet[4] & 0xffffu;
    const auto drawIndexRegister = opcode == 0x2c ? ((packet[4] & 0x80000000u) != 0 ? packet[4] & 0xffffu : noRegister)
        : opcode == 0x38 && indexedDrawIndex != 0 ? indexedDrawIndex : noRegister;
    std::uint32_t count = 1;
    std::uint32_t stride = 0;
    if (multi) {
        count = packet[5];
        stride = packet[8];
        if ((packet[4] & 0x40000000u) != 0) {
            std::uint32_t stored = 0;
            GuestMemory::Read(address(packet[6], packet[7]), std::as_writable_bytes(std::span(&stored, 1)), 4);
            count = std::min(count, stored);
        }
    }
    std::uint32_t indexSize = 0;
    if (indexed) {
        require(queue.indexType <= 2, "unsupported index type");
        indexSize = queue.indexType == 0 ? 2 : queue.indexType == 1 ? 4 : 1;
        require(queue.indexBase != 0 && queue.indexBase % indexSize == 0, "null or misaligned index base");
    }
    std::vector<IndirectDraw> draws;
    draws.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto offset = static_cast<std::uint64_t>(packet[1]) + static_cast<std::uint64_t>(i) * stride;
        require(offset <= std::numeric_limits<std::uint64_t>::max() - queue.drawIndirectBase, "indirect draw address overflow");
        // {count, instances, start vertex or index, [base vertex,] start instance}
        std::array<std::uint32_t, 5> arguments{};
        GuestMemory::Read(queue.drawIndirectBase + offset, std::as_writable_bytes(std::span(arguments).first(indexed ? 5 : 4)), 4);
        IndirectDraw draw;
        const auto patch = [&](std::uint32_t location, std::uint32_t value) {
            if (location != noRegister) draw.registers.emplace_back(location, value);
        };
        patch(baseVertexRegister, indexed ? arguments[3] : arguments[2]);
        patch(startInstanceRegister, indexed ? arguments[4] : arguments[3]);
        patch(startIndexRegister, arguments[2]);
        patch(drawIndexRegister, i);
        // Base vertex and start instance reach vertices only through those SGPRs, as on hardware.
        if (indexed) {
            // INDEX_BUFFER_SIZE bounds the indices a draw may read (KytyPS5).
            const auto indexCount = queue.indexBufferSize != 0 ? std::min(arguments[0], queue.indexBufferSize) : arguments[0];
            const auto start = static_cast<std::uint64_t>(arguments[2]) * indexSize;
            require(start <= std::numeric_limits<std::uint64_t>::max() - queue.indexBase, "index address overflow");
            draw.parameters = {queue.indexBase + start, indexCount, indexSize, arguments[1], packet.back() & 0x20u};
            if (indexCount != 0) GuestMemory::CheckRange(reinterpret_cast<const void*>(draw.parameters.indexAddress), static_cast<std::size_t>(static_cast<std::uint64_t>(indexCount) * indexSize), indexSize);
        } else {
            draw.parameters = {0, arguments[0], 0, arguments[1], packet.back() & 0x20u, false};
        }
        draws.push_back(std::move(draw));
    }
    return draws;
}

void Execute(std::span<const std::uint32_t> packet, QueueState& queue) {
    const auto opcode = (packet[0] >> 8u) & 0xffu;
    switch (opcode) {
        case 0x10:
            switch ((packet[0] >> 2u) & 0x3fu) {
                case 0: return;
                case 0x09: queue = QueueState{}; return;
                case 0x0b: queue.markers.emplace_back(reinterpret_cast<const char*>(packet.data() + 1)); return;
                case 0x0c:
                    require(!queue.markers.empty(), "marker stack underflow");
                    queue.markers.pop_back();
                    return;
                case 0x1a:
                    switch (packet[1]) {
                        case 0: queue.ClearContext(); break;
                        case 1: case 3:
                            require(!queue.savedContext.has_value(), "context state is already pushed");
                            queue.savedContext = queue.context;
                            if (packet[1] == 3) queue.ClearContext();
                            break;
                        case 2:
                            require(queue.savedContext.has_value(), "context state has not been pushed");
                            queue.context = std::move(*queue.savedContext);
                            queue.savedContext.reset();
                            break;
                    }
                    return;
                default: throw std::runtime_error("custom packet requires driver execution");
            }
        case 0x11:
            ((packet[0] & 2u) == 0 ? queue.drawIndirectBase : queue.dispatchIndirectBase) = address(packet[2], packet[3]);
            return;
        case 0x12:
            queue.ClearContext(); return;
        case 0x20: {
            // Predicate semantics follow KytyPS5: condition 0 skips when the value is non-zero,
            // condition 1 when it is zero.
            const auto flags = packet[1];
            const auto operation = (flags >> 16u) & 7u;
            if (operation == 0) {
                queue.predicateSkip = false;
                return;
            }
            const auto source = address(packet[2] & ~0xfu, packet[3]);
            std::uint64_t value = 0;
            if (operation == 3 || operation == 4) {
                // Boolean predication: a 64-bit (3) or 32-bit (4) value (upstream 124be3e7).
                GuestMemory::Read(source, std::as_writable_bytes(std::span(&value, 1)).first(operation == 3 ? 8 : 4), operation == 3 ? 8 : 4);
            } else {
                // Occlusion: one begin/end pair of ZPASS counters per depth block, each marked
                // ready by bit 63.
                std::array<std::uint64_t, 32> counters{};
                GuestMemory::Read(source, std::as_writable_bytes(std::span(counters)), 8);
                constexpr std::uint64_t ready = 1ull << 63u;
                for (std::size_t block = 0; block < 16; ++block) {
                    require((counters[block * 2] & counters[block * 2 + 1] & ready) != 0, "occlusion predication results are not available (occlusion queries are not implemented)");
                    value += (counters[block * 2 + 1] & ~ready) - (counters[block * 2] & ~ready);
                }
            }
            queue.predicateSkip = ((flags >> 8u) & 1u) == 0 ? value != 0 : value == 0;
            return;
        }
        case 0x59: return;
        case 0x13: queue.indexBufferSize = packet[1]; return;
        case 0x26: queue.indexBase = address(packet[1], packet[2]); return;
        case 0x2a: queue.indexType = packet[1]; return;
        case 0x2f: queue.instanceCount = packet[1]; return;
        case 0x63: case 0x64: case 0x9f: {
            std::vector<std::uint32_t> pairs(static_cast<std::size_t>(packet[4]) * 2);
            GuestMemory::Read(address(packet[1], packet[2]), std::as_writable_bytes(std::span(pairs)), 4);
            for (std::size_t i = 0; i < pairs.size(); i += 2) registerOffset(pairs[i]);
            for (std::size_t i = 0; i < pairs.size(); i += 2) writeRegister(queue, opcode, registerOffset(pairs[i]), pairs[i + 1]);
            return;
        }
        case 0x69: case 0x76: case 0x79: case 0x7a: {
            const auto offset = registerOffset(packet[1]);
            for (std::size_t i = 2; i < packet.size(); ++i) writeRegister(queue, opcode, offset + static_cast<std::uint32_t>(i - 2), packet[i]);
            return;
        }
        case 0x81:
            std::copy(packet.begin() + 2, packet.end(), queue.constantRam.begin() + packet[1] / 4);
            return;
        case 0x83:
            GuestMemory::Write(address(packet[3], packet[4]), std::as_bytes(std::span(queue.constantRam).subspan(packet[1] / 4, packet[2])), 4);
            return;
        case 0x37: {
            const auto destination = address(packet[2], packet[3]);
            if ((packet[1] & 0x10000u) != 0) {
                for (const auto& value : packet.subspan(4)) GuestMemory::Write(destination, std::as_bytes(std::span(&value, 1)), 4);
            } else GuestMemory::Write(destination, std::as_bytes(packet.subspan(4)), 4);
            return;
        }
        case 0x40: {
            const auto source = ((packet[1] & 0xfu) << 1u) | ((packet[1] >> 30u) & 1u);
            copyMemory(address(packet[2], packet[3]), address(packet[4], packet[5]), (packet[1] & 0x10000u) != 0 ? 8 : 4, source >= 10);
            return;
        }
        case 0x50:
            dmaData(packet);
            return;
        case 0x49: {
            const auto dataSelect = packet[2] >> 29u;
            const auto destination = address(packet[3], packet[4]);
            if (dataSelect == 1) GuestMemory::Write(destination, std::as_bytes(packet.subspan(5, 1)), 4);
            else if (dataSelect == 2 || dataSelect == 3) {
                const std::uint64_t value = dataSelect == 3 ? GpuClock() : address(packet[5], packet[6]);
                GuestMemory::Write(destination, std::as_bytes(std::span(&value, 1)), 8);
            }
            return;
        }
        default: throw std::runtime_error("packet requires driver execution");
    }
}

}
