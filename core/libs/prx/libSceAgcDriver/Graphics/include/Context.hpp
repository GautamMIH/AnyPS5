#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_CONTEXT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_CONTEXT_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace AgcDriver::Graphics {

// Advanced whenever a Vulkan device is destroyed: cached device functions of an older epoch are
// resolved again (a new device may reuse a destroyed one's handle).
inline std::atomic<std::uint64_t> DeviceFunctionEpoch{0};

class TextureDetiler;
class GpuColorTransfer;
class BufferPool;
class TextureCache;
class RenderCache;
class DrawQueue;
class GraphicsPipelineCache;
class DescriptorCache;
class SamplerCache;
class GuestGpuMemory;
class ImageMemory;
class GpuTimestamps;
class DrawProfiler;

inline void Require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error("AGC graphics: " + reason);
}

inline void Check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string("AGC graphics: ") + operation + ": Vulkan result " + std::to_string(result));
}

inline void Require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("AGC graphics: ") + reason);
}

struct Context {
    VkDevice device;
    VkPhysicalDevice physical;
    VkQueue queue;
    VkCommandPool pool;
    PFN_vkGetDeviceProcAddr deviceProc;
    PFN_vkGetPhysicalDeviceFormatProperties formatProperties;
    PFN_vkGetPhysicalDeviceImageFormatProperties imageFormatProperties;
    VkPhysicalDeviceMemoryProperties memory;
    VkPhysicalDeviceLimits limits;
    bool tessellationShader = false;
    bool meshShader = false;
    VkPhysicalDeviceMeshShaderPropertiesEXT meshLimits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    bool depthClipControl = false;
    bool depthRangeUnrestricted = false;
    bool bufferDeviceAddress = false;
    VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    bool fragmentShaderBarycentric = false;
    bool samplerAnisotropy = false;
    bool textureCompressionBC = false;
    TextureDetiler* detiler = nullptr;
    GpuColorTransfer* colorTransfer = nullptr;
    mutable std::shared_ptr<BufferPool> bufferPool;
    TextureCache* textureCache = nullptr;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    RenderCache* renderCache = nullptr;
    DrawQueue* drawQueue = nullptr;
    GraphicsPipelineCache* graphicsPipelines = nullptr;
    mutable std::shared_ptr<DescriptorCache> descriptorCache;
    mutable std::shared_ptr<SamplerCache> samplerCache;
    // VK_KHR_timeline_semaphore: every command batch signals its submission number on it, so a
    // thread can wait for a batch without the locks that guard the batch (null without the feature).
    VkSemaphore submitTimeline = VK_NULL_HANDLE;
    // Opt-in render scale (ANYPS5_RENDER_SCALE=0.5): render passes larger than coarseMinExtent shade
    // one fragment per coarseShading block (VK_KHR_fragment_shading_rate); {1, 1} when off.
    VkExtent2D coarseShading{1, 1};
    VkExtent2D coarseMinExtent{1920, 1080};
    bool depthBounds = false;
    bool depthBiasClamp = false;
    bool independentBlend = false;
    bool geometryShader = false;
    // sampleRateShading enabled: pixel shaders may declare SampleRateShading (SampleId).
    bool sampleRateShading = false;
    bool imageGatherExtended = false;
    bool shaderResourceMinLod = false;
    // VK_EXT_image_view_min_lod enabled (texture MIN_LOD clamps).
    bool imageViewMinLod = false;
    // VK_EXT_primitive_topology_list_restart enabled (primitive restart on list topologies).
    bool primitiveListRestart = false;
    bool storageImageReadWithoutFormat = false;
    bool storageImageWriteWithoutFormat = false;
    // shaderImageInt64Atomics enabled (VK_EXT_shader_image_atomic_int64): 64-bit image atomics.
    bool imageInt64Atomics = false;
    // VK_EXT_sampler_filter_minmax enabled: min and max sampler reduction modes.
    bool samplerFilterMinmax = false;
    // VK_EXT_conservative_rasterization overestimating by at most 1/256 pixel (upstream 29c22442).
    bool conservativeRasterization = false;
    // VK_EXT_subgroup_size_control can require 32-lane mesh subgroups (upstream a3437abd).
    bool meshWave32 = false;
    // VK_EXT_non_seamless_cube_map: samplers with DISABLE_CUBE_WRAP (upstream bf184200).
    bool nonSeamlessCubeMap = false;
    // VK_EXT_provoking_vertex with provokingVertexLast, and whether the mode may differ between
    // pipelines used in one render pass (upstream 3a8cf3a3).
    bool provokingVertexLast = false;
    bool provokingVertexModePerPipeline = false;
    // dualSrcBlend enabled (upstream b8cc4adb).
    bool dualSrcBlend = false;
    // shaderBufferInt64Atomics enabled (upstream a2b049d4).
    bool bufferInt64Atomics = false;
    // Float16, RoundingModeRTE and DenormPreserve for 16-bit floats (upstream 8685c14d).
    bool float16Rte = false;
    // The 8 and 8_8 sRGB formats sampled through UNORM views and decoded in the shader
    // (SrgbDecodeFormats); the shader target carries the same set.
    std::uint32_t srgbDecodeFormats = 0;
    // depthClamp: draws with depth clipping disabled (PA_CL_CLIP_CNTL ZCLIP_*_DISABLE).
    bool depthClamp = false;
    // shaderClipDistance / shaderCullDistance: vertex-stage clip and cull distance outputs.
    bool clipDistance = false;
    bool cullDistance = false;
    // VK_EXT_shader_viewport_index_layer: vertex-pipeline stages may write the layer and viewport index.
    bool viewportIndexLayer = false;
    // shader{Sampled,Storage}ImageArrayDynamicIndexing: image heaps indexed at run time (bindless
    // tables); descriptorIndexing: non-uniformly (VK_EXT_descriptor_indexing).
    bool imageArrayDynamicIndexing = false;
    bool descriptorIndexing = false;
    // Image and sampler tables (ShaderRecompiler::DescriptorRole::ImageTable/SamplerTable): runtime
    // descriptor arrays, partially bound, the image table with a variable descriptor count. The
    // capacities are the table bindings' layout counts (0: the device cannot bind tables).
    bool runtimeDescriptorArray = false;
    bool partiallyBound = false;
    bool variableDescriptorCount = false;
    std::uint32_t imageTableCapacity = 0;
    std::uint32_t samplerTableCapacity = 0;
    // VK_EXT_robustness2 nullDescriptor: null guest storage images bind no view.
    bool nullDescriptors = false;
    // shaderFloat64: the Float64 capability.
    bool float64 = false;
    bool externalMemoryHost = false;
    GuestGpuMemory* guestGpuMemory = nullptr;

    // Call sites pass string literals, so a resolved function is cached per resolver, device and
    // name address: resolving by name is a loader lookup, and draws make dozens of calls.
    template<typename TFunction>
    TFunction Function(const char* name) const {
        Require(deviceProc != nullptr, "missing Vulkan device function resolver");
        struct Resolved {
            PFN_vkGetDeviceProcAddr resolver;
            VkDevice device;
            const char* name;
            std::uint64_t epoch;
            PFN_vkVoidFunction function;
        };
        static thread_local std::array<Resolved, 512> resolved{};
        auto& entry = resolved[(reinterpret_cast<std::uintptr_t>(name) >> 3u) % resolved.size()];
        const auto epoch = DeviceFunctionEpoch.load(std::memory_order_acquire);
        if (entry.name != name || entry.device != device || entry.resolver != deviceProc || entry.epoch != epoch) {
            const auto function = deviceProc(device, name);
            if (function == nullptr) throw std::runtime_error(std::string("AGC graphics: missing Vulkan function: ") + name);
            entry = {deviceProc, device, name, epoch, function};
        }
        return reinterpret_cast<TFunction>(entry.function);
    }

    std::uint32_t MemoryType(std::uint32_t mask, VkMemoryPropertyFlags flags) const {
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((mask & (1u << i)) != 0 && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        }
        throw std::runtime_error("AGC graphics: required Vulkan memory type is unavailable");
    }

    // A type with the preferred flags if one exists, otherwise one with the required flags.
    std::uint32_t MemoryType(std::uint32_t mask, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const {
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((mask & (1u << i)) != 0 && (memory.memoryTypes[i].propertyFlags & (required | preferred)) == (required | preferred)) return i;
        }
        return MemoryType(mask, required);
    }
    // Device-local memory for texture images (see ImageMemory).
    mutable std::shared_ptr<ImageMemory> imageMemory;
    // GPU timing of command batches (ANYPS5_GPU_TIMING=1), null otherwise.
    mutable std::shared_ptr<GpuTimestamps> gpuTimestamps;
    mutable std::shared_ptr<DrawProfiler> drawProfiler;
};

// The layout counts of the image and sampler table bindings this device can afford next to the
// typed heaps (0 without the descriptor indexing features tables need).
std::uint32_t ImageTableCapacity(const Context& context);
std::uint32_t SamplerTableCapacity(const Context& context);

}

#endif
