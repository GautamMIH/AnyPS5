#include <cstdlib>
#include <cstdio>
#include "prx/libSceAgcDriver/Graphics/include/RenderCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthTargetLayout.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace AgcDriver::Graphics {
namespace {

std::uint32_t hostDepthBytes(const DepthTarget& depth) {
    return depth.format == VK_FORMAT_D16_UNORM ? 2u : 4u;
}

VkImageAspectFlags aspects(const DepthTarget& depth) {
    return depth.hasStencil ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
}

bool rangesOverlap(std::uint64_t address, std::size_t bytes, std::uint64_t base, std::size_t size) {
    return size != 0 && address < base + size && base < address + bytes;
}

// Guest depth (unorm16 or float32 texels) to the host attachment's depth texels. A 16-bit guest
// plane is widened to float when the host needs a combined float depth/stencil format.
void depthToHost(const DepthTarget& depth, std::span<const std::byte> guest, std::span<std::byte> host) {
    if (hostDepthBytes(depth) == depth.depthElementBytes) {
        std::memcpy(host.data(), guest.data(), host.size());
        return;
    }
    const auto count = guest.size() / 2u;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint16_t value = 0;
        std::memcpy(&value, guest.data() + i * 2u, sizeof(value));
        const float widened = static_cast<float>(value) / 65535.0f;
        std::memcpy(host.data() + i * 4u, &widened, sizeof(widened));
    }
}

void depthToGuest(const DepthTarget& depth, std::span<const std::byte> host, std::span<std::byte> guest) {
    if (hostDepthBytes(depth) == depth.depthElementBytes) {
        std::memcpy(guest.data(), host.data(), guest.size());
        return;
    }
    const auto count = guest.size() / 2u;
    for (std::size_t i = 0; i < count; ++i) {
        float value = 0;
        std::memcpy(&value, host.data() + i * 4u, sizeof(value));
        const auto narrowed = static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f));
        std::memcpy(guest.data() + i * 2u, &narrowed, sizeof(narrowed));
    }
}

}

ResidentDepth::ResidentDepth(const Context& context, const DepthTarget& depth) : context(context), depth(depth) {
    target = std::make_unique<RenderTarget>(context, depth);
    const auto pixels = static_cast<std::size_t>(depth.extent.width) * depth.extent.height;
    constexpr auto usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (depth.depthElementBytes != 0) depthLinear = std::make_unique<Buffer>(context, pixels * hostDepthBytes(depth), usage);
    if (depth.hasStencil) stencilLinear = std::make_unique<Buffer>(context, pixels, usage);
    const auto watch = [this](std::uint64_t address, std::size_t bytes) {
        watches.push_back(std::make_unique<GuestMemoryTracking::Watch>(address, bytes, this, [](void* owner, GuestMemoryTracking::Access access) {
            static_cast<ResidentDepth*>(owner)->resolveCpuAccess(access);
        }));
    };
    if (depth.depthElementBytes != 0) watch(depth.depthAddress, depth.depthBytes);
    if (depth.hasStencil) watch(depth.stencilAddress, depth.stencilBytes);
}

ResidentDepth::~ResidentDepth() {
    if (gpuPool != VK_NULL_HANDLE && context.detiler != nullptr) context.detiler->DestroyPool(gpuPool);
}

bool ResidentDepth::prepareGpu() {
    if (context.guestGpuMemory == nullptr || context.detiler == nullptr || context.drawQueue == nullptr) return false;
    // A 16-bit plane under a float host format is converted on the CPU.
    if (depth.depthElementBytes != 0 && hostDepthBytes(depth) != depth.depthElementBytes) return false;
    const auto plane = [&](GpuPlane& gpu, std::uint64_t address, std::uint32_t elementBytes) {
        const DepthTargetLayout layout(depth.extent.width, depth.extent.height, elementBytes);
        const auto view = context.guestGpuMemory->Resolve(address, layout.Bytes());
        if (!view || view->bytes < layout.Bytes()) return false;
        if (gpu.linear && gpu.guest == view->buffer && gpu.guestOffset == view->offset) return true;
        TileMipLayout mip{};
        mip.tiledSize = layout.Bytes();
        mip.linearSize = layout.LinearBytes();
        mip.width = depth.extent.width;
        mip.height = depth.extent.height;
        mip.blocksPerRow = elementBytes == 4 ? (depth.extent.width + 127u) / 128u : (depth.extent.width + 255u) / 256u;
        mip.pitchBytes = depth.extent.width * elementBytes;
        if (!gpu.linear) gpu.linear = std::make_unique<Buffer>(context, layout.LinearBytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        // Sets are never rewritten: a changed guest view takes new ones once the old ones are idle.
        if (gpuPool == VK_NULL_HANDLE || gpuPoolSets + 2 > 8) {
            if (gpuPool != VK_NULL_HANDLE) {
                context.drawQueue->WaitGpu();
                context.detiler->DestroyPool(gpuPool);
            }
            gpuPool = context.detiler->CreatePool(8);
            gpuPoolSets = 0;
            gpuDepth.guest = gpuStencil.guest = VK_NULL_HANDLE;
        }
        gpu.detile = context.detiler->Prepare(TextureTileMode::Depth64KB, elementBytes, view->buffer, view->offset, gpu.linear->Handle(), 0, mip, 0, false, gpuPool);
        gpu.tile = context.detiler->Prepare(TextureTileMode::Depth64KB, elementBytes, gpu.linear->Handle(), 0, view->buffer, view->offset, mip, 0, true, gpuPool);
        gpuPoolSets += 2;
        gpu.guest = view->buffer;
        gpu.guestOffset = view->offset;
        return true;
    };
    if (depth.depthElementBytes != 0 && !plane(gpuDepth, depth.depthAddress, depth.depthElementBytes)) return false;
    if (depth.hasStencil && !plane(gpuStencil, depth.stencilAddress, 1)) return false;
    return true;
}

void ResidentDepth::GpuWriteCompleted() {
    if (!gpuWritePending) return;
    gpuWritePending = false;
    if (!dirty && valid && !watches.empty()) protect(GuestMemoryTracking::Protection::Read);
}

bool ResidentDepth::WriteBackOnGpu() {
    if (!dirty) return true;
    if (!prepareGpu()) return false;
    PerformanceTimer timing("Graphics.ResidentDepth.GpuWriteBack");
    if (TraceRenderTargets()) std::fprintf(stderr, "[rt] writeback depth@0x%llx+0x%llx site=%s\n", static_cast<unsigned long long>(depth.depthAddress), static_cast<unsigned long long>(depth.depthBytes + depth.stencilBytes), GuestMemory::AccessSite::Current());
    const auto commands = context.drawQueue->Begin(context);
    const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    transition(commands, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkMemoryBarrier reuse{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    reuse.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    reuse.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reuse, 0, nullptr, 0, nullptr);
    copy(commands, false, gpuDepth.linear.get(), gpuStencil.linear.get());
    VkMemoryBarrier toShader{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &toShader, 0, nullptr, 0, nullptr);
    // Only texels inside the extent are written: padding keeps its guest contents, as on the CPU path.
    if (gpuDepth.linear) context.detiler->Record(commands, gpuDepth.tile);
    if (gpuStencil.linear && depth.hasStencil) context.detiler->Record(commands, gpuStencil.tile);
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
    const auto keepAlive = shared_from_this();
    if (depth.depthBytes != 0) {
        context.drawQueue->NoteGuestWrite(depth.depthAddress, depth.depthAddress + depth.depthBytes, keepAlive);
        WriteTracker::NoteGpuWrite(depth.depthAddress, depth.depthBytes);
    }
    if (depth.stencilBytes != 0) {
        context.drawQueue->NoteGuestWrite(depth.stencilAddress, depth.stencilAddress + depth.stencilBytes, keepAlive);
        WriteTracker::NoteGpuWrite(depth.stencilAddress, depth.stencilBytes);
    }
    dirty = false;
    gpuWritePending = true;
    timing.Mark("recorded", depth.depthBytes + depth.stencilBytes);
    return true;
}

void ResidentDepth::protect(GuestMemoryTracking::Protection protection) {
    for (const auto& watch : watches) watch->Protect(protection);
}

void ResidentDepth::transition(VkCommandBuffer commands, VkImageLayout next) {
    static const bool always = std::getenv("ANYPS5_ATTACHMENT_BARRIERS") != nullptr;
    if (!always && layout == next && next == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) return;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0u : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = layout;
    barrier.newLayout = next;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = target->Image();
    barrier.subresourceRange = {aspects(depth), 0, 1, 0, 1};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    layout = next;
}

void ResidentDepth::copy(VkCommandBuffer commands, bool toImage, Buffer* depthBuffer, Buffer* stencilBuffer) {
    const auto planes = [&](Buffer* buffer, VkImageAspectFlags aspect) {
        if (buffer == nullptr) return;
        VkBufferImageCopy region{};
        region.imageSubresource = {aspect, 0, 0, 1};
        region.imageExtent = {depth.extent.width, depth.extent.height, 1};
        if (toImage) context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, buffer->Handle(), target->Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        else context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, target->Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->Handle(), 1, &region);
    };
    planes(depthBuffer, VK_IMAGE_ASPECT_DEPTH_BIT);
    planes(stencilBuffer, VK_IMAGE_ASPECT_STENCIL_BIT);
}

void ResidentDepth::Begin(VkCommandBuffer commands) {
    PerformanceTimer timing("Graphics.ResidentDepth.Begin");
    Require(!watches.empty(), "depth target memory ownership was released");
    if (!valid && prepareGpu()) {
        protect(GuestMemoryTracking::Protection::Read);
        // Detiled on the GPU straight from imported guest memory, after earlier queued writes.
        const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier(commands, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
        if (gpuDepth.linear && depth.depthElementBytes != 0) context.detiler->Record(commands, gpuDepth.detile);
        if (gpuStencil.linear && depth.hasStencil) context.detiler->Record(commands, gpuStencil.detile);
        VkMemoryBarrier detiled{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        detiled.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        detiled.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &detiled, 0, nullptr, 0, nullptr);
        transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copy(commands, true, depth.depthElementBytes != 0 ? gpuDepth.linear.get() : nullptr, depth.hasStencil ? gpuStencil.linear.get() : nullptr);
        timing.Mark("gpu_detile", depth.depthBytes + depth.stencilBytes);
    } else if (!valid) {
        protect(GuestMemoryTracking::Protection::Read);
        const GuestMemory::MemoryAccessScope suspended(nullptr, nullptr);
        if (depthLinear) {
            const DepthTargetLayout plane(depth.extent.width, depth.extent.height, depth.depthElementBytes);
            depthTiled.resize(plane.Bytes());
            std::vector<std::byte> linear(plane.LinearBytes());
            GuestMemory::Read(depth.depthAddress, depthTiled, plane.Alignment());
            plane.Detile(depthTiled, linear);
            depthToHost(depth, linear, depthLinear->Bytes());
        }
        if (stencilLinear) {
            const DepthTargetLayout plane(depth.extent.width, depth.extent.height, 1);
            stencilTiled.resize(plane.Bytes());
            GuestMemory::Read(depth.stencilAddress, stencilTiled, plane.Alignment());
            plane.Detile(stencilTiled, stencilLinear->Bytes());
        }
        timing.Mark("detile", depth.depthBytes + depth.stencilBytes);
        VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        upload.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        upload.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &upload, 0, nullptr, 0, nullptr);
        transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copy(commands, true, depthLinear.get(), stencilLinear.get());
    } else {
        timing.Mark("reuse");
    }
    transition(commands, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    valid = true;
    dirty = true;
    gpuWritePending = false;
    ++generation;
    protect(GuestMemoryTracking::Protection::None);
}

std::uint32_t ResidentDepth::HostDepthBytes() const {
    return hostDepthBytes(depth);
}

void ResidentDepth::PrepareSampling(VkCommandBuffer commands) {
    if (layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) transition(commands, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void ResidentDepth::CopyDepth(VkCommandBuffer commands, VkBuffer buffer) {
    Require(valid && depth.depthElementBytes != 0, "resident depth has no current depth plane");
    transition(commands, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.imageExtent = {depth.extent.width, depth.extent.height, 1};
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, target->Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
}

void ResidentDepth::Download(VkCommandBuffer commands) {
    if (!dirty) return;
    transition(commands, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkMemoryBarrier reuse{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    reuse.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    reuse.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reuse, 0, nullptr, 0, nullptr);
    copy(commands, false, depthLinear.get(), stencilLinear.get());
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
}

void ResidentDepth::Commit() {
    if (!dirty) return;
    PerformanceTimer timing("Graphics.ResidentDepth.Commit");
    const GuestMemory::MemoryAccessScope suspended(nullptr, nullptr);
    if (depthLinear) {
        depthLinear->Invalidate();
        const DepthTargetLayout plane(depth.extent.width, depth.extent.height, depth.depthElementBytes);
        std::vector<std::byte> linear(plane.LinearBytes());
        depthToGuest(depth, depthLinear->Bytes(), linear);
        // Padding texels outside the extent keep their uploaded contents.
        plane.Tile(linear, depthTiled);
        GuestMemory::WriteThroughAlias(depth.depthAddress, depthTiled.data(), depthTiled.size());
    }
    if (stencilLinear) {
        stencilLinear->Invalidate();
        const DepthTargetLayout plane(depth.extent.width, depth.extent.height, 1);
        plane.Tile(stencilLinear->Bytes(), stencilTiled);
        GuestMemory::WriteThroughAlias(depth.stencilAddress, stencilTiled.data(), stencilTiled.size());
    }
    timing.Mark("guest_write", depth.depthBytes + depth.stencilBytes);
    dirty = false;
    protect(GuestMemoryTracking::Protection::Read);
}

void ResidentDepth::Invalidate() {
    Require(!dirty, "cannot discard GPU-owned depth target contents");
    if (TraceRenderTargets() && valid) std::fprintf(stderr, "[rt] invalidate depth@0x%llx site=%s\n", static_cast<unsigned long long>(depth.depthAddress), GuestMemory::AccessSite::Current());
    protect(GuestMemoryTracking::Protection::ReadWrite);
    valid = false;
}

void ResidentDepth::ReleaseMemory() {
    Require(!dirty, "cannot release GPU-owned depth target memory");
    Invalidate();
    watches.clear();
}

bool ResidentDepth::Overlaps(std::uint64_t address, std::size_t bytes) const {
    return rangesOverlap(address, bytes, depth.depthAddress, depth.depthBytes) || rangesOverlap(address, bytes, depth.stencilAddress, depth.stencilBytes);
}

bool ResidentDepth::Overlaps(const DepthTarget& other) const {
    const auto pages = [&](std::uint64_t address, std::size_t bytes, std::uint64_t otherAddress, std::size_t otherBytes) {
        return bytes != 0 && otherBytes != 0 && rangesOverlap(address, bytes, otherAddress, otherBytes);
    };
    const std::pair<std::uint64_t, std::size_t> mine[] = {{depth.depthAddress, depth.depthBytes}, {depth.stencilAddress, depth.stencilBytes}};
    const std::pair<std::uint64_t, std::size_t> theirs[] = {{other.depthAddress, other.depthBytes}, {other.stencilAddress, other.stencilBytes}};
    for (const auto& [address, bytes] : mine) {
        for (const auto& [otherAddress, otherBytes] : theirs) {
            if (pages(address, bytes, otherAddress, otherBytes)) return true;
        }
    }
    return false;
}

void ResidentDepth::resolveCpuAccess(GuestMemoryTracking::Access access) {
    PerformanceTimer timing("Graphics.DepthMemory.CpuAccess");
    Require(!watches.empty() && context.drawQueue != nullptr, "depth target memory resolver is unavailable");
    if (dirty && WriteBackOnGpu()) timing.Mark("gpu_writeback_recorded");
    if (dirty || gpuWritePending || access == GuestMemoryTracking::Access::Invalidate) {
        context.drawQueue->WaitGpu();
        timing.Mark("draw_wait");
    }
    if (gpuWritePending && !dirty) {
        gpuWritePending = false;
        if (access == GuestMemoryTracking::Access::Read) protect(GuestMemoryTracking::Protection::Read);
    }
    if (dirty) {
        CommandBatch batch(context);
        Download(batch.Handle());
        batch.SubmitAndWait();
        timing.Mark("download_wait");
        Commit();
        timing.Mark("guest_writeback");
    }
    if (access != GuestMemoryTracking::Access::Read) Invalidate();
    if (access == GuestMemoryTracking::Access::Invalidate) context.drawQueue->Wait();
}

std::shared_ptr<ResidentDepth> RenderCache::GetDepth(const DepthTarget& depth) {
    // As for color targets: a barrier, and a CPU wait only before a CPU upload.
    const auto resolvePlanes = [&] {
        if (depth.depthBytes != 0) context.drawQueue->Resolve(depth.depthAddress, depth.depthBytes);
        if (depth.stencilBytes != 0) context.drawQueue->Resolve(depth.stencilAddress, depth.stencilBytes);
    };
    static const bool cpuTargetWait = std::getenv("ANYPS5_CPU_TARGET_WAIT") != nullptr;
    if (context.drawQueue) {
        std::optional<GuestMemory::GpuAccessScope> gpuAccess;
        if (!cpuTargetWait) gpuAccess.emplace();
        resolvePlanes();
    }
    const auto cpuResolve = [&](const std::shared_ptr<ResidentDepth>& resident) {
        if (context.drawQueue && resident->NeedsCpuUpload()) resolvePlanes();
        return resident;
    };
    for (auto it = depthEntries.begin(); it != depthEntries.end();) {
        const auto& previous = (*it)->Description();
        if (!(*it)->Overlaps(depth)) {
            ++it;
            continue;
        }
        if (previous.depthAddress == depth.depthAddress && previous.stencilAddress == depth.stencilAddress && previous.extent.width == depth.extent.width && previous.extent.height == depth.extent.height && previous.depthElementBytes == depth.depthElementBytes && previous.hasStencil == depth.hasStencil) return cpuResolve(*it);
        if ((*it)->Dirty()) {
            const GuestMemory::AccessSite replaceSite("rt_replace");
        if (previous.depthBytes != 0) Resolve(previous.depthAddress, previous.depthBytes, true);
            if (previous.stencilBytes != 0) Resolve(previous.stencilAddress, previous.stencilBytes, true);
        }
        (*it)->ReleaseMemory();
        it = depthEntries.erase(it);
    }
    // A color target overlapping this depth surface holds the same bytes: it goes first.
    for (auto it = entries.begin(); it != entries.end();) {
        const bool shares = (depth.depthBytes != 0 && it->second->Overlaps(ColorTarget{depth.depthAddress, {}, VK_FORMAT_UNDEFINED, depth.depthBytes, 0})) || (depth.stencilBytes != 0 && it->second->Overlaps(ColorTarget{depth.stencilAddress, {}, VK_FORMAT_UNDEFINED, depth.stencilBytes, 0}));
        if (!shares) {
            ++it;
            continue;
        }
        const GuestMemory::AccessSite replaceSite("rt_replace");
        Resolve(it->second->Description().address, it->second->Description().bytes, true);
        it->second->ReleaseMemory();
        it = entries.erase(it);
    }
    if (depthEntries.size() >= 16) {
        Flush();
        for (const auto& resident : depthEntries) resident->ReleaseMemory();
        depthEntries.clear();
    }
    auto entry = std::make_shared<ResidentDepth>(context, depth);
    depthEntries.push_back(entry);
    return cpuResolve(entry);
}

}
