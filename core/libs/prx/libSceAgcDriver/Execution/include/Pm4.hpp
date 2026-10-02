#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PM4_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PM4_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4Opcodes.hpp"
#include <array>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Pm4 {

// A DMA_DATA between guest memory ranges: a copy, or a fill with an immediate dword (immediate).
struct DmaCopy {
    std::uint64_t source;
    std::uint64_t destination;
    std::uint64_t bytes;
    bool immediate;
    std::uint32_t value;
};
// The copy a validated DMA_DATA performs; nothing for the global data share forms.
std::optional<DmaCopy> DecodeDmaCopy(std::span<const std::uint32_t> packet);

struct DrawParameters {
    std::uint64_t indexAddress;
    std::uint32_t indexCount;
    std::uint32_t indexSize;
    std::uint32_t instanceCount;
    std::uint32_t flags;
    bool indexed = true;
    std::uint32_t firstVertex = 0;
    std::uint32_t firstInstance = 0;
};

// One draw of an indirect packet: the SH registers the command processor patches with its
// arguments (start vertex/index/instance, draw index), then the draw itself.
struct IndirectDraw {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> registers;
    DrawParameters parameters;
};

std::string Name(std::uint32_t header);
std::string_view UnsupportedReason(std::uint32_t header);
void Validate(std::span<const std::uint32_t> packet, std::uint32_t queue);
void Execute(std::span<const std::uint32_t> packet, QueueState& queue);
bool AccessesMemory(std::uint32_t header);
// Evaluates a memory WAIT_REG_MEM (32- or 64-bit) against current guest memory.
bool WaitSatisfied(std::span<const std::uint32_t> packet);
// A conditional INDIRECT_BUFFER (14 dwords, libSceAgc CbBranch) jumps to its then buffer when
// (*compare & mask) <function> reference holds, else (mode 2) to its else buffer; mode 1 falls
// through. The jump does not return (KytyPS5 CpOpBranch).
struct BranchTarget {
    std::uint64_t address;
    std::uint32_t dwords;
};
std::optional<BranchTarget> ResolveBranch(std::span<const std::uint32_t> packet);
std::string DescribeWait(std::span<const std::uint32_t> packet);
// GPU reference clock used for timestamp writes, in 100 MHz ticks.
std::uint64_t GpuClock();
bool UsesGpuCacheBarrier(std::span<const std::uint32_t> packet);
std::array<std::uint32_t, 5> ResolveDispatch(std::span<const std::uint32_t> packet, const QueueState& queue);
DrawParameters ResolveDraw(std::span<const std::uint32_t> packet, const QueueState& queue);
// Reads the arguments of DRAW_INDIRECT, DRAW_INDEX_INDIRECT and their MULTI forms from guest memory.
std::vector<IndirectDraw> ResolveIndirectDraws(std::span<const std::uint32_t> packet, const QueueState& queue);

}

#endif
