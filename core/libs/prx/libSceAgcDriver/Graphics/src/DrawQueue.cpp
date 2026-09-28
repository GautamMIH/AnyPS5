#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <algorithm>

namespace AgcDriver::Graphics {

DrawQueue::~DrawQueue() {
    recording.commands.reset();
    for (auto& batch : pending) batch.commands.reset();
}

VkCommandBuffer DrawQueue::Begin(const Context& context) {
    Collect();
    if (drawCount >= 64) Wait();
    if (!recording.commands) {
        if (available.empty()) recording.commands = std::make_unique<CommandBatch>(context);
        else {
            recording.commands = std::move(available.back());
            available.pop_back();
            recording.commands->Reset();
        }
    }
    if (barrierRequested) {
        barrierRequested = false;
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(recording.commands->Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    return recording.commands->Handle();
}

void DrawQueue::Enqueue(std::shared_ptr<ShaderResources> resources, std::shared_ptr<void> storage) {
    Require(recording.commands != nullptr && resources != nullptr && storage != nullptr, "draw batch is incomplete");
    recording.entries.push_back({std::move(storage), std::move(resources)});
    ++drawCount;
    if (recording.entries.size() >= 8) Flush();
}

void DrawQueue::Flush() {
    if (!recording.commands) return;
    Require(!recording.entries.empty() || recording.hasBarrier, "cannot submit an incomplete draw batch");
    pending.push_back(std::move(recording));
    recording = Batch{};
    pending.back().commands->Submit();
}

void DrawQueue::Resolve(std::uint64_t address, std::size_t bytes) {
    const auto overlaps = [&](const auto& entry) { return entry.resources->WritesOverlap(address, bytes); };
    if (!std::any_of(recording.entries.begin(), recording.entries.end(), overlaps) && !std::any_of(pending.begin(), pending.end(), [&](const auto& batch) { return std::any_of(batch.entries.begin(), batch.entries.end(), overlaps); })) return;
    PerformanceTimer timing("Graphics.DrawQueue.Resolve");
    // Queued draws run in submission order on one queue, so a barrier before the accessing draw
    // makes their writes visible to it; only CPU accesses have to wait for them.
    if (GuestMemory::GpuAccessScope::Active()) {
        barrierRequested = true;
        timing.Mark("gpu_barrier");
        return;
    }
    Wait();
    timing.Mark(GuestMemory::AccessSite::Current());
}


}
