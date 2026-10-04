#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RenderCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"

namespace AgcDriver::Graphics {

Texture::Texture(const Context& context, const std::shared_ptr<ResidentColor>& source, const GuestTextureResource& descriptor, VkComponentMapping components) : context(context), source(source) {
    PerformanceTimer timing("Graphics.RenderTexture");
    try {
        Require(source != nullptr && descriptor.dimension == TextureDimension::k2D && descriptor.mipCount == 1 && descriptor.baseLevel == 0 && descriptor.baseArray == 0, "invalid resident texture view");
        const auto format = ResolveTextureFormat(descriptor.format);
        // The copy reinterprets texels bitwise, so only the element size has to match.
        Require(!IsBlockCompressed(descriptor.format) && BytesPerElement(descriptor.format) == source->Description().elementBytes, "resident texture copy requires the render target's element size");
        VkFormatProperties properties{};
        context.formatProperties(context.physical, format, &properties);
        Require((properties.optimalTilingFeatures & (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) == (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT), "resident texture format does not support sampling and copies");
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {descriptor.width, descriptor.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage resident texture");
        bindImageMemory("resident texture");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.components = components;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView resident texture");
        timing.Mark("allocate");
        extent = info.extent;
        recordCopy();
        timing.Mark("copy");
    } catch (...) {
        release();
        throw;
    }
}

Texture::Texture(const Context& context, const std::shared_ptr<ResidentDepth>& depthSource, const GuestTextureResource& descriptor, VkComponentMapping components) : context(context), depthSource(depthSource) {
    PerformanceTimer timing("Graphics.DepthTexture");
    try {
        Require(depthSource != nullptr && descriptor.dimension == TextureDimension::k2D && descriptor.mipCount == 1 && descriptor.baseLevel == 0 && descriptor.baseArray == 0, "invalid resident depth texture view");
        const auto& depth = depthSource->Description();
        const auto format = ResolveTextureFormat(descriptor.format);
        // The copy reinterprets depth bits, so the texture's element size must be the depth's.
        Require(!IsBlockCompressed(descriptor.format) && BytesPerElement(descriptor.format) == depthSource->HostDepthBytes() && descriptor.width == depth.extent.width && descriptor.height == depth.extent.height, "resident depth texture copy requires the depth plane's size and element size");
        VkFormatProperties properties{};
        context.formatProperties(context.physical, format, &properties);
        Require((properties.optimalTilingFeatures & (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) == (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT), "resident depth texture format does not support sampling and copies");
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {descriptor.width, descriptor.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage resident depth texture");
        bindImageMemory("resident depth texture");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.components = components;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView resident depth texture");
        timing.Mark("allocate");
        const auto texelBytes = static_cast<std::size_t>(descriptor.width) * descriptor.height * depthSource->HostDepthBytes();
        staging = std::make_unique<Buffer>(context, texelBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        // Draws that wrote the depth are submitted first; the queue runs them before this copy.
        timing.Mark("staging");
        extent = info.extent;
        recordCopy();
        timing.Mark("copy");
    } catch (...) {
        release();
        throw;
    }
}

Texture::Texture(const Context& context, const std::shared_ptr<ResidentDepth>& depthSource, VkComponentMapping components, DirectView) : context(context), depthSource(depthSource), direct(true) {
    Require(depthSource != nullptr && depthSource->Sampleable(), "resident depth cannot be sampled directly");
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = depthSource->Target().Image();
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthSource->Description().format;
    viewInfo.components = components;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView direct depth");
}

Texture::Texture(const Context& context, const std::shared_ptr<ResidentDepth>& depthSource, VkComponentMapping components, FeedbackView) : Texture(context, depthSource, components, DirectView{}) {
    feedback = true;
}

VkImageLayout Texture::SampledLayout() const {
    return feedback ? ReadOnlyDepthLayout(depthSource->Description().hasStencil) : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void Texture::PrepareSampling(VkCommandBuffer commands) {
    if (direct && !feedback) depthSource->PrepareSampling(commands);
}

void Texture::Refresh() {
    Require(source != nullptr || depthSource != nullptr, "only resident copies can be refreshed");
    // A direct view always shows the current image.
    if (direct) return;
    recordCopy();
}

// Copies the resident source into the image. Everything runs on one queue in submission order, so
// the leading barrier makes earlier work (draws still sampling the previous contents, the previous
// copy's use of the staging buffer) finish before the copy overwrites the image.
void Texture::recordCopy() {
    PerformanceTimer timing("Graphics.TextureCopy");
    // Draws that wrote the source are submitted first; the queue runs them before this copy.
    if (context.drawQueue) context.drawQueue->Flush();
    timing.Mark("flush");
    std::erase_if(previousUploads, [](const std::unique_ptr<CommandBatch>& batch) { return batch->IsComplete(); });
    if (upload && upload->IsComplete()) {
        upload->Reset();
    } else {
        if (upload) previousUploads.push_back(std::move(upload));
        upload = std::make_unique<CommandBatch>(context);
        timing.Mark("new_batch");
    }
    timing.Mark("batch");
    upload->Label(source ? "texture_copy_color" : "texture_copy_depth");
    const auto commands = upload->Handle();
    const auto pipelineBarrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    VkMemoryBarrier earlier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    earlier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    earlier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    pipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &earlier, 0, nullptr, 0, nullptr);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (source) {
        source->Transition(commands, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        pipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = extent;
        context.Function<PFN_vkCmdCopyImage>("vkCmdCopyImage")(commands, source->Target().Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    } else {
        depthSource->CopyDepth(commands, staging->Handle());
        VkMemoryBarrier copied{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        copied.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copied.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        pipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &copied, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = extent;
        context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, staging->Handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    pipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    timing.Mark("record");
    upload->Submit();
    timing.Mark("submit");
}

}
