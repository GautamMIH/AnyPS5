#include <cstdio>
#include "prx/libSceAgcDriver/Graphics/include/RenderCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
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

ResidentDepth::~ResidentDepth() = default;

void ResidentDepth::protect(GuestMemoryTracking::Protection protection) {
    for (const auto& watch : watches) watch->Protect(protection);
}

void ResidentDepth::transition(VkCommandBuffer commands, VkImageLayout next) {
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

void ResidentDepth::copy(VkCommandBuffer commands, bool toImage) {
    const auto planes = [&](Buffer* buffer, VkImageAspectFlags aspect) {
        if (buffer == nullptr) return;
        VkBufferImageCopy region{};
        region.imageSubresource = {aspect, 0, 0, 1};
        region.imageExtent = {depth.extent.width, depth.extent.height, 1};
        if (toImage) context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, buffer->Handle(), target->Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        else context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, target->Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->Handle(), 1, &region);
    };
    planes(depthLinear.get(), VK_IMAGE_ASPECT_DEPTH_BIT);
    planes(stencilLinear.get(), VK_IMAGE_ASPECT_STENCIL_BIT);
}

void ResidentDepth::Begin(VkCommandBuffer commands) {
    PerformanceTimer timing("Graphics.ResidentDepth.Begin");
    Require(!watches.empty(), "depth target memory ownership was released");
    if (!valid) {
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
        copy(commands, true);
    } else {
        timing.Mark("reuse");
    }
    transition(commands, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    valid = true;
    dirty = true;
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
    copy(commands, false);
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

bool ResidentDepth::SharesPages(const DepthTarget& other) const {
    const auto pageSize = GuestMemoryTracking::GuestMemoryTrackingPageSize_nid_postfix();
    const auto pages = [&](std::uint64_t address, std::size_t bytes, std::uint64_t otherAddress, std::size_t otherBytes) {
        if (bytes == 0 || otherBytes == 0) return false;
        return address / pageSize <= (otherAddress + otherBytes - 1) / pageSize && otherAddress / pageSize <= (address + bytes - 1) / pageSize;
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
    if (dirty || access == GuestMemoryTracking::Access::Invalidate) {
        context.drawQueue->WaitGpu();
        timing.Mark("draw_wait");
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
    if (context.drawQueue) {
        if (depth.depthBytes != 0) context.drawQueue->Resolve(depth.depthAddress, depth.depthBytes);
        if (depth.stencilBytes != 0) context.drawQueue->Resolve(depth.stencilAddress, depth.stencilBytes);
    }
    for (auto it = depthEntries.begin(); it != depthEntries.end();) {
        const auto& previous = (*it)->Description();
        if (!(*it)->SharesPages(depth)) {
            ++it;
            continue;
        }
        if (previous.depthAddress == depth.depthAddress && previous.stencilAddress == depth.stencilAddress && previous.extent.width == depth.extent.width && previous.extent.height == depth.extent.height && previous.depthElementBytes == depth.depthElementBytes && previous.hasStencil == depth.hasStencil) return *it;
        if ((*it)->Dirty()) {
            if (previous.depthBytes != 0) Resolve(previous.depthAddress, previous.depthBytes, true);
            if (previous.stencilBytes != 0) Resolve(previous.stencilAddress, previous.stencilBytes, true);
        }
        (*it)->ReleaseMemory();
        it = depthEntries.erase(it);
    }
    // A color target sharing pages with this depth surface would fight over page protection.
    for (auto it = entries.begin(); it != entries.end();) {
        const bool shares = (depth.depthBytes != 0 && it->second->SharesPages(ColorTarget{depth.depthAddress, {}, VK_FORMAT_UNDEFINED, depth.depthBytes, 0})) || (depth.stencilBytes != 0 && it->second->SharesPages(ColorTarget{depth.stencilAddress, {}, VK_FORMAT_UNDEFINED, depth.stencilBytes, 0}));
        if (!shares) {
            ++it;
            continue;
        }
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
    return entry;
}

}
