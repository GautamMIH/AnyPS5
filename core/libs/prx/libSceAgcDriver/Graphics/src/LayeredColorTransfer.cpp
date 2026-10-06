#include <cstdio>
#include "prx/libSceAgcDriver/Graphics/include/LayeredColorTransfer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureAddressing.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <cstdlib>
#include <cstring>

namespace AgcDriver::Graphics {

LayeredColorTransfer::LayeredColorTransfer(const Context& context, const ColorTarget& color) : context(context), color(color) {
    Require(color.Layered() && color.tileMode != ColorTileMode::Linear, "layered colour transfers need a tiled array surface");
    // The rendered mip inside each slice's chain; TexelOffset counts from the slice start, the
    // target's address from the mip.
    mip = ComputeElementMipLayout(ColorTextureTileMode(color.tileMode), color.elementBytes, color.surfaceExtent.width, color.surfaceExtent.height, color.mipCount).at(color.mipLevel);
    Require(mip.width == color.extent.width && mip.height == color.extent.height, "layered colour target mip extent disagrees with its surface");
    const ColorTargetLayout layout(color.extent.width, color.extent.height, color.tileMode, color.elementBytes, color.tail);
    Require(color.bytes == color.LayeredBytes(layout.Bytes()), "layered colour target size disagrees with its slices");
    target = std::make_unique<RenderTarget>(context, color, false);
    const auto linearBytes = static_cast<std::size_t>(color.extent.width) * color.extent.height * color.elementBytes * color.layers;
    linear = std::make_unique<Buffer>(context, linearBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
}

LayeredColorTransfer::~LayeredColorTransfer() {
    if (gpuPool != VK_NULL_HANDLE && context.detiler != nullptr) context.detiler->DestroyPool(gpuPool);
}

bool LayeredColorTransfer::PrepareGpu() {
    static const bool cpuOnly = std::getenv("ANYPS5_CPU_LAYERED") != nullptr;
    if (cpuOnly || context.guestGpuMemory == nullptr || context.detiler == nullptr || context.drawQueue == nullptr) return false;
    const auto view = context.guestGpuMemory->Resolve(color.address, color.bytes);
    if (!view || view->bytes < color.bytes) return false;
    if (gpuLinear && gpuGuest == view->buffer && gpuGuestOffset == view->offset) return true;
    const auto layerBytes = static_cast<std::uint64_t>(color.extent.width) * color.extent.height * color.elementBytes;
    if (!gpuLinear) gpuLinear = std::make_unique<Buffer>(context, static_cast<std::size_t>(layerBytes * color.layers), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    // Sets are never rewritten: a changed guest view takes a new pool once the old passes are idle.
    if (gpuPool != VK_NULL_HANDLE) {
        context.drawQueue->WaitGpu();
        context.detiler->DestroyPool(gpuPool);
    }
    gpuPool = context.detiler->CreatePool(color.layers * 2u);
    detilePasses.clear();
    tilePasses.clear();
    // Slice L of the target starts sliceBytes * L past its address (the rendered mip of slice
    // baseLayer + L), addressed as array layer baseLayer + L: the CPU path's texelOffset.
    for (std::uint32_t layer = 0; layer < color.layers; ++layer) {
        const auto guestOffset = view->offset + layer * color.sliceBytes;
        const auto linearOffset = layer * layerBytes;
        detilePasses.push_back(context.detiler->Prepare(ColorTextureTileMode(color.tileMode), color.elementBytes, view->buffer, guestOffset, gpuLinear->Handle(), linearOffset, mip, color.baseLayer + layer, false, gpuPool));
        tilePasses.push_back(context.detiler->Prepare(ColorTextureTileMode(color.tileMode), color.elementBytes, gpuLinear->Handle(), linearOffset, view->buffer, guestOffset, mip, color.baseLayer + layer, true, gpuPool));
    }
    gpuGuest = view->buffer;
    gpuGuestOffset = view->offset;
    return true;
}

void LayeredColorTransfer::RecordGpuUpload(VkCommandBuffer commands) {
    const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier(commands, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    for (const auto& pass : detilePasses) context.detiler->Record(commands, pass);
    VkMemoryBarrier detiled{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    detiled.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    detiled.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &detiled, 0, nullptr, 0, nullptr);
}

void LayeredColorTransfer::RecordGpuWriteBack(VkCommandBuffer commands) {
    const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, color.layers};
    copy.imageExtent = {color.extent.width, color.extent.height, 1};
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, target->Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gpuLinear->Handle(), 1, &copy);
    VkMemoryBarrier toShader{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &toShader, 0, nullptr, 0, nullptr);
    // Only texels inside the extent are written: padding keeps its guest contents, as on the CPU path.
    for (const auto& pass : tilePasses) context.detiler->Record(commands, pass);
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
}

std::uint64_t LayeredColorTransfer::texelOffset(std::uint32_t layer, std::uint32_t x, std::uint32_t y) const {
    return layer * color.sliceBytes + TexelOffset(ColorTextureTileMode(color.tileMode), color.elementBytes, mip, x, y, color.baseLayer + layer) - mip.tiledOffset;
}

void LayeredColorTransfer::Upload() {
    PerformanceTimer timing("Graphics.LayeredColor.Upload");
    tiled.resize(color.bytes);
    GuestMemory::Read(color.address, tiled, 1);
    timing.Mark("guest_read", color.bytes);
    auto bytes = linear->Bytes();
    const auto rowBytes = static_cast<std::size_t>(color.extent.width) * color.elementBytes;
    for (std::uint32_t layer = 0; layer < color.layers; ++layer) {
        for (std::uint32_t y = 0; y < color.extent.height; ++y) {
            auto* row = bytes.data() + (static_cast<std::size_t>(layer) * color.extent.height + y) * rowBytes;
            for (std::uint32_t x = 0; x < color.extent.width; ++x) std::memcpy(row + static_cast<std::size_t>(x) * color.elementBytes, tiled.data() + texelOffset(layer, x, y), color.elementBytes);
        }
    }
    timing.Mark("detile");
}

void LayeredColorTransfer::RecordToImage(VkCommandBuffer commands) {
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, color.layers};
    copy.imageExtent = {color.extent.width, color.extent.height, 1};
    context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, linear->Handle(), target->Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

void LayeredColorTransfer::RecordFromImage(VkCommandBuffer commands) {
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, color.layers};
    copy.imageExtent = {color.extent.width, color.extent.height, 1};
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, target->Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, linear->Handle(), 1, &copy);
}

void LayeredColorTransfer::WriteBackTracked() {
    PerformanceTimer timing("Graphics.LayeredColor.WriteBack");
    if (tiled.size() != color.bytes) {
        // Uploaded on the GPU: the bytes between texels (padding) come from guest memory as it is.
        tiled.resize(color.bytes);
        GuestMemory::Read(color.address, tiled, 1);
    }
    linear->Invalidate();
    const auto bytes = linear->Bytes();
    const auto rowBytes = static_cast<std::size_t>(color.extent.width) * color.elementBytes;
    for (std::uint32_t layer = 0; layer < color.layers; ++layer) {
        for (std::uint32_t y = 0; y < color.extent.height; ++y) {
            const auto* row = bytes.data() + (static_cast<std::size_t>(layer) * color.extent.height + y) * rowBytes;
            for (std::uint32_t x = 0; x < color.extent.width; ++x) std::memcpy(tiled.data() + texelOffset(layer, x, y), row + static_cast<std::size_t>(x) * color.elementBytes, color.elementBytes);
        }
    }
    timing.Mark("tile");
    GuestMemory::WriteThroughAlias(color.address, tiled.data(), tiled.size());
    timing.Mark("guest_write", color.bytes);
}

}
