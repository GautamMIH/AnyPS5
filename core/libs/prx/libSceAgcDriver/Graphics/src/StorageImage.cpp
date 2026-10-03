#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include <cstdio>
#include "prx/libSceAgcDriver/Graphics/include/StorageImage.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureAddressing.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <cstdlib>
#include <cstring>
#include <limits>

namespace AgcDriver::Graphics {
namespace {

// Debug aid: ANYPS5_CHECK_STORAGE_TILING=1 re-tiles each GPU-tiled storage image on the CPU after
// its dispatch and compares the result with what the GPU wrote to guest memory.
bool checkTiling() {
    static const bool enabled = std::getenv("ANYPS5_CHECK_STORAGE_TILING") != nullptr;
    return enabled;
}

}

StorageImage::StorageImage(const Context& context, const GuestTextureResource& resource) : context(context), resource(resource) {
    PerformanceTimer timing("Graphics.StorageImage.Upload");
    Require(resource.dimension == TextureDimension::k2D || resource.dimension == TextureDimension::k2DArray || resource.dimension == TextureDimension::k3D, "storage images support 2D, 2D-array and 3D textures only");
    // A volume's slices are addressed like layers (thin tilings; see DecodeTextureResource).
    volume = resource.dimension == TextureDimension::k3D;
    thick = IsThickVolume(resource);
    Require(resource.baseLevel == resource.lastLevel, "storage image views must address a single mip");
    Require(resource.minLod <= resource.baseLevel * 256u, "guest storage texture descriptor clamps its minimum LOD above the level it addresses, which is not implemented");
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
    layers = resource.dimension == TextureDimension::k2DArray || volume ? resource.depthOrLastArray - resource.baseArray + 1u : 1u;
    Require(sliceBytes <= std::numeric_limits<std::uint64_t>::max() / (resource.baseArray + layers), "storage image surface size overflow");
    guestBase = resource.baseAddress + sliceBytes * resource.baseArray;
    // Thick volumes interleave slices inside blocks, so the whole surface is one unit.
    guestBytes = thick ? GuestTextureBytes(resource) : sliceBytes * layers;
    if (thick) sliceBytes = guestBytes;
    Require(guestBytes <= std::numeric_limits<std::size_t>::max(), "storage image exceeds the host address space");
    // Resolving as writable hands the range to this image: resident render targets are written
    // back and invalidated first.
    {
        // Reads of the texels go through GuestMemory::Read below, which waits as a CPU access.
        const GuestMemory::GpuAccessScope gpuAccess;
        GuestMemory::CheckRange(reinterpret_cast<const void*>(guestBase), static_cast<std::size_t>(guestBytes), 1, true);
    }
    const char* cpuReason = context.guestGpuMemory == nullptr ? "no import" : context.detiler == nullptr ? "no detiler" : thick ? "thick" : "";
    // Thick volumes interleave slices within blocks, which the GPU tiler does not model.
    if (context.guestGpuMemory != nullptr && context.detiler != nullptr && !thick) {
        guest = context.guestGpuMemory->Resolve(guestBase, guestBytes);
        if (!guest) cpuReason = "not imported";
        else if (guest->bytes != guestBytes) cpuReason = "partly imported";
        if (guest && guest->bytes != guestBytes) {
            guest.reset();
            // Split across backing segments (contiguous guest addresses, separate memory): tiled on
            // the GPU through a gathered copy. ANYPS5_NO_STORAGE_GATHER=1 tiles it on the CPU.
            static const bool noGather = std::getenv("ANYPS5_NO_STORAGE_GATHER") != nullptr;
            static const bool testNoMode5 = std::getenv("APS5_TEST_GATHER_NO_MODE5") != nullptr;
            if (!noGather && !(testNoMode5 && static_cast<int>(resource.tileMode) == 5)) pieces = context.guestGpuMemory->ResolvePieces(guestBase, guestBytes);
            if (pieces.size() > 1) {
                gathered = std::make_unique<Buffer>(context, static_cast<std::size_t>(guestBytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                guest = GuestGpuMemory::View{gathered->Handle(), 0, 0, guestBytes};
                cpuReason = "";
            } else {
                pieces.clear();
            }
        }
        // The GPU writes the image's texels into guest memory without passing the guest mappings.
        if (guest) WriteTracker::NoteGpuWrite(guestBase, guestBytes);
    }
    // Debug aid: APS5_TRACE_STORAGE=1 logs every storage image: its shape and whether the GPU tiles it.
    static const bool traceStorage = std::getenv("APS5_TRACE_STORAGE") != nullptr;
    if (traceStorage) std::fprintf(stderr, "[storage] 0x%llx %ux%ux%u mode %d bpe %u %s%s bytes 0x%llx -> %s %s\n", static_cast<unsigned long long>(guestBase), mip.width, mip.height, layers, static_cast<int>(resource.tileMode), elementBytes, volume ? "volume" : "2d", thick ? " thick" : "", static_cast<unsigned long long>(guestBytes), guest ? "gpu" : "cpu", guest ? "" : cpuReason);
    if (guest) {
        const auto deviceUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        tiledStaging = std::make_unique<Buffer>(context, static_cast<std::size_t>(guestBytes), deviceUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        tiledMask = std::make_unique<Buffer>(context, static_cast<std::size_t>(guestBytes), deviceUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ones = std::make_unique<Buffer>(context, static_cast<std::size_t>(mip.linearSize * layers), deviceUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const VkMemoryPropertyFlags linearMemory = checkTiling() ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        staging = std::make_unique<Buffer>(context, static_cast<std::size_t>(mip.linearSize * layers), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, linearMemory);
        if (checkTiling()) {
            tiled.resize(static_cast<std::size_t>(guestBytes));
            GuestMemory::Read(guestBase, tiled, 1);
        }
        // Per layer: a detile on upload; texel and mask tiles and a merge on download.
        tilingPool = context.detiler->CreatePool(layers * 4);
        timing.Mark("gpu_setup");
    } else {
    tiled.resize(static_cast<std::size_t>(guestBytes));
    GuestMemory::Read(guestBase, tiled, 1);
    timing.Mark("guest_read", guestBytes);

    const auto layerBytes = static_cast<std::size_t>(mip.width) * mip.height * elementBytes;
    staging = std::make_unique<Buffer>(context, layerBytes * layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto linear = staging->Bytes();
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        for (std::uint32_t y = 0; y < mip.height; ++y) {
            for (std::uint32_t x = 0; x < mip.width; ++x) {
                const auto source = thick ? ThickVolumeOffset(resource.tileMode, elementBytes, resource.width, resource.height, x, y, layer) : layer * sliceBytes + TexelOffset(resource.tileMode, elementBytes, mip, x, y, layer + resource.baseArray);
                std::memcpy(linear.data() + layer * layerBytes + (static_cast<std::size_t>(y) * mip.width + x) * elementBytes, tiled.data() + source, elementBytes);
            }
        }
    }
    timing.Mark("detile");
    }

    try {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {mip.width, mip.height, volume ? layers : 1u};
        info.mipLevels = 1;
        info.arrayLayers = volume ? 1u : layers;
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
        viewInfo.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : resource.dimension == TextureDimension::k2DArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, volume ? 1u : layers};
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
    if (tilingPool != VK_NULL_HANDLE && context.detiler != nullptr) context.detiler->DestroyPool(tilingPool);
    tilingPool = VK_NULL_HANDLE;
    if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
    if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
    if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
    view = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
}

std::vector<VkBufferImageCopy> StorageImage::imageCopies() const {
    std::vector<VkBufferImageCopy> copies;
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        VkBufferImageCopy copy{};
        copy.bufferOffset = layer * mip.linearSize;
        copy.bufferRowLength = mip.pitchBytes / elementBytes;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, volume ? 0u : layer, 1};
        copy.imageOffset.z = volume ? static_cast<std::int32_t>(layer) : 0;
        copy.imageExtent = {mip.width, mip.height, 1};
        copies.push_back(copy);
    }
    return copies;
}

// Pieces -> gathered (whole range), or gathered -> pieces (only this mip's tiled bytes of each
// layer, so other mips sharing the surface keep what others wrote).
void StorageImage::recordGather(VkCommandBuffer commands, bool scatter) {
    const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    const auto copy = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier(commands, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    std::uint64_t start = 0;
    for (const auto& piece : pieces) {
        const auto end = start + piece.bytes;
        if (!scatter) {
            const VkBufferCopy region{piece.offset, start, piece.bytes};
            copy(commands, piece.buffer, gathered->Handle(), 1, &region);
        } else {
            std::vector<VkBufferCopy> regions;
            for (std::uint32_t layer = 0; layer < layers; ++layer) {
                const auto first = std::max<std::uint64_t>(start, layer * sliceBytes + mip.tiledOffset);
                const auto last = std::min<std::uint64_t>(end, layer * sliceBytes + mip.tiledOffset + mip.tiledSize);
                if (first < last) regions.push_back({first, piece.offset + (first - start), last - first});
            }
            if (!regions.empty()) copy(commands, gathered->Handle(), piece.buffer, static_cast<std::uint32_t>(regions.size()), regions.data());
        }
        start = end;
    }
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
}

// Guest tiled bytes -> detile per layer -> image.
void StorageImage::recordGpuUpload(VkCommandBuffer commands) {
    if (!pieces.empty()) recordGather(commands, false);
    const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    VkMemoryBarrier earlier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    earlier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    earlier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier(commands, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &earlier, 0, nullptr, 0, nullptr);
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        context.detiler->Dispatch(commands, resource.tileMode, elementBytes, guest->buffer, guest->offset + layer * sliceBytes + mip.tiledOffset, staging->Handle(), layer * mip.linearSize, mip, layer + resource.baseArray, false, tilingPool);
    }
    VkMemoryBarrier detiled{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    detiled.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    detiled.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, volume ? 1u : layers};
    barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &detiled, 0, nullptr, 1, &toTransfer);
    const auto copies = imageCopies();
    context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, staging->Handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(copies.size()), copies.data());
    VkImageMemoryBarrier toGeneral = toTransfer;
    toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
}

// Image -> linear -> tile texels and their byte mask per layer -> merge the masked bytes into guest.
void StorageImage::recordGpuDownload(VkCommandBuffer commands) {
    const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    const auto fill = context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer");
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, volume ? 1u : layers};
    VkMemoryBarrier reuse{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    reuse.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    reuse.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reuse, 0, nullptr, 1, &toTransfer);
    const auto copies = imageCopies();
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging->Handle(), static_cast<std::uint32_t>(copies.size()), copies.data());
    fill(commands, ones->Handle(), 0, VK_WHOLE_SIZE, 0xffffffffu);
    for (std::uint32_t layer = 0; layer < layers; ++layer) fill(commands, tiledMask->Handle(), layer * sliceBytes + mip.tiledOffset, mip.tiledSize, 0u);
    VkMemoryBarrier linear{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    linear.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    linear.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &linear, 0, nullptr, 0, nullptr);
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        const auto tiledOffset = layer * sliceBytes + mip.tiledOffset;
        context.detiler->Dispatch(commands, resource.tileMode, elementBytes, staging->Handle(), layer * mip.linearSize, tiledStaging->Handle(), tiledOffset, mip, layer + resource.baseArray, true, tilingPool);
        context.detiler->Dispatch(commands, resource.tileMode, elementBytes, ones->Handle(), layer * mip.linearSize, tiledMask->Handle(), tiledOffset, mip, layer + resource.baseArray, true, tilingPool);
    }
    VkMemoryBarrier tiledBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    tiledBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    tiledBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &tiledBarrier, 0, nullptr, 0, nullptr);
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        const auto tiledOffset = layer * sliceBytes + mip.tiledOffset;
        context.detiler->Merge(commands, tiledStaging->Handle(), tiledOffset, tiledMask->Handle(), tiledOffset, guest->buffer, guest->offset + tiledOffset, mip.tiledSize, tilingPool);
    }
    VkMemoryBarrier merged{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    merged.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    merged.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &merged, 0, nullptr, 0, nullptr);
    if (!pieces.empty()) recordGather(commands, true);
}

void StorageImage::RecordUpload(VkCommandBuffer commands) {
    if (guest) {
        recordGpuUpload(commands);
        return;
    }
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
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, volume ? 1u : layers};
    barrier(commands, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &host, 0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, volume ? 1u : layers};
    copy.imageExtent = {mip.width, mip.height, volume ? layers : 1u};
    context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, staging->Handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier toGeneral = toTransfer;
    toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
}

void StorageImage::RecordDownload(VkCommandBuffer commands) {
    if (guest) {
        recordGpuDownload(commands);
        return;
    }
    const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, volume ? 1u : layers};
    barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, volume ? 1u : layers};
    copy.imageExtent = {mip.width, mip.height, volume ? layers : 1u};
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging->Handle(), 1, &copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
}

void StorageImage::WriteBack() {
    // The download already wrote the GPU tiling into guest memory.
    if (guest) {
        if (!checkTiling()) return;
        staging->Invalidate();
        const auto linear = staging->Bytes();
        std::size_t mismatches = 0;
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            for (std::uint32_t y = 0; y < mip.height; ++y) {
                for (std::uint32_t x = 0; x < mip.width; ++x) {
                    // Only this mip's texels: bytes around them (a shared mip tail) may have been
                    // written by earlier work after the reference copy was taken.
                    const auto destination = layer * sliceBytes + TexelOffset(resource.tileMode, elementBytes, mip, x, y, layer + resource.baseArray);
                    const auto* written = reinterpret_cast<const std::byte*>(guestBase + destination);
                    mismatches += std::memcmp(written, linear.data() + layer * mip.linearSize + static_cast<std::size_t>(y) * mip.pitchBytes + static_cast<std::size_t>(x) * elementBytes, elementBytes) != 0;
                }
            }
        }
        std::fprintf(stderr, "[storage-tiling] %ux%ux%u mode %d bpe %u: %zu mismatching texels\n", mip.width, mip.height, layers, static_cast<int>(resource.tileMode), elementBytes, mismatches);
        return;
    }
    PerformanceTimer timing("Graphics.StorageImage.WriteBack");
    staging->Invalidate();
    const auto linear = staging->Bytes();
    const auto layerBytes = static_cast<std::size_t>(mip.width) * mip.height * elementBytes;
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        for (std::uint32_t y = 0; y < mip.height; ++y) {
            for (std::uint32_t x = 0; x < mip.width; ++x) {
                const auto destination = thick ? ThickVolumeOffset(resource.tileMode, elementBytes, resource.width, resource.height, x, y, layer) : layer * sliceBytes + TexelOffset(resource.tileMode, elementBytes, mip, x, y, layer + resource.baseArray);
                std::memcpy(tiled.data() + destination, linear.data() + layer * layerBytes + (static_cast<std::size_t>(y) * mip.width + x) * elementBytes, elementBytes);
            }
        }
        if (thick) continue;
        // Only this mip's bytes are written, so other mips of the surface keep their contents.
        const auto offset = layer * sliceBytes + mip.tiledOffset;
        GuestMemory::WriteThroughAlias(guestBase + offset, tiled.data() + offset, static_cast<std::size_t>(mip.tiledSize));
    }
    if (thick) GuestMemory::WriteThroughAlias(guestBase, tiled.data(), tiled.size());
    timing.Mark("guest_write", mip.tiledSize * layers);
}

bool StorageImage::Overlaps(std::uint64_t address, std::size_t bytes) const {
    const auto [begin, end] = Range();
    return address < end && begin < address + bytes;
}

std::pair<std::uint64_t, std::uint64_t> StorageImage::Range() const {
    const auto size = thick ? sliceBytes : sliceBytes * layers;
    return {guestBase, guestBase + size};
}

}
