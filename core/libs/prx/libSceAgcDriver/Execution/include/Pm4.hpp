#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PM4_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PM4_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4Opcodes.hpp"
#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Pm4 {

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
