#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINE_HPP

#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"

namespace AgcDriver::Graphics {

// A render pass for one combination of attachment formats. Every pass loads and stores its
// attachments in the same layouts, so passes with the same formats are compatible and one serves all
// target sets of those formats (and every pipeline made for it).
class RenderPass {
public:
    // readOnlyDepth: the subpass uses the depth attachment in its read-only depth layout (the depth
    // plane is sampled by the work; stencil stays writable), entered from and returned to the
    // attachment layout by the pass. Layouts do not affect compatibility: pipelines are shared.
    RenderPass(const Context& context, const State& state, bool readOnlyDepth = false);
    ~RenderPass();
    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;
    VkRenderPass Handle() const { return renderPass; }
    // The attachment formats the pass was made for (slot formats, ~0 for unwritten slots, then depth).
    static std::vector<std::uint64_t> FormatKey(const State& state);

private:
    Context context;
    VkRenderPass renderPass = VK_NULL_HANDLE;
};

// The framebuffer over one set of attachment views.
class Framebuffer {
public:
    // targets holds one entry per colour slot up to State::ColorSlotCount(); unwritten slots are null.
    Framebuffer(const Context& context, const State& state, std::shared_ptr<const RenderPass> renderPass, std::span<const RenderTarget* const> targets, const RenderTarget* depthTarget, bool readOnlyDepth = false);
    ~Framebuffer();
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;
    // Extent, layers, formats and views: draws with the same key can share an open render pass.
    const std::vector<std::uint64_t>& PassKey() const { return passKey; }
    void Begin(VkCommandBuffer commands, VkExtent2D extent) const;
    // The key of the framebuffer a draw with these targets uses.
    static std::vector<std::uint64_t> Key(const State& state, std::span<const RenderTarget* const> targets, const RenderTarget* depthTarget, bool readOnlyDepth = false);

private:
    Context context;
    std::shared_ptr<const RenderPass> renderPass;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    std::vector<std::uint64_t> passKey;
};

// A compiled graphics pipeline: shaders, layout and fixed state, usable in every render pass
// compatible with the one it was made for. Viewport, scissor, blend constants, depth bias, depth
// bounds and stencil masks and references are dynamic (SetDynamicState), so draws differing only in
// those share it.
class GraphicsPipeline {
public:
    GraphicsPipeline(const Context& context, const State& state, const RenderPass& renderPass, const ShaderResources& resources, std::span<const CompiledShader> shaders);
    ~GraphicsPipeline();
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    VkPipelineLayout Layout() const { return layout; }
    void Bind(VkCommandBuffer commands, const State& state) const;
    void PushConstants(VkCommandBuffer commands, std::span<const CompiledShader> shaders) const;

private:
    void release() noexcept;
    Context context;
    std::vector<VkShaderModule> _modules;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool depthStencil = false;
};

// A draw's pipeline and framebuffer.
class Pipeline {
public:
    Pipeline(std::shared_ptr<const GraphicsPipeline> pipeline, std::shared_ptr<const Framebuffer> framebuffer) : pipeline(std::move(pipeline)), framebuffer(std::move(framebuffer)) {}
    VkPipelineLayout Layout() const { return pipeline->Layout(); }
    // Begins the render pass over the framebuffer and binds the pipeline with the draw's dynamic state.
    void Begin(VkCommandBuffer commands, const State& state) const;
    // Binds the pipeline inside a render pass another draw with the same PassKey began: the render
    // passes are compatible (same attachment formats) and the framebuffers hold the same views.
    void Bind(VkCommandBuffer commands, const State& state) const;
    const std::vector<std::uint64_t>& PassKey() const { return framebuffer->PassKey(); }
    void PushConstants(VkCommandBuffer commands, std::span<const CompiledShader> shaders) const { pipeline->PushConstants(commands, shaders); }

private:
    std::shared_ptr<const GraphicsPipeline> pipeline;
    std::shared_ptr<const Framebuffer> framebuffer;
};

// The draw's dynamic state is within the device's limits (checked per draw: pipelines no longer
// carry it).
void ValidateDynamicState(const Context& context, const State& state);

void ValidateShaderPair(const ShaderRecompiler::RecompileResult& vertex, const ShaderRecompiler::RecompileResult& fragment);
// Optional device features that unlock SPIR-V capabilities.
struct ShaderDeviceFeatures {
    bool imageGatherExtended = false;
    bool storageImageReadWithoutFormat = false;
    bool storageImageWriteWithoutFormat = false;
    // geometryShader: the Geometry capability, used by pixel shaders reading their layer.
    bool geometryShader = false;
    // shaderResourceMinLod: the MinLod capability, used by samples with an LOD clamp.
    bool minLod = false;
    // shaderClipDistance / shaderCullDistance: the ClipDistance and CullDistance capabilities.
    bool clipDistance = false;
    bool cullDistance = false;
    // VK_EXT_shader_viewport_index_layer: ShaderViewportIndexLayerEXT in vertex-pipeline stages.
    bool viewportIndexLayer = false;
    // VK_EXT_shader_image_atomic_int64: Int64Atomics, Int64ImageEXT and SPV_EXT_shader_image_int64.
    bool imageInt64Atomics = false;
    // sampleRateShading: the SampleRateShading capability, used by pixel shaders reading SampleId.
    bool sampleRateShading = false;
    // Image arrays indexed at run time, and non-uniformly (VK_EXT_descriptor_indexing).
    bool imageArrayDynamicIndexing = false;
    bool descriptorIndexing = false;
    bool float64 = false;

    static ShaderDeviceFeatures Of(const Context& context) {
        return {context.imageGatherExtended, context.storageImageReadWithoutFormat, context.storageImageWriteWithoutFormat, context.geometryShader, context.shaderResourceMinLod, context.clipDistance, context.cullDistance, context.viewportIndexLayer, context.imageInt64Atomics, context.sampleRateShading, context.imageArrayDynamicIndexing, context.descriptorIndexing, context.float64};
    }
};

// Host invocations of a mesh workgroup: a wave64 program on a 32-lane host subgroup runs two guest
// lanes per invocation (the recompiler's laneCount).
inline std::uint32_t MeshInvocations(const State& state, std::uint32_t hostSubgroupSize) {
    const auto lanes = state.stages.vertexWaveSize == 64u && hostSubgroupSize == 32u ? 2u : 1u;
    return state.stages.mesh.has_value() ? state.stages.mesh->threadsPerGroup / lanes : 0u;
}

void ValidateShaders(std::span<const CompiledShader> shaders, const State& state, const VkPhysicalDeviceSubgroupProperties& subgroup, bool fragmentShaderBarycentric, const ShaderDeviceFeatures& features = {});

}

#endif
