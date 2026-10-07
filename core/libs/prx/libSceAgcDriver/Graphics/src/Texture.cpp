#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureAddressing.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace AgcDriver::Graphics {

namespace {


std::uint32_t FullArrayLayers(const GuestTextureResource& descriptor) {
    switch (descriptor.dimension) {
        case TextureDimension::k1D:
        case TextureDimension::k2D: return 1u;
        case TextureDimension::k2DArray:
        case TextureDimension::kCube:
        case TextureDimension::k3D: return descriptor.depthOrLastArray + 1u;
    }
    throw std::runtime_error("AGC graphics: Texture encountered an unknown guest texture dimension");
}

VkImageType ImageTypeFor(TextureDimension dimension) {
    return dimension == TextureDimension::k1D ? VK_IMAGE_TYPE_1D : dimension == TextureDimension::k3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
}

VkImageViewType ViewTypeFor(TextureDimension dimension, std::uint32_t viewLayerCount) {
    switch (dimension) {
        case TextureDimension::k1D: return VK_IMAGE_VIEW_TYPE_1D;
        case TextureDimension::k2D: return VK_IMAGE_VIEW_TYPE_2D;
        case TextureDimension::k2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case TextureDimension::kCube: return viewLayerCount == 6u ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case TextureDimension::k3D: return VK_IMAGE_VIEW_TYPE_3D;
    }
    throw std::runtime_error("AGC graphics: Texture encountered an unknown guest texture dimension");
}

float HalfToFloat(std::uint16_t half) {
    const std::uint32_t sign = (half & 0x8000u) << 16u;
    std::uint32_t exponent = (half >> 10u) & 0x1fu;
    std::uint32_t mantissa = half & 0x3ffu;
    std::uint32_t bits = 0;
    if (exponent == 0x1fu) {
        bits = sign | 0x7f800000u | (mantissa << 13u);
    } else if (exponent != 0) {
        bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
    } else if (mantissa != 0) {
        exponent = 113;
        while ((mantissa & 0x400u) == 0) {
            mantissa <<= 1u;
            --exponent;
        }
        bits = sign | (exponent << 23u) | ((mantissa & 0x3ffu) << 13u);
    } else {
        bits = sign;
    }
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float SrgbToLinear(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

template<typename T>
T Load(const std::byte* texel, std::size_t offset = 0) {
    T value{};
    std::memcpy(&value, texel + offset, sizeof(value));
    return value;
}

// A comparison sample compares against the texel's first channel whatever the format (engines bind
// a colour placeholder where no shadow map is used). Vulkan compares depth formats only, so the
// channel is decoded (sRGB to linear, as the sampler would) into a D32 image. Null for formats this
// does not decode.
using ChannelDecoder = float (*)(const std::byte*);
ChannelDecoder CompareChannelDecoder(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8B8A8_UNORM:
            return [](const std::byte* texel) { return static_cast<float>(Load<std::uint8_t>(texel)) / 255.0f; };
        case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8G8_SRGB: case VK_FORMAT_R8G8B8A8_SRGB:
            return [](const std::byte* texel) { return SrgbToLinear(static_cast<float>(Load<std::uint8_t>(texel)) / 255.0f); };
        case VK_FORMAT_B8G8R8A8_UNORM:
            return [](const std::byte* texel) { return static_cast<float>(Load<std::uint8_t>(texel, 2)) / 255.0f; };
        case VK_FORMAT_B8G8R8A8_SRGB:
            return [](const std::byte* texel) { return SrgbToLinear(static_cast<float>(Load<std::uint8_t>(texel, 2)) / 255.0f); };
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return [](const std::byte* texel) { return static_cast<float>(Load<std::uint32_t>(texel) & 0x3ffu) / 1023.0f; };
        case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16B16A16_UNORM:
            return [](const std::byte* texel) { return static_cast<float>(Load<std::uint16_t>(texel)) / 65535.0f; };
        case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT:
            return [](const std::byte* texel) { return HalfToFloat(Load<std::uint16_t>(texel)); };
        case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT:
            return [](const std::byte* texel) { return Load<float>(texel); };
        default:
            return nullptr;
    }
}

std::uint64_t SliceLinearBytes(const std::vector<TileMipLayout>& mips) {
    std::uint64_t bytes = 0;
    for (const auto& mip : mips) bytes = std::max(bytes, mip.linearOffset + mip.linearSize);
    return bytes;
}

}

Texture::Texture(const Context& context, TextureDetiler& detiler, const GuestTextureResource& descriptor, VkComponentMapping components, std::span<const std::byte> snapshot, bool depthCompare) : context(context) {
    PerformanceTimer timing("Graphics.TextureCreate");
    try {
        const auto colorFormat = SampledTextureFormat(context.srgbDecodeFormats, descriptor.format);
        const bool depthLayout = colorFormat == VK_FORMAT_R32_SFLOAT || colorFormat == VK_FORMAT_R16_UNORM;
        const auto decodeChannel = depthCompare && !depthLayout ? CompareChannelDecoder(colorFormat) : nullptr;
        Require(!depthCompare || depthLayout || decodeChannel != nullptr, "comparison sampling of a texture whose first channel is not decoded (guest format " + std::to_string(static_cast<unsigned>(descriptor.format)) + ", Vulkan format " + std::to_string(static_cast<int>(colorFormat)) + ")");
        Require(!depthCompare || descriptor.dimension != TextureDimension::k3D, "comparison sampling does not support 3D depth textures");
        const auto vkFormat = depthCompare ? (colorFormat == VK_FORMAT_R16_UNORM ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT) : colorFormat;
        const VkImageAspectFlags aspect = depthCompare ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        if (IsBlockCompressed(descriptor.format)) {
            Require(context.textureCompressionBC, "device does not support BC compressed textures");
        }

        const auto mips = ComputeMipLayout(descriptor.tileMode, descriptor.format, descriptor.width, descriptor.height, descriptor.mipCount);
        const auto arrayLayers = FullArrayLayers(descriptor);
        const auto elementBytes = BytesPerElement(descriptor.format);

        const auto guestBytes = GuestTextureBytes(descriptor);
        const bool thick = IsThickVolume(descriptor);
        const auto guestSliceBytes = guestBytes / arrayLayers;
        Require(snapshot.size() == guestBytes, "texture snapshot size mismatch");

        // Converted comparison textures upload packed floats: per layer, each mip's texels in rows.
        std::vector<std::uint64_t> convertedMipOffsets;
        std::uint64_t convertedSliceBytes = 0;
        if (decodeChannel) {
            for (std::uint32_t level = 0; level < descriptor.mipCount; ++level) {
                convertedMipOffsets.push_back(convertedSliceBytes);
                convertedSliceBytes += static_cast<std::uint64_t>(std::max(descriptor.width >> level, 1u)) * std::max(descriptor.height >> level, 1u) * sizeof(float);
            }
        }
        const auto sliceLinearBytes = decodeChannel ? convertedSliceBytes : SliceLinearBytes(mips);
        Require(arrayLayers == 0 || sliceLinearBytes <= UINT64_MAX / arrayLayers, "detiled texture buffer size overflows");
        const auto linearBytes = sliceLinearBytes * arrayLayers;

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.flags = descriptor.dimension == TextureDimension::kCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
        imageInfo.imageType = ImageTypeFor(descriptor.dimension);
        imageInfo.format = vkFormat;
        // A volume's slices are detiled like layers but become depth of a single 3D image layer.
        const bool volume = descriptor.dimension == TextureDimension::k3D;
        const auto imageLayers = volume ? 1u : arrayLayers;
        imageInfo.extent = {descriptor.width, descriptor.height, volume ? arrayLayers : 1u};
        imageInfo.mipLevels = descriptor.mipCount;
        imageInfo.arrayLayers = imageLayers;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &imageInfo, nullptr, &image), "vkCreateImage");
        timing.Mark("create_image");

        bindImageMemory("texture");
        timing.Mark("image_memory");

        {
            // The upload runs asynchronously: it is submitted ahead of the draws that sample the
            // texture (one queue, in order), and its buffers and descriptor pool live until it
            // completes (ReleaseUpload). Waiting here stalled every miss on all queued GPU work.
            uploadStaging = std::make_unique<Buffer>(context, static_cast<std::size_t>(guestBytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            auto& staging = *uploadStaging;
            timing.Mark("staging_buffer");
            std::memcpy(staging.Bytes().data(), snapshot.data(), snapshot.size());
            timing.Mark("staging_copy", snapshot.size());
            uploadLinear = std::make_unique<Buffer>(context, static_cast<std::size_t>(linearBytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            auto& linear = *uploadLinear;
            timing.Mark("linear_buffer");
            if (!thick && !decodeChannel) {
                uploadDetiler = &detiler;
                uploadPool = detiler.CreatePool(static_cast<std::uint32_t>(arrayLayers * mips.size()));
            }

            timing.Mark("detiler_pool");
            if (context.drawQueue) context.drawQueue->Flush();
            upload = std::make_unique<CommandBatch>(context);
            upload->Label("texture_upload");
            const auto commands = upload->Handle();
            timing.Mark("batch");

            VkBufferMemoryBarrier stagingReadBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            stagingReadBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            stagingReadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            stagingReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            stagingReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            stagingReadBarrier.buffer = staging.Handle();
            stagingReadBarrier.offset = 0;
            stagingReadBarrier.size = VK_WHOLE_SIZE;

            VkBufferMemoryBarrier linearWriteBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            linearWriteBarrier.srcAccessMask = 0;
            linearWriteBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            linearWriteBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            linearWriteBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            linearWriteBarrier.buffer = linear.Handle();
            linearWriteBarrier.offset = 0;
            linearWriteBarrier.size = VK_WHOLE_SIZE;

            const VkBufferMemoryBarrier preBarriers[] = {stagingReadBarrier, linearWriteBarrier};
            context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2, preBarriers, 0, nullptr);

            if (decodeChannel) {
                auto destination = linear.Bytes();
                for (std::uint32_t layer = 0; layer < arrayLayers; ++layer) {
                    for (std::uint32_t level = 0; level < descriptor.mipCount; ++level) {
                        const auto width = std::max(descriptor.width >> level, 1u);
                        const auto height = std::max(descriptor.height >> level, 1u);
                        auto* out = destination.data() + layer * convertedSliceBytes + convertedMipOffsets[level];
                        for (std::uint32_t y = 0; y < height; ++y) {
                            for (std::uint32_t x = 0; x < width; ++x) {
                                const auto source = static_cast<std::uint64_t>(layer) * guestSliceBytes + TexelOffset(descriptor.tileMode, elementBytes, mips[level], x, y, layer);
                                Require(source + elementBytes <= snapshot.size(), "comparison texel lies outside the texture snapshot");
                                const float value = decodeChannel(snapshot.data() + source);
                                std::memcpy(out + (static_cast<std::size_t>(y) * width + x) * sizeof(float), &value, sizeof(value));
                            }
                        }
                    }
                }
            } else if (thick) {
                // Thick blocks span slices, which the 2D detile shader cannot address.
                const auto& mip = mips.front();
                auto destination = linear.Bytes();
                for (std::uint32_t z = 0; z < arrayLayers; ++z) {
                    for (std::uint32_t y = 0; y < mip.height; ++y) {
                        for (std::uint32_t x = 0; x < mip.width; ++x) {
                            const auto source = ThickVolumeOffset(descriptor.tileMode, elementBytes, descriptor.width, descriptor.height, x, y, z);
                            std::memcpy(destination.data() + z * sliceLinearBytes + mip.linearOffset + (static_cast<std::size_t>(y) * mip.width + x) * elementBytes, snapshot.data() + source, elementBytes);
                        }
                    }
                }
            } else {
                for (std::uint32_t layer = 0; layer < arrayLayers; ++layer) {
                    const auto guestLayerOffset = static_cast<std::uint64_t>(layer) * guestSliceBytes;
                    const auto linearLayerOffset = static_cast<std::uint64_t>(layer) * sliceLinearBytes;
                    for (const auto& mip : mips) {
                        detiler.Dispatch(commands, descriptor.tileMode, elementBytes, staging.Handle(), guestLayerOffset + mip.tiledOffset, linear.Handle(), linearLayerOffset + mip.linearOffset, mip, layer, false, uploadPool);
                    }
                }
            }

            VkBufferMemoryBarrier linearReadBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            linearReadBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            linearReadBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            linearReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            linearReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            linearReadBarrier.buffer = linear.Handle();
            linearReadBarrier.offset = 0;
            linearReadBarrier.size = VK_WHOLE_SIZE;

            VkImageMemoryBarrier toTransferDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toTransferDst.srcAccessMask = 0;
            toTransferDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransferDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toTransferDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransferDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransferDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransferDst.image = image;
            toTransferDst.subresourceRange = {aspect, 0, descriptor.mipCount, 0, imageLayers};
            context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &linearReadBarrier, 1, &toTransferDst);

            std::vector<VkBufferImageCopy> regions;
            regions.reserve(static_cast<std::size_t>(arrayLayers) * mips.size());
            for (std::uint32_t layer = 0; layer < arrayLayers; ++layer) {
                const auto linearLayerOffset = static_cast<std::uint64_t>(layer) * sliceLinearBytes;
                for (std::uint32_t level = 0; level < descriptor.mipCount; ++level) {
                    const auto& mip = mips[level];
                    VkBufferImageCopy region{};
                    region.bufferOffset = linearLayerOffset + (decodeChannel ? convertedMipOffsets[level] : mip.linearOffset);
                    region.bufferRowLength = decodeChannel ? 0u : mip.pitchBytes / elementBytes * BlockWidth(descriptor.format);
                    region.bufferImageHeight = 0;
                    region.imageSubresource = {aspect, level, volume ? 0u : layer, 1};
                    if (volume) region.imageOffset.z = static_cast<std::int32_t>(layer);
                    region.imageOffset = {0, 0, 0};
                    region.imageExtent = {std::max(descriptor.width >> level, 1u), std::max(descriptor.height >> level, 1u), 1u};
                    regions.push_back(region);
                }
            }
            context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, linear.Handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(regions.size()), regions.data());

            VkImageMemoryBarrier toShaderRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toShaderRead.image = image;
            toShaderRead.subresourceRange = toTransferDst.subresourceRange;
            context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShaderRead);

            timing.Mark("record");
            upload->Submit();
            timing.Mark("submit");
        }

        const auto viewLevelCount = descriptor.lastLevel - descriptor.baseLevel + 1u;
        // A 1D or 2D view shows the base layer alone.
        const bool singleLayer = descriptor.viewDimension == TextureDimension::k1D || descriptor.viewDimension == TextureDimension::k2D;
        const auto viewLayerCount = volume || singleLayer ? 1u : arrayLayers - descriptor.baseArray;
        if (descriptor.viewDimension == TextureDimension::kCube) {
            Require(viewLayerCount % 6u == 0, "guest cube texture view does not contain a multiple of 6 array slices");
        }

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = ViewTypeFor(descriptor.viewDimension, viewLayerCount);
        viewInfo.format = vkFormat;
        // A depth view returns the comparison result; swizzles do not apply to it.
        viewInfo.components = depthCompare ? VkComponentMapping{} : components;
        viewInfo.subresourceRange = {aspect, descriptor.baseLevel, viewLevelCount, descriptor.baseArray, viewLayerCount};
        // T# MIN_LOD (upstream 2be78b69); views of render targets show one level, which no clamp changes.
        VkImageViewMinLodCreateInfoEXT minLod{VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT};
        if (const auto clamp = EffectiveMinLod(descriptor); clamp != 0.0f) {
            Require(context.imageViewMinLod, "guest texture descriptor uses a minimum LOD clamp, which needs VK_EXT_image_view_min_lod");
            minLod.minLod = clamp;
            viewInfo.pNext = &minLod;
        }
        Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView");
        noteView(viewInfo);
    } catch (...) {
        release();
        throw;
    }
}

Texture::~Texture() {
    release();
}

void Texture::release() noexcept {
    upload.reset();
    previousUploads.clear();
    releaseUploadResources();
    if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
    if (direct) return;
    if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
    if (imageAllocation) context.imageMemory->Free(*imageAllocation);
    else if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
    imageAllocation.reset();
}

void Texture::bindImageMemory(const char* operation) {
    VkMemoryRequirements requirements{};
    context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
    allocationBytes = requirements.size;
    if (context.imageMemory) {
        imageAllocation = context.imageMemory->Allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, imageAllocation->memory, imageAllocation->offset), operation);
        return;
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory), operation);
    Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), operation);
}

VkImageView Texture::View() const {
    return view;
}

void Texture::releaseUploadResources() noexcept {
    if (uploadPool != VK_NULL_HANDLE && uploadDetiler != nullptr) uploadDetiler->DestroyPool(uploadPool);
    uploadPool = VK_NULL_HANDLE;
    uploadDetiler = nullptr;
    uploadStaging.reset();
    uploadLinear.reset();
}

bool Texture::ReleaseUpload() {
    if (!UploadComplete()) return false;
    releaseUploadResources();
    if (source == nullptr && depthSource == nullptr) upload.reset();
    return true;
}

bool Texture::UploadComplete() const {
    for (const auto& batch : previousUploads)
        if (!batch->IsComplete()) return false;
    return upload == nullptr || upload->IsComplete();
}

}
