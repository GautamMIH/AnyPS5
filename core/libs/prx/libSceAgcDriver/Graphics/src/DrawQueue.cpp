#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <algorithm>

namespace AgcDriver::Graphics {

DrawQueue::~DrawQueue() {
    recording.commands.reset();
    for (auto& batch : pending) batch.commands.reset();
    markers.clear();
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

void DrawQueue::WriteIndex::Add(std::uint64_t begin, std::uint64_t end) {
    if (begin >= end) return;
    ranges.emplace(begin, end);
    longest = std::max(longest, end - begin);
}

void DrawQueue::WriteIndex::Remove(std::uint64_t begin, std::uint64_t end) {
    if (begin >= end) return;
    auto [first, last] = ranges.equal_range(begin);
    const auto found = std::find_if(first, last, [&](const auto& range) { return range.second == end; });
    Require(found != last, "draw queue write index is missing a range");
    ranges.erase(found);
    if (ranges.empty()) longest = 0;
}

bool DrawQueue::WriteIndex::Overlaps(std::uint64_t address, std::size_t bytes) const {
    if (ranges.empty() || bytes == 0) return false;
    // Ranges beginning before the end of the query, walking back while one could still reach it.
    auto it = ranges.lower_bound(address + bytes);
    while (it != ranges.begin()) {
        --it;
        if (it->second > address) return true;
        if (address - it->first >= longest) return false;
    }
    return false;
}

void DrawQueue::Enqueue(std::shared_ptr<ShaderResources> resources, std::shared_ptr<void> storage) {
    Require(recording.commands != nullptr && resources != nullptr && storage != nullptr, "draw batch is incomplete");
    Entry entry{std::move(storage), std::move(resources), {}};
    entry.resources->AppendWrites(entry.writes);
    for (const auto& [begin, end] : entry.writes) writes.Add(begin, end);
    recording.entries.push_back(std::move(entry));
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
    if (!writes.Overlaps(address, bytes)) return;
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
