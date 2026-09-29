#include "prx/libSceAgcDriver/Graphics/include/GraphicsPipelineCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <type_traits>
#include <algorithm>
#include <iterator>

namespace AgcDriver::Graphics {
namespace {

template<typename TValue>
void append(std::string& key, const TValue& value) {
    static_assert(std::is_trivially_copyable_v<TValue>);
    key.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void appendStencil(std::string& key, const VkStencilOpState& stencil) {
    append(key, stencil.failOp);
    append(key, stencil.passOp);
    append(key, stencil.depthFailOp);
    append(key, stencil.compareOp);
    append(key, stencil.compareMask);
    append(key, stencil.writeMask);
    append(key, stencil.reference);
}

std::string makeKey(const Context& context, const State& state, const std::array<std::shared_ptr<ResidentColor>, MaxColorTargets>& targets, const std::shared_ptr<ResidentDepth>& depth, const ShaderResources& resources, std::span<const CompiledShader> shaders) {
    PerformanceTimer timing("Graphics.PipelineKey");
    std::string key;
    key.reserve(512);
    append(key, state.colorTargetMask);
    for (std::uint32_t slot = 0; slot < state.ColorSlotCount(); ++slot) append(key, targets[slot] ? targets[slot]->Target().View() : VK_NULL_HANDLE);
    append(key, depth ? depth->Target().View() : VK_NULL_HANDLE);
    append(key, state.hasDepthTarget);
    if (state.hasDepthTarget) {
        const auto& depthState = state.depthState;
        append(key, state.depth.format);
        append(key, depthState.depthTest);
        append(key, depthState.depthWrite);
        append(key, depthState.depthCompare);
        append(key, depthState.depthBounds);
        append(key, depthState.minDepthBounds);
        append(key, depthState.maxDepthBounds);
        append(key, depthState.stencilTest);
        append(key, depthState.depthBias);
        append(key, depthState.depthBiasConstant);
        append(key, depthState.depthBiasSlope);
        append(key, depthState.depthBiasClamp);
        appendStencil(key, depthState.front);
        appendStencil(key, depthState.back);
    }
    append(key, state.rectList);
    append(key, state.renderExtent.width);
    append(key, state.renderExtent.height);
    append(key, state.topology);
    append(key, state.viewport.x);
    append(key, state.viewport.y);
    append(key, state.viewport.width);
    append(key, state.viewport.height);
    append(key, state.viewport.minDepth);
    append(key, state.viewport.maxDepth);
    append(key, state.negativeOneToOne);
    append(key, state.scissor.offset.x);
    append(key, state.scissor.offset.y);
    append(key, state.scissor.extent.width);
    append(key, state.scissor.extent.height);
    append(key, state.cullMode);
    append(key, state.frontFace);
    for (std::uint32_t slot = 0; slot < state.ColorSlotCount(); ++slot) {
        const auto& blend = state.blends[slot];
        append(key, blend.blendEnable);
        append(key, blend.srcColorBlendFactor);
        append(key, blend.dstColorBlendFactor);
        append(key, blend.colorBlendOp);
        append(key, blend.srcAlphaBlendFactor);
        append(key, blend.dstAlphaBlendFactor);
        append(key, blend.alphaBlendOp);
        append(key, blend.colorWriteMask);
    }
    for (const auto value : state.blendConstants) append(key, value);
    append(key, state.stages.mesh.has_value());
    append(key, state.stages.tessellation.has_value());
    if (state.stages.mesh) {
        const auto& mesh = *state.stages.mesh;
        append(key, mesh.inputPrimitive);
        append(key, mesh.primitivesPerGroup);
        append(key, mesh.verticesPerGroup);
        append(key, mesh.maxVertices);
        append(key, mesh.maxPrimitives);
        append(key, mesh.threadsPerGroup);
        append(key, mesh.ldsSizeDwords);
        append(key, mesh.provokingVertex);
    }
    if (state.stages.tessellation) {
        const auto& tessellation = *state.stages.tessellation;
        append(key, tessellation.inputControlPoints);
        append(key, tessellation.outputControlPoints);
        append(key, tessellation.domain);
        append(key, tessellation.partitioning);
        append(key, tessellation.outputTopology);
    }
    timing.Mark("state");
    const auto input = BuildVertexInputLayout(context, shaders.front().program->vertexAttributes);
    timing.Mark("vertex_layout");
    append(key, input.bindings.size());
    for (const auto& binding : input.bindings) {
        append(key, binding.binding);
        append(key, binding.stride);
        append(key, binding.inputRate);
    }
    append(key, input.attributes.size());
    for (const auto& attribute : input.attributes) {
        append(key, attribute.location);
        append(key, attribute.binding);
        append(key, attribute.format);
        append(key, attribute.offset);
    }
    append(key, resources.LayoutKey().size());
    const auto layout = std::span(resources.LayoutKey());
    if (!layout.empty()) key.append(reinterpret_cast<const char*>(layout.data()), layout.size_bytes());
    append(key, PushConstantStages(shaders));
    append(key, shaders.size());
    timing.Mark("resources");
    std::uint64_t shaderBytes = 0;
    for (const auto& shader : shaders) {
        append(key, shader.stage);
        // A compiled variant's id stands for its SPIR-V (copying kilobytes of code into every
        // draw's key cost several microseconds); results without one key by their code.
        append(key, shader.program->variantId);
        if (shader.program->variantId != 0) continue;
        append(key, shader.program->spirv.size());
        const auto bytes = std::as_bytes(std::span(shader.program->spirv));
        if (!bytes.empty()) key.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        shaderBytes += bytes.size();
    }
    timing.Mark("shader_code", shaderBytes);
    return key;
}

}

std::shared_ptr<Pipeline> GraphicsPipelineCache::Get(const State& state, const std::array<std::shared_ptr<ResidentColor>, MaxColorTargets>& targets, const std::shared_ptr<ResidentDepth>& depth, const ShaderResources& resources, std::span<const CompiledShader> shaders) {
    PerformanceTimer timing("Graphics.PipelineCache");
    auto key = makeKey(context, state, targets, depth, resources, shaders);
    timing.Mark("key");
    const auto found = lookup.find(key);
    if (found != lookup.end()) {
        const auto it = found->second;
        auto pipeline = it->pipeline;
        entries.splice(entries.end(), entries, it);
        timing.Mark("hit");
        return pipeline;
    }
    timing.Mark("miss");
    std::vector<const RenderTarget*> views(state.ColorSlotCount(), nullptr);
    for (std::size_t slot = 0; slot < views.size(); ++slot) {
        if (targets[slot]) views[slot] = &targets[slot]->Target();
    }
    auto pipeline = std::make_shared<Pipeline>(context, state, views, depth ? &depth->Target() : nullptr, resources, shaders);
    timing.Mark("create");
    entries.push_back({std::move(key), targets, depth, pipeline});
    try {
        const auto it = std::prev(entries.end());
        Require(lookup.emplace(it->key, it).second, "duplicate graphics pipeline cache key");
    } catch (...) {
        entries.pop_back();
        throw;
    }
    while (entries.size() > 128) {
        const auto it = std::find_if(entries.begin(), entries.end(), [](const auto& entry) { return entry.pipeline.use_count() == 1; });
        if (it == entries.end()) break;
        lookup.erase(it->key);
        entries.erase(it);
    }
    return pipeline;
}

}
