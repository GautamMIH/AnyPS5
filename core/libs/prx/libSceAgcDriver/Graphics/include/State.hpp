#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STATE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STATE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthTargetLayout.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include "Recompiler.hpp"

namespace AgcDriver::Graphics {

enum class ShaderPath {
    Vertex,
    Geometry,
    Tessellation,
    TessellationGeometry
};

struct ShaderStages {
    ShaderPath path;
    std::uint32_t registerValue;
    std::uint32_t vertexWaveSize;
    std::uint32_t fragmentWaveSize;
    std::optional<ShaderRecompiler::MeshConfiguration> mesh;
    std::optional<ShaderRecompiler::TessellationConfiguration> tessellation;
};

struct ColorTarget {
    std::uint64_t address;
    VkExtent2D extent;
    VkFormat format;
    std::size_t bytes;
    std::uint8_t componentMapping;
    ColorTileMode tileMode = ColorTileMode::Linear;
};

struct DepthTarget {
    std::uint64_t depthAddress;
    std::uint64_t stencilAddress;
    VkExtent2D extent;
    std::uint32_t depthElementBytes;
    bool hasStencil;
    VkFormat format;
    std::size_t depthBytes;
    std::size_t stencilBytes;
};

struct DepthState {
    bool depthTest;
    bool depthWrite;
    VkCompareOp depthCompare;
    bool depthBounds;
    float minDepthBounds;
    float maxDepthBounds;
    bool stencilTest;
    VkStencilOpState front;
    VkStencilOpState back;
    bool clearDepth;
    bool clearStencil;
    float depthClearValue;
    std::uint32_t stencilClearValue;
};

struct State {
    ShaderStages stages;
    ColorTarget color;
    bool hasColorTarget;
    DepthTarget depth;
    bool hasDepthTarget;
    // Depth-only draws may bind no pixel shader (the SDK's null PS).
    bool hasFragmentShader = true;
    DepthState depthState;
    bool rectList = false;
    VkExtent2D renderExtent;
    VkPrimitiveTopology topology;
    VkViewport viewport;
    bool negativeOneToOne;
    VkRect2D scissor;
    VkCullModeFlags cullMode;
    VkFrontFace frontFace;
    VkPipelineColorBlendAttachmentState blend;
    std::array<float, 4> blendConstants;
};

ShaderStages DecodeShaderStages(const QueueState& queue);
State DecodeState(const QueueState& queue);

}

#endif
