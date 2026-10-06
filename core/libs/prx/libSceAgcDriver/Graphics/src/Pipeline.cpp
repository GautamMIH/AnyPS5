#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace AgcDriver::Graphics {

std::vector<std::uint64_t> RenderPass::FormatKey(const State& state) {
    const auto slots = state.ColorSlotCount();
    std::vector<std::uint64_t> key{slots};
    for (std::uint32_t slot = 0; slot < slots; ++slot) key.push_back((state.colorTargetMask & (1u << slot)) != 0 ? static_cast<std::uint64_t>(state.colors[slot].format) : ~0ull);
    key.push_back(state.hasDepthTarget ? static_cast<std::uint64_t>(state.depth.format) | (state.depth.hasStencil ? 1ull << 32u : 0u) : ~0ull);
    return key;
}

RenderPass::RenderPass(const Context& context, const State& state, bool readOnlyDepth) : context(context) {
    const auto slots = state.ColorSlotCount();
    Require(slots <= context.limits.maxColorAttachments, "color targets exceed the device's attachment limit");
    VkAttachmentDescription color{};
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription depth{};
    depth.format = state.depth.format;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = state.depth.hasStencil ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = state.depth.hasStencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> references(slots, VkAttachmentReference{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED});
    for (std::uint32_t slot = 0; slot < slots; ++slot) {
        if ((state.colorTargetMask & (1u << slot)) == 0) continue;
        references[slot] = {static_cast<std::uint32_t>(attachments.size()), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        color.format = state.colors[slot].format;
        attachments.push_back(color);
    }
    const VkAttachmentReference depthReference{static_cast<std::uint32_t>(attachments.size()), readOnlyDepth ? ReadOnlyDepthLayout(state.depth.hasStencil) : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    if (state.hasDepthTarget) attachments.push_back(depth);
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = slots;
    subpass.pColorAttachments = references.empty() ? nullptr : references.data();
    subpass.pDepthStencilAttachment = state.hasDepthTarget ? &depthReference : nullptr;
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    passInfo.pAttachments = attachments.empty() ? nullptr : attachments.data();
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    // Earlier passes' attachment writes (and reads) complete before this pass touches the same
    // attachments: the write-after-write order between draws, without draining other stages.
    VkSubpassDependency external{};
    external.srcSubpass = VK_SUBPASS_EXTERNAL;
    external.dstSubpass = 0;
    external.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    external.dstStageMask = external.srcStageMask;
    external.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    external.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    std::array<VkSubpassDependency, 2> dependencies{external, external};
    if (readOnlyDepth) {
        // The entry transition precedes shader reads of the depth; the exit transition (back to the
        // attachment layout) follows them and precedes later attachment and shader access.
        constexpr VkPipelineStageFlags shaders = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].dstStageMask |= shaders;
        dependencies[0].dstAccessMask |= VK_ACCESS_SHADER_READ_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = external.srcStageMask | shaders;
        dependencies[1].dstStageMask = external.dstStageMask | shaders;
        dependencies[1].srcAccessMask = external.srcAccessMask;
        dependencies[1].dstAccessMask = external.dstAccessMask | VK_ACCESS_SHADER_READ_BIT;
    }
    passInfo.dependencyCount = readOnlyDepth ? 2u : 1u;
    passInfo.pDependencies = dependencies.data();
    Check(context.Function<PFN_vkCreateRenderPass>("vkCreateRenderPass")(context.device, &passInfo, nullptr, &renderPass), "vkCreateRenderPass");
}

RenderPass::~RenderPass() {
    if (renderPass) context.Function<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(context.device, renderPass, nullptr);
}

namespace {

// A layered draw spans the layers every attachment has (depth targets have one).
std::uint32_t framebufferLayers(const State& state, std::span<const RenderTarget* const> targets) {
    std::uint32_t layers = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t slot = 0; slot < targets.size(); ++slot) {
        if (targets[slot] != nullptr) layers = std::min(layers, state.colors[slot].Layered() ? state.colors[slot].layers : 1u);
    }
    if (state.hasDepthTarget || layers == std::numeric_limits<std::uint32_t>::max()) layers = 1;
    return layers;
}

}

std::vector<std::uint64_t> Framebuffer::Key(const State& state, std::span<const RenderTarget* const> targets, const RenderTarget* depthTarget, bool readOnlyDepth) {
    // The read-only depth variant begins another render pass: draws on the same views in the two
    // variants never share an open pass.
    std::vector<std::uint64_t> key{state.renderExtent.width, state.renderExtent.height, framebufferLayers(state, targets), readOnlyDepth ? 1u : 0u};
    const auto formats = RenderPass::FormatKey(state);
    key.insert(key.end(), formats.begin(), formats.end());
    for (const auto* target : targets) {
        if (target != nullptr) key.push_back(reinterpret_cast<std::uint64_t>(target->View()));
    }
    if (depthTarget != nullptr) key.push_back(reinterpret_cast<std::uint64_t>(depthTarget->View()));
    return key;
}

Framebuffer::Framebuffer(const Context& context, const State& state, std::shared_ptr<const RenderPass> pass, std::span<const RenderTarget* const> targets, const RenderTarget* depthTarget, bool readOnlyDepth) : context(context), renderPass(std::move(pass)) {
    const auto slots = state.ColorSlotCount();
    Require(targets.size() == slots, "render targets do not match decoded color state");
    for (std::uint32_t slot = 0; slot < slots; ++slot) Require((targets[slot] != nullptr) == ((state.colorTargetMask & (1u << slot)) != 0), "render target does not match decoded color state");
    Require((depthTarget != nullptr) == state.hasDepthTarget, "depth target does not match decoded depth state");
    Require(state.renderExtent.width != 0 && state.renderExtent.height != 0 && state.renderExtent.width <= context.limits.maxFramebufferWidth && state.renderExtent.height <= context.limits.maxFramebufferHeight, "framebuffer extent exceeds device limits");
    Require(state.HasColorTarget() || state.hasDepthTarget || (context.limits.framebufferNoAttachmentsSampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0, "device does not support single-sample rendering without attachments");
    std::vector<VkImageView> views;
    for (const auto* target : targets) {
        if (target != nullptr) views.push_back(target->View());
    }
    if (depthTarget != nullptr) views.push_back(depthTarget->View());
    VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebufferInfo.renderPass = renderPass->Handle();
    framebufferInfo.attachmentCount = static_cast<std::uint32_t>(views.size());
    framebufferInfo.pAttachments = views.empty() ? nullptr : views.data();
    framebufferInfo.width = state.renderExtent.width;
    framebufferInfo.height = state.renderExtent.height;
    framebufferInfo.layers = framebufferLayers(state, targets);
    passKey = Key(state, targets, depthTarget, readOnlyDepth);
    Check(context.Function<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(context.device, &framebufferInfo, nullptr, &framebuffer), "vkCreateFramebuffer");
}

Framebuffer::~Framebuffer() {
    if (framebuffer) context.Function<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(context.device, framebuffer, nullptr);
}

void Framebuffer::Begin(VkCommandBuffer commands, VkExtent2D extent) const {
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = renderPass->Handle();
    begin.framebuffer = framebuffer;
    begin.renderArea = {{0, 0}, extent};
    context.Function<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
}

void ValidateDynamicState(const Context& context, const State& state) {
    const auto& viewport = state.viewport;
    Require(std::isfinite(viewport.minDepth) && std::isfinite(viewport.maxDepth), "non-finite viewport depth range");
    // The message is built only on failure (this runs for every draw).
    if (!(context.depthRangeUnrestricted || (viewport.minDepth >= 0 && viewport.minDepth <= 1 && viewport.maxDepth >= 0 && viewport.maxDepth <= 1)))
        Require(false, "viewport depth [" + std::to_string(viewport.minDepth) + ", " + std::to_string(viewport.maxDepth) + "] outside [0, 1] requires VK_EXT_depth_range_unrestricted");
    Require(std::isfinite(viewport.x) && std::isfinite(viewport.y) && std::isfinite(viewport.width) && std::isfinite(viewport.height), "viewport arithmetic overflow");
    Require(viewport.width <= context.limits.maxViewportDimensions[0] && std::abs(viewport.height) <= context.limits.maxViewportDimensions[1], "viewport dimensions exceed device limits");
    Require(viewport.x >= context.limits.viewportBoundsRange[0] && viewport.x + viewport.width <= context.limits.viewportBoundsRange[1], "viewport X exceeds device bounds");
    Require(std::min(viewport.y, viewport.y + viewport.height) >= context.limits.viewportBoundsRange[0] && std::max(viewport.y, viewport.y + viewport.height) <= context.limits.viewportBoundsRange[1], "viewport Y exceeds device bounds");
    Require(!state.hasDepthTarget || !state.depthState.depthBias || state.depthState.depthBiasClamp == 0.0f || context.depthBiasClamp, "clamped depth bias requires the depthBiasClamp device feature");
}

GraphicsPipeline::GraphicsPipeline(const Context& context, const State& state, const RenderPass& renderPass, const ShaderResources& resources, std::span<const CompiledShader> shaders) : context(context), _modules(shaders.size()), depthStencil(state.hasDepthTarget) {
    const auto slots = state.ColorSlotCount();
    const auto sameBlend = [](const VkPipelineColorBlendAttachmentState& a, const VkPipelineColorBlendAttachmentState& b) {
        return a.blendEnable == b.blendEnable && a.srcColorBlendFactor == b.srcColorBlendFactor && a.dstColorBlendFactor == b.dstColorBlendFactor && a.colorBlendOp == b.colorBlendOp && a.srcAlphaBlendFactor == b.srcAlphaBlendFactor && a.dstAlphaBlendFactor == b.dstAlphaBlendFactor && a.alphaBlendOp == b.alphaBlendOp && a.colorWriteMask == b.colorWriteMask;
    };
    for (std::uint32_t slot = 1; slot < slots; ++slot) Require(context.independentBlend || sameBlend(state.blends[0], state.blends[slot]), "per-target blend state requires the independentBlend device feature");
    Require(!state.hasDepthTarget || !state.depthState.depthBounds || context.depthBounds, "depth bounds test requires the depthBounds device feature");
    ValidateShaders(shaders, state, context.subgroup, context.fragmentShaderBarycentric, ShaderDeviceFeatures::Of(context));
    Require(!state.negativeOneToOne || context.depthClipControl, "negative-one-to-one depth clipping requires VK_EXT_depth_clip_control with depthClipControl enabled");
    if (state.rectList) Require(context.tessellationShader && context.limits.maxTessellationPatchSize >= 4, "rect-list requires tessellation with four output control points");
    if (state.stages.tessellation) {
        Require(context.tessellationShader, "device does not support tessellation shaders");
        Require(state.stages.tessellation->inputControlPoints <= context.limits.maxTessellationPatchSize && state.stages.tessellation->outputControlPoints <= context.limits.maxTessellationPatchSize, "tessellation patch exceeds device limits");
    }
    if (state.stages.mesh) {
        Require(context.meshShader, "device does not support VK_EXT_mesh_shader");
        const auto& mesh = *state.stages.mesh;
        const auto invocations = MeshInvocations(state, context.subgroup.subgroupSize);
        Require(invocations <= context.meshLimits.maxMeshWorkGroupInvocations && invocations <= context.meshLimits.maxMeshWorkGroupSize[0], "mesh workgroup exceeds device limits");
        Require(mesh.maxVertices <= context.meshLimits.maxMeshOutputVertices && mesh.maxPrimitives <= context.meshLimits.maxMeshOutputPrimitives && static_cast<std::uint64_t>(mesh.ldsSizeDwords) * 4 <= context.meshLimits.maxMeshSharedMemorySize, "mesh output or LDS exceeds device limits");
    }
    const auto pushStages = PushConstantStages(shaders);
    Require(pushStages == 0 || context.limits.maxPushConstantsSize >= PipelinePushConstantBytes, "graphics push constant range exceeds device limit");
    try {
        std::vector<VkPipelineShaderStageCreateInfo> stages(shaders.size());
        for (std::uint32_t i = 0; i < shaders.size(); ++i) {
            const auto& shader = *shaders[i].program;
            VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module.codeSize = shader.spirv.size() * sizeof(std::uint32_t);
            module.pCode = shader.spirv.data();
            Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &module, nullptr, &_modules[i]), "vkCreateShaderModule graphics");
            const auto stage = VulkanStage(shaders[i].stage);
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = stage;
            stages[i].module = _modules[i];
            stages[i].pName = "main";
        }
        const auto setLayout = resources.Layout();
        const VkPushConstantRange push{pushStages, 0, PipelinePushConstantBytes};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;
        layoutInfo.pushConstantRangeCount = pushStages != 0 ? 1 : 0;
        layoutInfo.pPushConstantRanges = pushStages != 0 ? &push : nullptr;
        Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout graphics");
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        const auto vertexInput = BuildVertexInputLayout(context, shaders.front().program->vertexAttributes);
        input.vertexBindingDescriptionCount = static_cast<std::uint32_t>(vertexInput.bindings.size());
        input.pVertexBindingDescriptions = vertexInput.bindings.data();
        input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(vertexInput.attributes.size());
        input.pVertexAttributeDescriptions = vertexInput.attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = state.topology;
        const bool listTopology = state.topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST || state.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        assembly.primitiveRestartEnable = state.primitiveRestart && (!listTopology || context.primitiveListRestart) ? VK_TRUE : VK_FALSE;
        VkPipelineViewportStateCreateInfo viewports{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        VkPipelineViewportDepthClipControlCreateInfoEXT depthClip{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
        depthClip.negativeOneToOne = state.negativeOneToOne;
        if (state.negativeOneToOne) viewports.pNext = &depthClip;
        viewports.viewportCount = 1;
        viewports.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = state.cullMode;
        raster.frontFace = state.frontFace;
        raster.lineWidth = 1;
        Require(!state.depthClamp || context.depthClamp, "disabled depth clipping requires the depthClamp device feature");
        raster.depthClampEnable = state.depthClamp ? VK_TRUE : VK_FALSE;
        raster.depthBiasEnable = state.hasDepthTarget && state.depthState.depthBias ? VK_TRUE : VK_FALSE;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = slots;
        blend.pAttachments = slots != 0 ? state.blends.data() : nullptr;
        std::vector<VkDynamicState> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
        if (state.hasDepthTarget) {
            dynamicStates.insert(dynamicStates.end(), {VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE});
            if (context.depthBounds) dynamicStates.push_back(VK_DYNAMIC_STATE_DEPTH_BOUNDS);
        }
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        VkPipelineTessellationStateCreateInfo tessellation{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
        if (state.rectList || state.stages.tessellation) {
            tessellation.patchControlPoints = state.rectList ? 3u : state.stages.tessellation->inputControlPoints;
            pipelineInfo.pTessellationState = &tessellation;
        }
        pipelineInfo.pVertexInputState = state.stages.mesh ? nullptr : &input;
        pipelineInfo.pInputAssemblyState = state.stages.mesh ? nullptr : &assembly;
        pipelineInfo.pViewportState = &viewports;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &samples;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        VkPipelineDepthStencilStateCreateInfo depthStencilInfo{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        if (state.hasDepthTarget) {
            const auto& depthState = state.depthState;
            depthStencilInfo.depthTestEnable = depthState.depthTest;
            depthStencilInfo.depthWriteEnable = depthState.depthWrite;
            depthStencilInfo.depthCompareOp = depthState.depthTest ? depthState.depthCompare : VK_COMPARE_OP_ALWAYS;
            depthStencilInfo.depthBoundsTestEnable = depthState.depthBounds;
            depthStencilInfo.stencilTestEnable = depthState.stencilTest;
            depthStencilInfo.front = depthState.front;
            depthStencilInfo.back = depthState.back;
            pipelineInfo.pDepthStencilState = &depthStencilInfo;
        }
        pipelineInfo.layout = layout;
        pipelineInfo.renderPass = renderPass.Handle();
        Check(context.Function<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(context.device, context.pipelineCache, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateGraphicsPipelines");
    } catch (...) {
        release();
        throw;
    }
}

GraphicsPipeline::~GraphicsPipeline() {
    release();
}

void GraphicsPipeline::release() noexcept {
    if (pipeline) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
    if (layout) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, layout, nullptr);
    for (auto module : _modules) {
        if (module) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
    }
}

void GraphicsPipeline::Bind(VkCommandBuffer commands, const State& state) const {
    context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    context.Function<PFN_vkCmdSetViewport>("vkCmdSetViewport")(commands, 0, 1, &state.viewport);
    context.Function<PFN_vkCmdSetScissor>("vkCmdSetScissor")(commands, 0, 1, &state.scissor);
    context.Function<PFN_vkCmdSetBlendConstants>("vkCmdSetBlendConstants")(commands, state.blendConstants.data());
    if (!depthStencil) return;
    const auto& depthState = state.depthState;
    if (depthState.depthBias) context.Function<PFN_vkCmdSetDepthBias>("vkCmdSetDepthBias")(commands, depthState.depthBiasConstant, depthState.depthBiasClamp, depthState.depthBiasSlope);
    else context.Function<PFN_vkCmdSetDepthBias>("vkCmdSetDepthBias")(commands, 0.0f, 0.0f, 0.0f);
    if (context.depthBounds) {
        // Without the test the bounds are unused; without VK_EXT_depth_range_unrestricted they must lie in [0, 1].
        const bool clamp = !context.depthRangeUnrestricted;
        const auto minBounds = !depthState.depthBounds ? 0.0f : clamp ? std::clamp(depthState.minDepthBounds, 0.0f, 1.0f) : depthState.minDepthBounds;
        const auto maxBounds = !depthState.depthBounds ? 1.0f : clamp ? std::clamp(depthState.maxDepthBounds, 0.0f, 1.0f) : depthState.maxDepthBounds;
        context.Function<PFN_vkCmdSetDepthBounds>("vkCmdSetDepthBounds")(commands, minBounds, maxBounds);
    }
    const auto setStencil = [&](VkStencilFaceFlags face, const VkStencilOpState& stencil) {
        context.Function<PFN_vkCmdSetStencilCompareMask>("vkCmdSetStencilCompareMask")(commands, face, stencil.compareMask);
        context.Function<PFN_vkCmdSetStencilWriteMask>("vkCmdSetStencilWriteMask")(commands, face, stencil.writeMask);
        context.Function<PFN_vkCmdSetStencilReference>("vkCmdSetStencilReference")(commands, face, stencil.reference);
    };
    setStencil(VK_STENCIL_FACE_FRONT_BIT, depthState.front);
    setStencil(VK_STENCIL_FACE_BACK_BIT, depthState.back);
}

void Pipeline::Begin(VkCommandBuffer commands, const State& state) const {
    framebuffer->Begin(commands, state.renderExtent);
    pipeline->Bind(commands, state);
}

void Pipeline::Bind(VkCommandBuffer commands, const State& state) const {
    pipeline->Bind(commands, state);
}

void GraphicsPipeline::PushConstants(VkCommandBuffer commands, std::span<const CompiledShader> shaders) const {
    const auto stages = PushConstantStages(shaders);
    if (stages == 0) return;
    const auto bytes = AssemblePushConstants(shaders);
    context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, layout, stages, 0, PipelinePushConstantBytes, bytes.data());
}

}
