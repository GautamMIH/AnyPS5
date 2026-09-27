#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINE_HPP

#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"

namespace AgcDriver::Graphics {

class Pipeline {
public:
    // targets holds one entry per colour slot up to State::ColorSlotCount(); unwritten slots are null.
    Pipeline(const Context& context, const State& state, std::span<const RenderTarget* const> targets, const RenderTarget* depthTarget, const ShaderResources& resources, std::span<const CompiledShader> shaders);
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    VkPipelineLayout Layout() const;
    void Begin(VkCommandBuffer commands, VkExtent2D extent) const;
    void PushConstants(VkCommandBuffer commands, std::span<const CompiledShader> shaders) const;

private:
    void release() noexcept;
    Context context;
    std::vector<VkShaderModule> _modules;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

void ValidateShaderPair(const ShaderRecompiler::RecompileResult& vertex, const ShaderRecompiler::RecompileResult& fragment);
// Optional device features that unlock SPIR-V capabilities.
struct ShaderDeviceFeatures {
    bool imageGatherExtended = false;
    bool storageImageReadWithoutFormat = false;
    bool storageImageWriteWithoutFormat = false;
    // geometryShader: the Geometry capability, used by pixel shaders reading their layer.
    bool geometryShader = false;

    static ShaderDeviceFeatures Of(const Context& context) {
        return {context.imageGatherExtended, context.storageImageReadWithoutFormat, context.storageImageWriteWithoutFormat, context.geometryShader};
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
