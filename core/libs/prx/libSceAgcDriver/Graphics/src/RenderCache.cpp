#include <cstdio>
#include "prx/libSceAgcDriver/Graphics/include/RenderCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include <cstdlib>
#include <limits>

namespace AgcDriver::Graphics {

void ResidentColor::Transition(VkCommandBuffer commands, VkImageLayout next) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0u : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = layout;
    barrier.newLayout = next;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = Target().Image();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, color.Layered() ? color.layers : 1u};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    layout = next;
}

void ResidentColor::Begin(VkCommandBuffer commands) {
    PerformanceTimer timing("Graphics.ResidentColor.Begin");
    Require(generation != std::numeric_limits<std::uint64_t>::max(), "resident color generation overflow");
    ++generation;
    Require(memoryWatch != nullptr, "render target memory ownership was released");
    if (!valid) {
        if (TraceRenderTargets()) std::fprintf(stderr, "[rt] upload color@0x%llx+0x%llx %ux%u\n", static_cast<unsigned long long>(color.address), static_cast<unsigned long long>(color.bytes), color.extent.width, color.extent.height);
        memoryWatch->Protect(GuestMemoryTracking::Protection::Read);
        const GuestMemory::MemoryAccessScope suspended(nullptr, nullptr);
        if (const auto view = layered ? std::nullopt : guestView()) {
            // Detiled straight from imported guest memory, in queue order after earlier writes.
            transfer.UploadFrom(commands, view->buffer, view->offset, color.extent.width, color.extent.height, color.tileMode, color.elementBytes, color.tail);
            Transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {color.extent.width, color.extent.height, 1};
            context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, transfer.LinearBuffer(), Target().Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            timing.Mark("gpu_upload", color.bytes);
        } else if (layered && layered->PrepareGpu()) {
            // Slices detiled on the GPU from imported guest memory (ANYPS5_CPU_LAYERED=1: on the CPU).
            layered->RecordGpuUpload(commands);
            Transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, color.layers};
            copy.imageExtent = {color.extent.width, color.extent.height, 1};
            context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, layered->GpuLinearBuffer(), Target().Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            timing.Mark("gpu_layered_upload", color.bytes);
        } else if (layered) {
            layered->Upload();
            Transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            layered->RecordToImage(commands);
        } else {
            transfer.Upload(color.address, color.extent.width, color.extent.height, color.tileMode, color.elementBytes, color.tail);
            transfer.Detile(commands);
            Transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {color.extent.width, color.extent.height, 1};
            context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, transfer.LinearBuffer(), Target().Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }
        timing.Mark("upload", color.bytes);
    } else {
        timing.Mark("reuse");
    }
    Transition(commands, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    valid = true;
    dirty = true;
    gpuWritePending = false;
    memoryWatch->Protect(GuestMemoryTracking::Protection::None);
}

std::optional<GuestGpuMemory::View> ResidentColor::guestView() const {
    if (context.guestGpuMemory == nullptr || layered) return std::nullopt;
    const ColorTargetLayout layout(color.extent.width, color.extent.height, color.tileMode, color.elementBytes, color.tail);
    auto view = context.guestGpuMemory->Resolve(color.address, layout.Bytes());
    if (!view || view->bytes < layout.Bytes()) return std::nullopt;
    return view;
}

void ResidentColor::downloadLinear(VkCommandBuffer commands) {
    Transition(commands, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkMemoryBarrier reuse{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    reuse.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    reuse.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reuse, 0, nullptr, 0, nullptr);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {color.extent.width, color.extent.height, 1};
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, Target().Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transfer.LinearBuffer(), 1, &copy);
}

bool ResidentColor::WriteBackOnGpu() {
    if (!dirty) return true;
    if (context.drawQueue == nullptr) return false;
    if (layered) {
        if (!layered->PrepareGpu()) return false;
        PerformanceTimer timing("Graphics.ResidentColor.GpuWriteBack");
        if (TraceRenderTargets()) std::fprintf(stderr, "[rt] writeback layered color@0x%llx+0x%llx site=%s\n", static_cast<unsigned long long>(color.address), static_cast<unsigned long long>(color.bytes), GuestMemory::AccessSite::Current());
        const auto commands = context.drawQueue->Begin(context);
        Transition(commands, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkMemoryBarrier reuse{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        reuse.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        reuse.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reuse, 0, nullptr, 0, nullptr);
        layered->RecordGpuWriteBack(commands);
        context.drawQueue->NoteGuestWrite(color.address, color.address + color.bytes, shared_from_this());
        WriteTracker::NoteGpuWrite(color.address, color.bytes);
        dirty = false;
        gpuWritePending = true;
        timing.Mark("recorded", color.bytes);
        return true;
    }
    const auto view = guestView();
    if (!view) return false;
    PerformanceTimer timing("Graphics.ResidentColor.GpuWriteBack");
    if (TraceRenderTargets()) std::fprintf(stderr, "[rt] writeback color@0x%llx+0x%llx site=%s\n", static_cast<unsigned long long>(color.address), static_cast<unsigned long long>(color.bytes), GuestMemory::AccessSite::Current());
    const auto commands = context.drawQueue->Begin(context);
    downloadLinear(commands);
    transfer.TileTo(commands, view->buffer, view->offset);
    const ColorTargetLayout layout(color.extent.width, color.extent.height, color.tileMode, color.elementBytes, color.tail);
    // CPU accesses of the range wait for the batch; the texture cache sees the range as written.
    context.drawQueue->NoteGuestWrite(color.address, color.address + layout.Bytes(), shared_from_this());
    WriteTracker::NoteGpuWrite(color.address, layout.Bytes());
    dirty = false;
    gpuWritePending = true;
    timing.Mark("recorded", layout.Bytes());
    return true;
}

void ResidentColor::GpuWriteCompleted() {
    if (!gpuWritePending) return;
    gpuWritePending = false;
    if (!dirty && valid && memoryWatch) memoryWatch->Protect(GuestMemoryTracking::Protection::Read);
}

void ResidentColor::Download(VkCommandBuffer commands) {
    if (!dirty) return;
    Transition(commands, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkMemoryBarrier reuse{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    reuse.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    reuse.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reuse, 0, nullptr, 0, nullptr);
    if (layered) {
        layered->RecordFromImage(commands);
        return;
    }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {color.extent.width, color.extent.height, 1};
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, Target().Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transfer.LinearBuffer(), 1, &copy);
    transfer.Tile(commands);
}

void ResidentColor::Commit() {
    if (!dirty) return;
    const GuestMemory::MemoryAccessScope suspended(nullptr, nullptr);
    if (layered) layered->WriteBackTracked();
    else transfer.WriteBackTracked(color.address);
    dirty = false;
    memoryWatch->Protect(GuestMemoryTracking::Protection::Read);
}

std::shared_ptr<ResidentColor> RenderCache::Get(const ColorTarget& color, bool blending) {
    Require(color.address != 0 && color.bytes != 0 && color.bytes <= std::numeric_limits<std::uint64_t>::max() - color.address, "invalid resident color range");
    if (context.drawQueue) context.drawQueue->Resolve(color.address, color.bytes);
    if (blending) {
        VkFormatProperties properties{};
        context.formatProperties(context.physical, color.format, &properties);
        Require((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0, "render target format does not support blending");
    }
    for (auto it = entries.begin(); it != entries.end();) {
        const auto& previous = it->second->Description();
        if (!it->second->Overlaps(color)) {
            ++it;
            continue;
        }
        if (previous.address == color.address && previous.bytes == color.bytes && previous.extent.width == color.extent.width && previous.extent.height == color.extent.height && previous.format == color.format && previous.tileMode == color.tileMode && previous.elementBytes == color.elementBytes && previous.tail == color.tail && previous.layers == color.layers && previous.baseLayer == color.baseLayer && previous.surfaceSlices == color.surfaceSlices) return it->second;
        if (TraceRenderTargets()) std::fprintf(stderr, "[rt] replace color@0x%llx+0x%llx %ux%u fmt%d tile%u eb%u dirty=%d -> color@0x%llx+0x%llx %ux%u fmt%d tile%u eb%u\n", static_cast<unsigned long long>(previous.address), static_cast<unsigned long long>(previous.bytes), previous.extent.width, previous.extent.height, static_cast<int>(previous.format), static_cast<unsigned>(previous.tileMode), static_cast<unsigned>(previous.elementBytes), it->second->Dirty() ? 1 : 0, static_cast<unsigned long long>(color.address), static_cast<unsigned long long>(color.bytes), color.extent.width, color.extent.height, static_cast<int>(color.format), static_cast<unsigned>(color.tileMode), static_cast<unsigned>(color.elementBytes));
        // The new target is uploaded on the GPU after this, so the old one can be written back there.
        Resolve(previous.address, previous.bytes, true, true);
        it->second->ReleaseMemory();
        it = entries.erase(it);
    }
    for (auto it = depthEntries.begin(); it != depthEntries.end();) {
        if (!(*it)->Overlaps(color.address, color.bytes)) {
            ++it;
            continue;
        }
        const auto& previous = (*it)->Description();
        if (previous.depthBytes != 0) Resolve(previous.depthAddress, previous.depthBytes, true);
        if (previous.stencilBytes != 0) Resolve(previous.stencilAddress, previous.stencilBytes, true);
        (*it)->ReleaseMemory();
        it = depthEntries.erase(it);
    }
    if (entries.size() >= 64) {
        Flush();
        for (const auto& [address, resident] : entries) resident->ReleaseMemory();
        entries.clear();
    }
    auto entry = std::make_shared<ResidentColor>(context, color);
    entries.emplace(color.address, entry);
    largestColor = std::max<std::uint64_t>(largestColor, color.bytes);
    return entry;
}

std::shared_ptr<ResidentColor> RenderCache::Find(std::uint64_t address) const {
    const auto it = entries.find(address);
    if (it != entries.end() && context.drawQueue) context.drawQueue->Resolve(address, it->second->Description().bytes);
    return it != entries.end() && it->second->Valid() ? it->second : nullptr;
}

std::shared_ptr<ResidentDepth> RenderCache::FindDepth(std::uint64_t address) const {
    for (const auto& entry : depthEntries)
        if (entry->Valid() && entry->Description().depthElementBytes != 0 && entry->Description().depthAddress == address) return entry;
    return nullptr;
}

void RenderCache::Resolve(std::uint64_t address, std::size_t bytes, bool writable, bool gpuConsumer) {
    std::vector<std::shared_ptr<ResidentColor>> affected;
    // Entries beginning before the end of the range and at most largestColor bytes before it.
    const auto end = entries.lower_bound(address + bytes);
    auto first = address > largestColor ? entries.lower_bound(address - largestColor) : entries.begin();
    for (auto it = first; it != end; ++it) {
        const auto base = it->first;
        const auto& entry = it->second;
        const auto& color = entry->Description();
        if (address >= base + color.bytes || base >= address + bytes) continue;
        if (TraceRenderTargets() && (entry->Dirty() || (writable && entry->Valid()))) std::fprintf(stderr, "[rt] resolve 0x%llx+0x%zx %s%s hits color@0x%llx site=%s\n", static_cast<unsigned long long>(address), bytes, writable ? "write" : "read", gpuConsumer || GuestMemory::GpuAccessScope::Active() ? " gpu" : "", static_cast<unsigned long long>(base), GuestMemory::AccessSite::Current());
        if (entry->Dirty()) affected.push_back(entry);
        else if (writable) entry->Invalidate();
    }
    // A GPU consumer (a draw or dispatch, which runs after the draw queue's batch in order) needs
    // no CPU copy: colour targets are written back into imported guest memory on the GPU.
    if ((gpuConsumer || GuestMemory::GpuAccessScope::Active()) && !affected.empty()) {
        PerformanceTimer timing("Graphics.RenderCache.GpuResolve");
        std::erase_if(affected, [&](const std::shared_ptr<ResidentColor>& entry) {
            if (!entry->WriteBackOnGpu()) return false;
            if (writable) entry->Invalidate();
            return true;
        });
        timing.Mark("recorded");
    }
    std::vector<std::shared_ptr<ResidentDepth>> affectedDepth;
    for (const auto& entry : depthEntries) {
        if (!entry->Overlaps(address, bytes)) continue;
        if (entry->Dirty()) affectedDepth.push_back(entry);
        else if (writable) entry->Invalidate();
    }
    if ((gpuConsumer || GuestMemory::GpuAccessScope::Active()) && !affectedDepth.empty()) {
        std::erase_if(affectedDepth, [&](const std::shared_ptr<ResidentDepth>& entry) {
            if (!entry->WriteBackOnGpu()) return false;
            if (writable) entry->Invalidate();
            return true;
        });
    }
    if (!affected.empty() || !affectedDepth.empty()) {
        // A CPU consumer: surfaces with a GPU path are tiled into guest memory on the GPU, and the
        // CPU waits once for the draw queue instead of downloading and tiling them itself.
        bool recorded = false;
        const auto viaGpu = [&](auto& list) {
            std::erase_if(list, [&](const auto& entry) {
                if (!entry->WriteBackOnGpu()) return false;
                recorded = true;
                return true;
            });
        };
        viaGpu(affected);
        viaGpu(affectedDepth);
        if (recorded) {
            PerformanceTimer timing("Graphics.RenderCache.GpuResolveWait");
            context.drawQueue->WaitGpu();
            timing.Mark("draw_wait");
            for (const auto& [base, entry] : entries) entry->GpuWriteCompleted();
            for (const auto& entry : depthEntries) entry->GpuWriteCompleted();
            if (writable) {
                for (const auto& [base, entry] : entries)
                    if (!entry->Dirty() && entry->Valid() && !(address >= base + entry->Description().bytes || base >= address + bytes)) entry->Invalidate();
                for (const auto& entry : depthEntries)
                    if (!entry->Dirty() && entry->Valid() && entry->Overlaps(address, bytes)) entry->Invalidate();
            }
        }
    }
    if (affected.empty() && affectedDepth.empty()) return;
    // Debug aid: APS5_TRACE_RESOLVE=1 logs every resolve that writes resident surfaces back to guest
    // memory, with the access that caused it.
    static const bool traceResolve = std::getenv("APS5_TRACE_RESOLVE") != nullptr;
    if (traceResolve) {
        std::fprintf(stderr, "[resolve] site=%s range=0x%llx+0x%zx %s colors=%zu depths=%zu", GuestMemory::AccessSite::Current(), static_cast<unsigned long long>(address), bytes, writable ? "write" : "read", affected.size(), affectedDepth.size());
        for (const auto& entry : affected) std::fprintf(stderr, " color@0x%llx+0x%llx(%ux%u layers %u%s)", static_cast<unsigned long long>(entry->Description().address), static_cast<unsigned long long>(entry->Description().bytes), entry->Description().extent.width, entry->Description().extent.height, entry->Description().layers, entry->Description().Layered() ? " layered" : "");
        for (const auto& entry : affectedDepth) std::fprintf(stderr, " depth@0x%llx", static_cast<unsigned long long>(entry->Description().depthAddress));
        std::fprintf(stderr, "\n");
    }
    PerformanceTimer timing("Graphics.RenderCache.Resolve");
    if (context.drawQueue) context.drawQueue->Wait();
    CommandBatch batch(context);
    for (const auto& entry : affected) entry->Download(batch.Handle());
    for (const auto& entry : affectedDepth) entry->Download(batch.Handle());
    batch.SubmitAndWait();
    timing.Mark("download_wait");
    for (const auto& entry : affected) {
        entry->Commit();
        if (writable) entry->Invalidate();
    }
    for (const auto& entry : affectedDepth) {
        entry->Commit();
        if (writable) entry->Invalidate();
    }
    timing.Mark("guest_writeback");
}

void RenderCache::Flush() {
    Resolve(0, std::numeric_limits<std::size_t>::max(), true);
}

}
