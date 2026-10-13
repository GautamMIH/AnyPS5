#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADERS_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "Recompiler.hpp"
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

inline constexpr std::uint32_t PipelinePushConstantBytes = 128;
// Upstream's pipeline libraries give the pixel stage a fixed push slot at byte 128 of a 256-byte
// block (SpirvTarget::fixedPushSlots). This driver links whole pipelines and never sets it, so
// stages pack their push data from offset 0 of one 128-byte block.
inline constexpr std::uint32_t PipelinePushSlotBytes = PipelinePushConstantBytes;
inline constexpr std::uint32_t ComputePushConstantBytes = PipelinePushConstantBytes;

inline std::uint32_t StagePushOffset(std::uint32_t cursor, ShaderRecompiler::ShaderStage stage, bool fixedSlots) {
    return fixedSlots && stage == ShaderRecompiler::ShaderStage::Fragment ? PipelinePushSlotBytes : cursor;
}

struct CompiledShader {
    ShaderRecompiler::ShaderStage stage;
    const ShaderRecompiler::RecompileResult* program;
    std::uint32_t pushConstantOffset;
    // The invocation's push data when it differs from the program's (runtime user data, see
    // ShaderRecompiler::ApplyRuntimeUserData): same size, other user-data words.
    const std::vector<std::byte>* pushData = nullptr;

    [[nodiscard]] const std::vector<std::byte>& PushData() const { return pushData != nullptr ? *pushData : program->pushConstants; }
    // This draw's guest words of program->bindings[binding] (buffer V#s, image T#s, sampler S#s,
    // the flattened SRT or the shader data) and its vertex attributes. Read through these, not the
    // program, so a program can be one shared by every draw of its variant (design:
    // docs/dev/per-draw-dataflow-design.md).
    [[nodiscard]] std::span<const std::uint32_t> Words(std::size_t binding) const { return program->bindings[binding].guestDescriptor; }
    [[nodiscard]] std::span<const ShaderRecompiler::VertexAttribute> VertexAttributes() const { return program->vertexAttributes; }
};

inline VkShaderStageFlagBits VulkanStage(ShaderRecompiler::ShaderStage stage) {
    switch (stage) {
        case ShaderRecompiler::ShaderStage::Vertex:
        case ShaderRecompiler::ShaderStage::Local: return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderRecompiler::ShaderStage::TessellationControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case ShaderRecompiler::ShaderStage::TessellationEvaluation: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        case ShaderRecompiler::ShaderStage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
        case ShaderRecompiler::ShaderStage::Mesh: return VK_SHADER_STAGE_MESH_BIT_EXT;
        case ShaderRecompiler::ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderRecompiler::ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
        default: throw std::runtime_error("AGC graphics: unsupported compiled shader stage");
    }
}

inline VkPipelineStageFlags PipelineStages(std::span<const CompiledShader> shaders) {
    VkPipelineStageFlags result = 0;
    for (const auto& shader : shaders) {
        switch (VulkanStage(shader.stage)) {
            case VK_SHADER_STAGE_VERTEX_BIT: result |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT; break;
            case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: result |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT; break;
            case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: result |= VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT; break;
            case VK_SHADER_STAGE_GEOMETRY_BIT: result |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT; break;
            case VK_SHADER_STAGE_MESH_BIT_EXT: result |= VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT; break;
            case VK_SHADER_STAGE_FRAGMENT_BIT: result |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; break;
            default: throw std::runtime_error("AGC graphics: invalid graphics pipeline stage");
        }
    }
    return result;
}

inline VkShaderStageFlags PushConstantStages(std::span<const CompiledShader> shaders) {
    VkShaderStageFlags result = 0;
    for (const auto& shader : shaders) {
        Require(shader.program != nullptr, "missing compiled shader");
        // A mesh program reads its draw parameters from the push block even without push data.
        if (!shader.program->pushConstants.empty() || shader.stage == ShaderRecompiler::ShaderStage::Mesh) result |= VulkanStage(shader.stage);
    }
    return result;
}

inline std::array<std::byte, PipelinePushConstantBytes> AssemblePushConstants(std::span<const CompiledShader> shaders) {
    std::array<std::byte, PipelinePushConstantBytes> result{};
    std::array<bool, PipelinePushConstantBytes> occupied{};
    for (const auto& shader : shaders) {
        Require(shader.program != nullptr, "missing compiled shader");
        const auto& bytes = shader.PushData();
        Require(bytes.size() == shader.program->pushConstants.size(), "invocation push data differs in size from the program's");
        if (bytes.empty()) continue;
        Require(bytes.size() % 4 == 0 && shader.pushConstantOffset % 4 == 0, "shader push constant range is not DWORD aligned");
        Require(shader.pushConstantOffset < PipelinePushConstantBytes && bytes.size() <= PipelinePushConstantBytes - shader.pushConstantOffset, "shader push constant range lies outside the pipeline push constant block");
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            const auto position = shader.pushConstantOffset + i;
            Require(!occupied[position], "shader push constant ranges of different stages overlap");
            occupied[position] = true;
            result[position] = bytes[i];
        }
    }
    return result;
}

}

#endif
