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
    bool depthBounds = false;
    bool depthBiasClamp = false;
    bool independentBlend = false;
    bool geometryShader = false;
    bool imageGatherExtended = false;
    bool shaderResourceMinLod = false;
    // VK_EXT_image_view_min_lod enabled (texture MIN_LOD clamps).
    bool imageViewMinLod = false;
    // VK_EXT_primitive_topology_list_restart enabled (primitive restart on list topologies).
    bool primitiveListRestart = false;
    bool storageImageReadWithoutFormat = false;
    bool storageImageWriteWithoutFormat = false;
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
};

}

#endif
