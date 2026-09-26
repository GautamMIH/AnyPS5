#include "prx/libSceAgcDriver/Graphics/include/StorageImage.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureAddressing.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <cstring>
#include <limits>

namespace AgcDriver::Graphics {

StorageImage::StorageImage(const Context& context, const GuestTextureResource& resource) : context(context), resource(resource) {
    PerformanceTimer timing("Graphics.StorageImage.Upload");
    Require(resource.dimension == TextureDimension::k2D || resource.dimension == TextureDimension::k2DArray, "storage images support 2D and 2D-array textures only");
    Require(resource.baseLevel == resource.lastLevel, "storage image views must address a single mip");
    Require(!IsBlockCompressed(resource.format), "block-compressed storage images are invalid");
    const auto format = ResolveTextureFormat(resource.format);
    elementBytes = BytesPerElement(resource.format);
    VkFormatProperties properties{};
    context.formatProperties(context.physical, format, &properties);
    constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    Require((properties.optimalTilingFeatures & required) == required, "storage image format does not support storage and copies");

    const auto mips = ComputeMipLayout(resource.tileMode, resource.format, resource.width, resource.height, resource.mipCount);
    mip = mips.at(resource.baseLevel);
    sliceBytes = ComputeSurfaceSize(mips, 1);
    layers = resource.dimension == TextureDimension::k2DArray ? resource.depthOrLastArray - resource.baseArray + 1u : 1u;
    Require(sliceBytes <= std::numeric_limits<std::uint64_t>::max() / (resource.baseArray + layers), "storage image surface size overflow");
    guestBase = resource.baseAddress + sliceBytes * resource.baseArray;
    const auto guestBytes = sliceBytes * layers;
    Require(guestBytes <= std::numeric_limits<std::size_t>::max(), "storage image exceeds the host address space");
    // Resolving as writable hands the range to this image: resident render targets are written
    // back and invalidated first.
    GuestMemory::CheckRange(reinterpret_cast<const void*>(guestBase), static_cast<std::size_t>(guestBytes), 1, true);
    tiled.resize(static_cast<std::size_t>(guestBytes));
    GuestMemory::Read(guestBase, tiled, 1);
    timing.Mark("guest_read", guestBytes);

    const auto layerBytes = static_cast<std::size_t>(mip.width) * mip.height * elementBytes;
    staging = std::make_unique<Buffer>(context, layerBytes * layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto linear = staging->Bytes();
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        for (std::uint32_t y = 0; y < mip.height; ++y) {
            for (std::uint32_t x = 0; x < mip.width; ++x) {
                const auto source = layer * sliceBytes + TexelOffset(resource.tileMode, elementBytes, mip, x, y, layer + resource.baseArray);
                std::memcpy(linear.data() + layer * layerBytes + (static_cast<std::size_t>(y) * mip.width + x) * elementBytes, tiled.data() + source, elementBytes);
            }
        }
    }
    timing.Mark("detile");

    try {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {mip.width, mip.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = layers;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage storage image");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory), "vkAllocateMemory storage image");
        Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory storage image");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = resource.dimension == TextureDimension::k2DArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView storage image");
    } catch (...) {
        release();
        throw;
    }
}

StorageImage::~StorageImage() {
    release();
}

void StorageImage::release() noexcept {
    if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
    if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
    if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
    view = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
}

void StorageImage::RecordUpload(VkCommandBuffer commands) {
    const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    barrier(commands, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &host, 0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    copy.imageExtent = {mip.width, mip.height, 1};
    context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, staging->Handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier toGeneral = toTransfer;
    toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
}

void StorageImage::RecordDownload(VkCommandBuffer commands) {
    const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    copy.imageExtent = {mip.width, mip.height, 1};
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging->Handle(), 1, &copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
}

void StorageImage::WriteBack() {
    PerformanceTimer timing("Graphics.StorageImage.WriteBack");
    staging->Invalidate();
    const auto linear = staging->Bytes();
    const auto layerBytes = static_cast<std::size_t>(mip.width) * mip.height * elementBytes;
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        for (std::uint32_t y = 0; y < mip.height; ++y) {
            for (std::uint32_t x = 0; x < mip.width; ++x) {
                const auto destination = layer * sliceBytes + TexelOffset(resource.tileMode, elementBytes, mip, x, y, layer + resource.baseArray);
                std::memcpy(tiled.data() + destination, linear.data() + layer * layerBytes + (static_cast<std::size_t>(y) * mip.width + x) * elementBytes, elementBytes);
            }
        }
        // Only this mip's bytes are written, so other mips of the surface keep their contents.
        const auto offset = layer * sliceBytes + mip.tiledOffset;
        GuestMemoryBacking::GuestMemoryBackingWrite_nid_postfix(guestBase + offset, tiled.data() + offset, static_cast<std::size_t>(mip.tiledSize));
    }
    timing.Mark("guest_write", mip.tiledSize * layers);
}

bool StorageImage::Overlaps(std::uint64_t address, std::size_t bytes) const {
    const auto size = sliceBytes * layers;
    return address < guestBase + size && guestBase < address + bytes;
}

}
