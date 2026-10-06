#include "prx/libSceAgcDriver/Graphics/include/GraphicsPipelineCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <type_traits>
#include <algorithm>
#include <iterator>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <unordered_map>

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
}

// The vertex input layout of a vertex program's attributes: a function of the attributes and the
// device's format support, so it is remembered by their bytes (rebuilding it cost about 0.35 ms of
// a heavy frame). Equal bytes are equal attributes.
const VertexInputLayout& vertexInputLayout(const Context& context, std::span<const ShaderRecompiler::VertexAttribute> attributes) {
    static_assert(std::is_trivially_copyable_v<ShaderRecompiler::VertexAttribute>);
    thread_local std::unordered_map<std::string, VertexInputLayout> layouts;
    std::string key;
    key.reserve(sizeof(VkPhysicalDevice) + attributes.size_bytes());
    append(key, context.physical);
    if (!attributes.empty()) key.append(reinterpret_cast<const char*>(attributes.data()), attributes.size_bytes());
    if (const auto found = layouts.find(key); found != layouts.end()) return found->second;
    if (layouts.size() >= 4096) layouts.clear();
    return layouts.emplace(std::move(key), BuildVertexInputLayout(context, attributes)).first->second;
}

std::string makeKey(const Context& context, const State& state, const ShaderResources& resources, std::span<const CompiledShader> shaders) {
    PerformanceTimer timing("Graphics.PipelineKey");
    std::string key;
    key.reserve(512);
    append(key, state.colorTargetMask);
    for (const auto word : RenderPass::FormatKey(state)) append(key, word);
    append(key, state.hasDepthTarget);
    if (state.hasDepthTarget) {
        // Bias values, bounds and stencil masks and references are dynamic state.
        const auto& depthState = state.depthState;
        append(key, depthState.depthTest);
        append(key, depthState.depthWrite);
        append(key, depthState.depthCompare);
        append(key, depthState.depthBounds);
        append(key, depthState.stencilTest);
        append(key, depthState.depthBias);
        appendStencil(key, depthState.front);
        appendStencil(key, depthState.back);
    }
    append(key, state.rectList);
    append(key, state.topology);
    append(key, state.primitiveRestart);
    append(key, state.negativeOneToOne);
    append(key, state.depthClamp);
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
    const auto& input = vertexInputLayout(context, shaders.front().program->vertexAttributes);
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

std::shared_ptr<Pipeline> GraphicsPipelineCache::Get(const State& state, const std::array<std::shared_ptr<ResidentColor>, MaxColorTargets>& targets, const std::shared_ptr<ResidentDepth>& depth, const ShaderResources& resources, std::span<const CompiledShader> shaders, bool readOnlyDepth) {
    PerformanceTimer timing("Graphics.PipelineCache");
    ValidateDynamicState(context, state);
    auto target = framebuffer(state, targets, depth, readOnlyDepth && depth != nullptr);
    timing.Mark("framebuffer");
    auto key = makeKey(context, state, resources, shaders);
    timing.Mark("key");
    const auto found = lookup.find(key);
    if (found != lookup.end()) {
        const auto it = found->second;
        auto pipeline = it->pipeline;
        entries.splice(entries.end(), entries, it);
        timing.Mark("hit");
        return std::make_shared<Pipeline>(std::move(pipeline), std::move(target));
    }
    timing.Mark("miss");
    const auto traceStarted = std::chrono::steady_clock::now();
    auto pipeline = std::make_shared<const GraphicsPipeline>(context, state, *renderPass(state), resources, shaders);
    timing.Mark("create");
    static const bool traceCreates = std::getenv("APS5_TRACE_PIPELINE_CREATES") != nullptr;
    if (traceCreates) {
        // Whether every stage was compiled into an earlier pipeline (fast linking would cover it).
        static std::set<std::pair<int, std::uintptr_t>> seen;
        static std::array<double, 2> ms{};
        static std::array<std::uint64_t, 2> counts{};
        bool known = true;
        for (const auto& shader : shaders) known = known && seen.count({static_cast<int>(shader.stage), reinterpret_cast<std::uintptr_t>(shader.program->spirv.data())}) != 0;
        for (const auto& shader : shaders) seen.insert({static_cast<int>(shader.stage), reinterpret_cast<std::uintptr_t>(shader.program->spirv.data())});
        ++counts[known];
        ms[known] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - traceStarted).count();
        std::fprintf(stderr, "[pipeline-creates] new-shaders %llu (%.0f ms) known-shaders %llu (%.0f ms)\n", static_cast<unsigned long long>(counts[0]), ms[0], static_cast<unsigned long long>(counts[1]), ms[1]);
    }
    entries.push_back({std::move(key), pipeline});
    try {
        const auto it = std::prev(entries.end());
        Require(lookup.emplace(it->key, it).second, "duplicate graphics pipeline cache key");
    } catch (...) {
        entries.pop_back();
        throw;
    }
    // Pipelines no longer hold targets, so many stay: the games' working sets exceeded the former 128
    // (Spirit and Smurfs recreated 50-200 pipelines a frame).
    while (entries.size() > MaxPipelines) {
        const auto it = std::find_if(entries.begin(), entries.end(), [](const auto& entry) { return entry.pipeline.use_count() == 1; });
        if (it == entries.end()) break;
        lookup.erase(it->key);
        entries.erase(it);
    }
    return std::make_shared<Pipeline>(std::move(pipeline), std::move(target));
}

std::shared_ptr<const RenderPass> GraphicsPipelineCache::renderPass(const State& state, bool readOnlyDepth) {
    auto key = RenderPass::FormatKey(state);
    key.push_back(readOnlyDepth ? 1u : 0u);
    const auto found = renderPasses.find(key);
    if (found != renderPasses.end()) return found->second;
    auto pass = std::make_shared<const RenderPass>(context, state, readOnlyDepth);
    renderPasses.emplace(std::move(key), pass);
    return pass;
}

std::shared_ptr<const Framebuffer> GraphicsPipelineCache::framebuffer(const State& state, const std::array<std::shared_ptr<ResidentColor>, MaxColorTargets>& targets, const std::shared_ptr<ResidentDepth>& depth, bool readOnlyDepth) {
    std::vector<const RenderTarget*> views(state.ColorSlotCount(), nullptr);
    for (std::size_t slot = 0; slot < views.size(); ++slot) {
        if (targets[slot]) views[slot] = &targets[slot]->Target();
    }
    const auto* depthView = depth ? &depth->Target() : nullptr;
    auto key = Framebuffer::Key(state, views, depthView, readOnlyDepth);
    const auto found = framebufferLookup.find(key);
    if (found != framebufferLookup.end()) {
        const auto it = found->second;
        framebuffers.splice(framebuffers.end(), framebuffers, it);
        return it->framebuffer;
    }
    auto created = std::make_shared<const Framebuffer>(context, state, renderPass(state, readOnlyDepth), views, depthView, readOnlyDepth);
    framebuffers.push_back({key, targets, depth, created});
    framebufferLookup.emplace(std::move(key), std::prev(framebuffers.end()));
    while (framebuffers.size() > MaxFramebuffers) {
        const auto it = std::find_if(framebuffers.begin(), framebuffers.end(), [](const auto& entry) { return entry.framebuffer.use_count() == 1; });
        if (it == framebuffers.end()) break;
        framebufferLookup.erase(it->key);
        framebuffers.erase(it);
    }
    return created;
}

}
