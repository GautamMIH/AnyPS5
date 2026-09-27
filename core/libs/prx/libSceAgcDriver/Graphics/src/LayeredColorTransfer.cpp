#include <cstdio>
#include "prx/libSceAgcDriver/Graphics/include/LayeredColorTransfer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureAddressing.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <cstring>

namespace AgcDriver::Graphics {

LayeredColorTransfer::LayeredColorTransfer(const Context& context, const ColorTarget& color) : context(context), color(color) {
    Require(color.Layered() && color.tileMode == ColorTileMode::RenderTarget, "layered colour transfers need a tiled array surface");
    mip = ComputeElementMipLayout(TextureTileMode::RenderTarget64KB, color.elementBytes, color.extent.width, color.extent.height, 1).at(0);
    Require(color.bytes == color.sliceBytes * color.layers, "layered colour target size disagrees with its slices");
    target = std::make_unique<RenderTarget>(context, color, false);
    const auto linearBytes = static_cast<std::size_t>(color.extent.width) * color.extent.height * color.elementBytes * color.layers;
    linear = std::make_unique<Buffer>(context, linearBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
}

std::uint64_t LayeredColorTransfer::texelOffset(std::uint32_t layer, std::uint32_t x, std::uint32_t y) const {
    return layer * color.sliceBytes + TexelOffset(TextureTileMode::RenderTarget64KB, color.elementBytes, mip, x, y, color.baseLayer + layer);
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
    Require(tiled.size() == color.bytes, "layered colour target written back before upload");
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
    GuestMemoryBacking::GuestMemoryBackingWrite_nid_postfix(color.address, tiled.data(), tiled.size());
    timing.Mark("guest_write", color.bytes);
}

}
