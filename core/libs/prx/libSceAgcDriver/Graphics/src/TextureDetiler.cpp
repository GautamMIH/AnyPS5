#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/shaders/TextureDetile_spv.h"
#include "prx/libSceAgcDriver/Graphics/shaders/TextureMerge_spv.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace AgcDriver::Graphics {

namespace {

struct Push {
    std::uint32_t srcBase;
    std::uint32_t dstBase;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pitchBytes;
    std::uint32_t blocksPerRow;
    std::uint32_t tail;
    std::uint32_t tailX;
    std::uint32_t tailY;
    std::uint32_t elementBytes;
    std::uint32_t arrayLayer;
};

std::uint32_t BlockBytesFor(TextureTileMode tileMode) {
    switch (tileMode) {
        case TextureTileMode::kLinear: return 0u;
        case TextureTileMode::kStandard256B: return 256u;
        case TextureTileMode::kStandard4KB: return 4096u;
        case TextureTileMode::RenderTarget64KB:
        case TextureTileMode::Depth64KB:
        case TextureTileMode::kStandard64KB: return 65536u;
    }
    throw std::runtime_error("AGC graphics: TextureDetiler encountered an unknown tile mode");
}

std::uint32_t PipelineKey(TextureTileMode tileMode, std::uint32_t elementBytes) {
    return (static_cast<std::uint32_t>(tileMode) << 8) | elementBytes;
}

}

TextureDetiler::TextureDetiler(const Context& context) : context(context) {
    try {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        for (std::uint32_t index = 0; index < bindings.size(); ++index) {
            bindings[index].binding = index;
            bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[index].descriptorCount = 1;
            bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &layoutInfo, nullptr, &descriptorLayout), "vkCreateDescriptorSetLayout");
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(Push);
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout");
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = sizeof(TEXTURE_DETILE_SPV);
        moduleInfo.pCode = TEXTURE_DETILE_SPV;
        Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
    } catch (...) {
        release();
        throw;
    }
}

TextureDetiler::~TextureDetiler() {
    release();
}

void TextureDetiler::createMerge() {
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (std::uint32_t i = 0; i < bindings.size(); ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &layoutInfo, nullptr, &mergeDescriptorLayout), "vkCreateDescriptorSetLayout texture merge");
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 * sizeof(std::uint32_t)};
    VkPipelineLayoutCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineInfo.setLayoutCount = 1;
    pipelineInfo.pSetLayouts = &mergeDescriptorLayout;
    pipelineInfo.pushConstantRangeCount = 1;
    pipelineInfo.pPushConstantRanges = &push;
    Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &pipelineInfo, nullptr, &mergePipelineLayout), "vkCreatePipelineLayout texture merge");
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = sizeof(TEXTURE_MERGE_SPV);
    moduleInfo.pCode = TEXTURE_MERGE_SPV;
    Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &mergeModule), "vkCreateShaderModule texture merge");
    VkComputePipelineCreateInfo createInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    createInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mergeModule, "main", nullptr};
    createInfo.layout = mergePipelineLayout;
    Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, context.pipelineCache, 1, &createInfo, nullptr, &mergePipeline), "vkCreateComputePipelines texture merge");
}

void TextureDetiler::Merge(VkCommandBuffer commands, VkBuffer source, std::uint64_t sourceOffset, VkBuffer mask, std::uint64_t maskOffset, VkBuffer guest, std::uint64_t guestOffset, std::uint64_t bytes, VkDescriptorPool pool) {
    Require(pool != VK_NULL_HANDLE && bytes != 0 && sourceOffset % 4 == 0 && maskOffset % 4 == 0 && guestOffset % 4 == 0 && bytes % 4 == 0, "texture merge requires dword-aligned ranges and a descriptor pool");
    if (mergePipeline == VK_NULL_HANDLE) createMerge();
    const auto alignment = std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 4);
    const std::array<std::uint64_t, 3> offsets{sourceOffset, maskOffset, guestOffset};
    const std::array<VkBuffer, 3> buffers{source, mask, guest};
    std::array<VkDescriptorBufferInfo, 3> infos{};
    std::array<std::uint32_t, 4> push{};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto aligned = offsets[i] - offsets[i] % alignment;
        const auto base = offsets[i] - aligned;
        infos[i] = {buffers[i], aligned, base + bytes};
        Require(base + bytes <= context.limits.maxStorageBufferRange, "texture merge range exceeds device limits");
        push[i] = static_cast<std::uint32_t>(base / 4);
    }
    push[3] = static_cast<std::uint32_t>(bytes / 4);
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = pool;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &mergeDescriptorLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocation, &set), "vkAllocateDescriptorSets texture merge");
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, 3, writes.data(), 0, nullptr);
    context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mergePipeline);
    context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mergePipelineLayout, 0, 1, &set, 0, nullptr);
    context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, mergePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push.data());
    context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, (push[3] + 63u) / 64u, 1, 1);
}

void TextureDetiler::release() noexcept {
    if (mergePipeline) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, mergePipeline, nullptr);
    if (mergeModule) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, mergeModule, nullptr);
    if (mergePipelineLayout) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, mergePipelineLayout, nullptr);
    if (mergeDescriptorLayout) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, mergeDescriptorLayout, nullptr);
    for (const auto pool : descriptorPools) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
    for (const auto& entry : pipelines) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, entry.second, nullptr);
    if (module) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
    if (pipelineLayout) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, pipelineLayout, nullptr);
    if (descriptorLayout) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, descriptorLayout, nullptr);
}

VkPipeline TextureDetiler::pipeline(TextureTileMode tileMode, std::uint32_t elementBytes, bool tile) {
    Require(std::has_single_bit(elementBytes) && elementBytes <= 16u, "unsupported element size for texture detiling");
    const auto key = PipelineKey(tileMode, elementBytes) | (tile ? 0x80000000u : 0u);
    for (const auto& entry : pipelines) {
        if (entry.first == key) return entry.second;
    }
    const std::uint32_t values[4] = {elementBytes, BlockBytesFor(tileMode), tileMode == TextureTileMode::kLinear ? 0u : tileMode == TextureTileMode::RenderTarget64KB ? 2u : tileMode == TextureTileMode::Depth64KB ? 3u : 1u, tile ? 1u : 0u};
    const VkSpecializationMapEntry entries[4] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}, {3, 12, 4}};
    VkSpecializationInfo specialization{};
    specialization.mapEntryCount = 4;
    specialization.pMapEntries = entries;
    specialization.dataSize = sizeof(values);
    specialization.pData = values;
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";
    stage.pSpecializationInfo = &specialization;
    VkComputePipelineCreateInfo createInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    createInfo.stage = stage;
    createInfo.layout = pipelineLayout;
    VkPipeline result = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, context.pipelineCache, 1, &createInfo, nullptr, &result), "vkCreateComputePipelines");
    pipelines.emplace_back(key, result);
    return result;
}

void TextureDetiler::Dispatch(VkCommandBuffer commands, TextureTileMode tileMode, std::uint32_t elementBytes, VkBuffer source, std::uint64_t sourceOffset, VkBuffer destination, std::uint64_t destinationOffset, const TileMipLayout& layout, std::uint32_t arrayLayer, bool tile, VkDescriptorPool pool) {
    Require(commands != VK_NULL_HANDLE, "texture detiling requires an active command buffer");
    Require(source != VK_NULL_HANDLE && destination != VK_NULL_HANDLE, "texture detiling requires source and destination buffers");
    Require(layout.width != 0 && layout.height != 0, "texture detiling requires a non-empty mip layout");
    Require(layout.tiledSize != 0 && layout.linearSize != 0, "texture detiling requires a non-empty mip layout");
    const auto target = pipeline(tileMode, elementBytes, tile);
    const auto alignment = std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 4);
    const auto sourceDescriptorOffset = sourceOffset - sourceOffset % alignment;
    const auto destinationDescriptorOffset = destinationOffset - destinationOffset % alignment;
    const auto sourceBase = sourceOffset - sourceDescriptorOffset;
    const auto destinationBase = destinationOffset - destinationDescriptorOffset;
    Require(sourceBase <= UINT32_MAX && destinationBase <= UINT32_MAX, "texture detiling buffer offset exceeds addressable range");
    const auto sourceRange = (sourceBase + (tile ? layout.linearSize : layout.tiledSize) + 3) / 4 * 4;
    const auto destinationRange = (destinationBase + (tile ? layout.tiledSize : layout.linearSize) + 3) / 4 * 4;
    Require(sourceRange <= context.limits.maxStorageBufferRange && destinationRange <= context.limits.maxStorageBufferRange, "texture detiling buffer range exceeds device limits");
    const auto set = pool == VK_NULL_HANDLE ? allocateSet() : allocateSet(pool);
    const VkDescriptorBufferInfo sourceInfo{source, sourceDescriptorOffset, sourceRange};
    const VkDescriptorBufferInfo destinationInfo{destination, destinationDescriptorOffset, destinationRange};
    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &sourceInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &destinationInfo;
    context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, target);
    context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
    Push push{};
    push.srcBase = static_cast<std::uint32_t>(sourceBase);
    push.dstBase = static_cast<std::uint32_t>(destinationBase);
    push.width = layout.width;
    push.height = layout.height;
    push.pitchBytes = layout.pitchBytes;
    push.blocksPerRow = layout.blocksPerRow;
    push.tail = layout.tail ? 1u : 0u;
    push.tailX = layout.tailX;
    push.tailY = layout.tailY;
    push.elementBytes = elementBytes;
    push.arrayLayer = arrayLayer;
    context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &push);
    const auto groupsX = (layout.width + 7u) / 8u;
    const auto groupsY = (layout.height + 7u) / 8u;
    context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, groupsX, groupsY, 1);
}

}
