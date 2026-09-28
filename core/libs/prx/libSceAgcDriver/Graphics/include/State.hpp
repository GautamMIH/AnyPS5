#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STATE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STATE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthTargetLayout.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <array>
#include <bit>
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
    std::uint32_t elementBytes = 4;
    ColorTail tail;
    // CB_COLOR_INFO.FAST_CLEAR with the CMASK address and CB_COLOR_CLEAR_WORD0/1; see FastClear.hpp.
    bool fastClear = false;
    std::uint64_t cmaskAddress = 0;
    std::array<std::uint32_t, 2> clearWords{};
    // Slices of an array or volume surface: the draw renders slices [baseLayer, baseLayer +
    // layers) of surfaceSlices, sliceBytes apart; address and bytes cover the rendered slices.
    std::uint32_t layers = 1;
    std::uint32_t baseLayer = 0;
    std::uint32_t surfaceSlices = 1;
    std::uint64_t sliceBytes = 0;
    bool Layered() const { return surfaceSlices > 1; }
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
    // DB_HTILE_DATA_BASE when DB_Z_INFO.TILE_SURFACE_ENABLE is set, else 0 (see FastClear.hpp).
    std::uint64_t htileAddress = 0;
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
    bool depthBias;
    float depthBiasConstant;
    float depthBiasSlope;
    float depthBiasClamp;
    float depthClearValue;
    std::uint32_t stencilClearValue;
};

// CB colour targets (MRT slots) a draw can write.
constexpr std::uint32_t MaxColorTargets = 8;

struct State {
    ShaderStages stages;
    // Slot i is written when bit i of colorTargetMask is set; other slots are unused attachments.
    std::array<ColorTarget, MaxColorTargets> colors{};
    std::uint32_t colorTargetMask = 0;
    bool HasColorTarget() const { return colorTargetMask != 0; }
    // Attachment slots up to the highest written one.
    std::uint32_t ColorSlotCount() const { return static_cast<std::uint32_t>(std::bit_width(colorTargetMask)); }
    DepthTarget depth;
    bool hasDepthTarget;
    // Depth-only draws may bind no pixel shader (the SDK's null PS).
    bool hasFragmentShader = true;
    DepthState depthState;
    bool rectList = false;
    // The geometry stage selects each primitive's render-target layer.
    bool layeredOutput = false;
    // CB_COLOR_CONTROL.MODE ELIMINATE_FAST_CLEAR: the draw only resolves fast-cleared tiles.
    bool eliminateFastClear = false;
    VkExtent2D renderExtent;
    VkPrimitiveTopology topology;
    VkViewport viewport;
    bool negativeOneToOne;
    VkRect2D scissor;
    VkCullModeFlags cullMode;
    VkFrontFace frontFace;
    std::array<VkPipelineColorBlendAttachmentState, MaxColorTargets> blends{};
    std::array<float, 4> blendConstants;
};

ShaderStages DecodeShaderStages(const QueueState& queue);
State DecodeState(const QueueState& queue);

}

#endif
