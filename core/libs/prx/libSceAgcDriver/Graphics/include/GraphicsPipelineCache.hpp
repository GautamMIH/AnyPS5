#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GRAPHICSPIPELINECACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GRAPHICSPIPELINECACHE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RenderCache.hpp"
#include <list>
#include <map>
#include <string>

namespace AgcDriver::Graphics {

class GraphicsPipelineCache {
public:
    explicit GraphicsPipelineCache(const Context& context) : context(context) {}
    // targets holds one entry per colour slot; unwritten slots are null.
    std::shared_ptr<Pipeline> Get(const State& state, const std::array<std::shared_ptr<ResidentColor>, MaxColorTargets>& targets, const std::shared_ptr<ResidentDepth>& depth, const ShaderResources& resources, std::span<const CompiledShader> shaders);

private:
    // Pipelines are keyed by their shaders, layout and fixed state with the attachment formats (not
    // the attachments, nor the dynamic state): draws on other targets or with another viewport share
    // them. Framebuffers hold the targets whose views they bind.
    struct Entry {
        std::string key;
        std::shared_ptr<const GraphicsPipeline> pipeline;
    };
    struct FramebufferEntry {
        std::vector<std::uint64_t> key;
        std::array<std::shared_ptr<ResidentColor>, MaxColorTargets> targets;
        std::shared_ptr<ResidentDepth> depth;
        std::shared_ptr<const Framebuffer> framebuffer;
    };
    std::shared_ptr<const Framebuffer> framebuffer(const State& state, const std::array<std::shared_ptr<ResidentColor>, MaxColorTargets>& targets, const std::shared_ptr<ResidentDepth>& depth);
    std::shared_ptr<const RenderPass> renderPass(const State& state);
    static constexpr std::size_t MaxPipelines = 4096;
    static constexpr std::size_t MaxFramebuffers = 128;
    Context context;
    std::list<Entry> entries;
    std::map<std::string, std::list<Entry>::iterator> lookup;
    std::list<FramebufferEntry> framebuffers;
    std::map<std::vector<std::uint64_t>, std::list<FramebufferEntry>::iterator> framebufferLookup;
    std::map<std::vector<std::uint64_t>, std::shared_ptr<const RenderPass>> renderPasses;
};

}

#endif
