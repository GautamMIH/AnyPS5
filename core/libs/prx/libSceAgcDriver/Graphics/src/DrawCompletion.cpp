#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"

namespace AgcDriver::Graphics {

void DrawQueue::retire(Batch batch) {
    PerformanceTimer timing("Graphics.DrawQueue.Retire");
    const GuestMemory::MemoryAccessScope suspended(nullptr, nullptr);
    Require(drawCount >= batch.entries.size(), "draw queue completion count underflow");
    drawCount -= batch.entries.size();
    for (auto& entry : batch.entries)
        if (entry.resources) entry.resources->WriteBack();
    timing.Mark("resources_writeback");
    if (batch.query >= 0) {
        // The batch completed, so its query result is available.
        std::uint64_t passed = 0;
        const auto query = static_cast<std::uint32_t>(batch.query);
        Check(samples->context.Function<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(samples->context.device, samples->pool, query, 1, sizeof(passed), &passed, sizeof(passed), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults");
        samplesPassed += passed;
        samples->free.push_back(query);
        timing.Mark("sample_count");
    }
    // Written back: later accesses no longer depend on this batch.
    for (const auto& entry : batch.entries)
        for (const auto& [begin, end] : entry.writes) writes.Remove(begin, end);
    batch.entries.clear();
    available.push_back(std::move(batch.commands));
    timing.Mark("resources_release");
}

void DrawQueue::Collect() {
    while (!pending.empty() && pending.front().commands->IsComplete()) {
        auto batch = std::move(pending.front());
        pending.erase(pending.begin());
        retire(std::move(batch));
    }
}

void DrawQueue::WaitGpu() {
    Flush();
    for (auto& batch : pending) batch.commands->Wait();
}

void DrawQueue::Wait() {
    if (pending.empty() && !recording.commands) return;
    PerformanceTimer timing("Graphics.DrawQueue.Wait");
    Flush();
    timing.Mark("submit");
    while (!pending.empty()) {
        pending.front().commands->Wait();
        timing.Mark("fence_wait");
        auto batch = std::move(pending.front());
        pending.erase(pending.begin());
        retire(std::move(batch));
        timing.Mark("retire");
    }
}

void DrawQueue::RecordMemoryBarrier(const Context& context) {
    const auto commands = Begin(context);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    recording.hasBarrier = true;
}

std::uint64_t DrawQueue::SubmitMarker(const Context& context) {
    Flush();
    std::unique_ptr<CommandBatch> commands;
    if (available.empty()) {
        commands = std::make_unique<CommandBatch>(context);
    } else {
        commands = std::move(available.back());
        available.pop_back();
        commands->Reset();
    }
    commands->Label("marker");
    commands->Submit();
    const auto serial = nextMarker++;
    markers.push_back({serial, std::move(commands)});
    return serial;
}

void DrawQueue::collectMarkers(bool wait, std::uint64_t serial) {
    while (!markers.empty() && markers.front().serial <= serial) {
        auto& front = markers.front();
        if (!front.commands->IsComplete()) {
            if (!wait) break;
            front.commands->Wait();
        }
        reachedMarker = front.serial;
        available.push_back(std::move(front.commands));
        markers.pop_front();
    }
    // Draw batches submitted before a reached marker completed before it.
    Collect();
}

bool DrawQueue::MarkerReached(std::uint64_t serial) {
    collectMarkers(false, serial);
    return reachedMarker >= serial;
}

void DrawQueue::WaitMarker(std::uint64_t serial) {
    collectMarkers(true, serial);
    Require(reachedMarker >= serial, "draw queue marker was never submitted");
}

}
