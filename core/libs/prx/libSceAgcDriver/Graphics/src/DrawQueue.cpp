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
        // Every query is in flight with a pending batch: retire them first.
        if (samples && samples->free.empty()) Wait();
        if (available.empty()) recording.commands = std::make_unique<CommandBatch>(context);
        else {
            recording.commands = std::move(available.back());
            available.pop_back();
            recording.commands->Reset();
        }
        recording.commands->Label("draws_and_dispatches");
        if (samples) {
            // Draws open and close their render passes, so the query spans the batch outside them.
            recording.query = static_cast<std::int32_t>(samples->free.back());
            samples->free.pop_back();
            const auto commands = recording.commands->Handle();
            context.Function<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(commands, samples->pool, static_cast<std::uint32_t>(recording.query), 1);
            context.Function<PFN_vkCmdBeginQuery>("vkCmdBeginQuery")(commands, samples->pool, static_cast<std::uint32_t>(recording.query), samples->precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0u);
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

void DrawQueue::NoteGuestWrite(std::uint64_t begin, std::uint64_t end, std::shared_ptr<void> storage) {
    Require(recording.commands != nullptr && storage != nullptr && begin < end, "guest write is not recorded in a draw batch");
    Entry entry{std::move(storage), nullptr, {{begin, end}}};
    writes.Add(begin, end);
    recording.entries.push_back(std::move(entry));
    ++drawCount;
}

void DrawQueue::Flush() {
    // A batch may hold only commands recorded ahead of draws that did not follow (mirror copies,
    // write-backs): it is submitted all the same.
    if (!recording.commands) return;
    if (recording.query >= 0) samples->context.Function<PFN_vkCmdEndQuery>("vkCmdEndQuery")(recording.commands->Handle(), samples->pool, static_cast<std::uint32_t>(recording.query));
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
    // A CPU access waits for the batches that write the range, not the whole queue: completed
    // batches are retired first (labels a DMA reset are usually done by the time a WAIT_REG_MEM
    // reads them), then batches are waited for in order up to the last one writing the range.
    Collect();
    if (!writes.Overlaps(address, bytes)) {
        timing.Mark("retired");
        return;
    }
    const auto end = address + bytes;
    const auto writesRange = [&](const Batch& batch) {
        for (const auto& entry : batch.entries)
            for (const auto& [first, last] : entry.writes)
                if (first < end && address < last) return true;
        return false;
    };
    if (recording.commands && writesRange(recording)) Flush();
    std::size_t through = 0;
    for (std::size_t index = 0; index < pending.size(); ++index)
        if (writesRange(pending[index])) through = index + 1;
    for (; through > 0; --through) {
        pending.front().commands->Wait();
        auto batch = std::move(pending.front());
        pending.erase(pending.begin());
        retire(std::move(batch));
    }
    timing.Mark(GuestMemory::AccessSite::Current());
}

DrawQueue::SampleCounter::SampleCounter(const Context& context, bool precise) : context(context), precise(precise) {
    // Enough for every batch the queue keeps in flight (Begin waits when none is free).
    constexpr std::uint32_t Queries = 128;
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_OCCLUSION;
    info.queryCount = Queries;
    Check(context.Function<PFN_vkCreateQueryPool>("vkCreateQueryPool")(context.device, &info, nullptr, &pool), "vkCreateQueryPool");
    for (std::uint32_t query = Queries; query-- > 0;) free.push_back(query);
}

DrawQueue::SampleCounter::~SampleCounter() {
    context.Function<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(context.device, pool, nullptr);
}

void DrawQueue::EnableSampleCounting(const Context& context, bool precise) {
    if (samples) return;
    // The batch being recorded has no query: it is submitted uncounted, as it began before counting.
    Flush();
    samples = std::make_unique<SampleCounter>(context, precise);
}


}
