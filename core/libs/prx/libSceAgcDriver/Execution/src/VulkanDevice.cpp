#include "prx/libSceAgcDriver/Graphics/include/GuestGpuMemory.hpp"
#include "BdaAbi.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RenderCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastClear.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GraphicsPipelineCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/BdaFeatures.hpp"
#include "prx/libSceAgcDriver/Execution/include/PresentationScaler.hpp"
#include "prx/libSceAgcDriver/Execution/include/SwapchainState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GpuColorTransfer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DescriptorCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Sampler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/StorageImage.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineCache.hpp"
#include "prx/libc/include/General.hpp"
#include <cctype>
#include <optional>
#include <string_view>
#include <SDL_loadso.h>
#include <SDL_error.h>
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <algorithm>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace AgcDriver {
namespace {

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + ": Vulkan result " + std::to_string(result));
    }
}

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("Vulkan presentation: ") + reason);
}

}

namespace {

// A compute shader's module, pipeline layout and pipeline, kept while dispatches may still run it.
struct ComputePipeline {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr deviceProc = nullptr;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    ComputePipeline() = default;
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ~ComputePipeline() {
        if (pipeline != VK_NULL_HANDLE) reinterpret_cast<PFN_vkDestroyPipeline>(deviceProc(device, "vkDestroyPipeline"))(device, pipeline, nullptr);
        if (layout != VK_NULL_HANDLE) reinterpret_cast<PFN_vkDestroyPipelineLayout>(deviceProc(device, "vkDestroyPipelineLayout"))(device, layout, nullptr);
        if (module != VK_NULL_HANDLE) reinterpret_cast<PFN_vkDestroyShaderModule>(deviceProc(device, "vkDestroyShaderModule"))(device, module, nullptr);
    }
};

}

struct VulkanDevice::State {
    void* library = nullptr;
    PFN_vkGetInstanceProcAddr instanceProc = nullptr;
    PFN_vkGetDeviceProcAddr deviceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    void* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    SwapchainState swapchainState;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    VkFence acquireFence = VK_NULL_HANDLE;
    VkFence renderFence = VK_NULL_HANDLE;
    struct RetiredSwapchain {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        std::vector<VkSemaphore> rendered;
    };
    std::vector<VkSemaphore> rendered;
    std::vector<RetiredSwapchain> retiredSwapchains;
    VkCommandBuffer clearCommands = VK_NULL_HANDLE;
    // The last presentation submission (renderFence) has not been waited for yet.
    bool renderPending = false;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkBuffer uploadBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uploadMemory = VK_NULL_HANDLE;
    void* uploadMapping = nullptr;
    VkDeviceSize uploadSize = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    std::vector<std::uint32_t> capabilities{1};
    std::vector<std::string_view> spirvExtensions;
    bool tessellationShader = false;
    bool meshShader = false;
    bool fragmentShaderBarycentric = false;
    bool depthClipControl = false;
    bool primitiveListRestart = false;
    bool occlusionQueryPrecise = false;
    bool imageViewMinLod = false;
    // 8 and 8_8 sRGB formats the shader decodes (Graphics::SrgbDecodeFormats).
    std::uint32_t srgbDecodeFormats = 0;
    // VK_EXT_shader_image_atomic_int64 with shaderImageInt64Atomics enabled (64-bit image atomics).
    bool imageInt64Atomics = false;
    // VK_KHR_maintenance8: sampling takes a non-constant texel Offset (upstream 301f3b16).
    bool maintenance8 = false;
    // VK_KHR_shader_clock (s_memtime, upstream 1043f90d).
    bool shaderClock = false;
    bool depthRangeUnrestricted = false;
    bool externalMemoryHost = false;
    std::unique_ptr<Graphics::GuestGpuMemory> guestGpuMemory;
    bool depthBounds = false;
    bool depthBiasClamp = false;
    bool independentBlend = false;
    bool geometryShader = false;
    bool imageGatherExtended = false;
    bool shaderResourceMinLod = false;
    bool storageImageReadWithoutFormat = false;
    bool storageImageWriteWithoutFormat = false;
    bool clipDistance = false;
    bool cullDistance = false;
    bool viewportIndexLayer = false;
    bool depthClamp = false;
    bool samplerAnisotropy = false;
    bool textureCompressionBC = false;
    // VK_EXT_sampler_filter_minmax with both filterMinmax properties: min/max sampler reduction.
    bool samplerFilterMinmax = false;
    std::unique_ptr<Graphics::TextureDetiler> detiler;
    std::unique_ptr<Graphics::GpuColorTransfer> colorTransfer;
    std::shared_ptr<Graphics::BufferPool> bufferPool;
    std::shared_ptr<Graphics::ImageMemory> imageMemory;
    std::shared_ptr<Graphics::GpuTimestamps> gpuTimestamps;
    std::shared_ptr<Graphics::DrawProfiler> drawProfiler;
    // Compute pipelines by compiled variant (or SPIR-V), descriptor layout and push stages.
    std::map<std::string, std::shared_ptr<ComputePipeline>> computePipelines;
    std::shared_ptr<Graphics::DescriptorCache> descriptorCache;
    std::shared_ptr<Graphics::SamplerCache> samplerCache;
    std::unique_ptr<Graphics::TextureCache> textureCache;
    std::unique_ptr<Graphics::PipelineCache> pipelineCache;
    std::unique_ptr<Graphics::DrawQueue> drawQueue;
    std::unique_ptr<Graphics::RenderCache> renderCache;
    std::unique_ptr<Graphics::GraphicsPipelineCache> graphicsPipelines;
    VkPhysicalDeviceMeshShaderPropertiesEXT meshLimits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    std::unique_ptr<PresentationScaler> scaler;
    std::unique_ptr<PresentationScaler> rgbaScaler;
    std::map<VkFormat, std::unique_ptr<PresentationScaler>> packedScalers;

    template<typename TFunction>
    TFunction InstanceFunction(const char* name) const {
        auto function = reinterpret_cast<TFunction>(instanceProc(instance, name));
        if (function == nullptr) {
            throw std::runtime_error(std::string("Vulkan instance function missing: ") + name);
        }
        return function;
    }

    template<typename TFunction>
    TFunction DeviceFunction(const char* name) const {
        auto function = reinterpret_cast<TFunction>(deviceProc(device, name));
        if (function == nullptr) {
            throw std::runtime_error(std::string("Vulkan device function missing: ") + name);
        }
        return function;
    }

    void Upload(std::span<const std::byte> pixels) {
        if (uploadSize < pixels.size()) {
            if (uploadMapping) DeviceFunction<PFN_vkUnmapMemory>("vkUnmapMemory")(device, uploadMemory);
            uploadMapping = nullptr;
            if (uploadBuffer) DeviceFunction<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, uploadBuffer, nullptr);
            uploadBuffer = VK_NULL_HANDLE;
            if (uploadMemory) DeviceFunction<PFN_vkFreeMemory>("vkFreeMemory")(device, uploadMemory, nullptr);
            uploadMemory = VK_NULL_HANDLE;
            uploadSize = 0;
            VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer.size = pixels.size();
            buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(DeviceFunction<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &buffer, nullptr, &uploadBuffer), "vkCreateBuffer display upload");
            VkMemoryRequirements requirements{};
            DeviceFunction<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, uploadBuffer, &requirements);
            std::uint32_t memoryType = memoryProperties.memoryTypeCount;
            const auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
                if ((requirements.memoryTypeBits & (1u << i)) != 0 && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags) {
                    memoryType = i;
                    break;
                }
            }
            require(memoryType < memoryProperties.memoryTypeCount, "coherent host upload memory is unavailable");
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memoryType;
            check(DeviceFunction<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &allocation, nullptr, &uploadMemory), "vkAllocateMemory display upload");
            check(DeviceFunction<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, uploadBuffer, uploadMemory, 0), "vkBindBufferMemory display upload");
            check(DeviceFunction<PFN_vkMapMemory>("vkMapMemory")(device, uploadMemory, 0, pixels.size(), 0, &uploadMapping), "vkMapMemory display upload");
            uploadSize = pixels.size();
        }
        std::memcpy(uploadMapping, pixels.data(), pixels.size());
    }

    void DestroyRetiredSwapchains() {
        if (retiredSwapchains.empty()) return;
        const auto destroySemaphore = DeviceFunction<PFN_vkDestroySemaphore>("vkDestroySemaphore");
        const auto destroySwapchain = DeviceFunction<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR");
        for (const auto& retired : retiredSwapchains) {
            for (auto semaphore : retired.rendered) {
                if (semaphore) destroySemaphore(device, semaphore, nullptr);
            }
            destroySwapchain(device, retired.swapchain, nullptr);
        }
        retiredSwapchains.clear();
    }

    ~State() {
        std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
        if (device != VK_NULL_HANDLE) {
            if (drawQueue) drawQueue->Wait();
            if (renderCache) renderCache->Flush();
            const auto idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(deviceProc(device, "vkDeviceWaitIdle"))(device);
            if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST) std::terminate();
            drawQueue.reset();
            computePipelines.clear();
            guestGpuMemory.reset();
            graphicsPipelines.reset();
            renderCache.reset();
            textureCache.reset();
            detiler.reset();
            colorTransfer.reset();
            scaler.reset();
            rgbaScaler.reset();
            packedScalers.clear();
            pipelineCache.reset();
            bufferPool.reset();
            imageMemory.reset();
            gpuTimestamps.reset();
            drawProfiler.reset();
            descriptorCache.reset();
            samplerCache.reset();
            const auto destroyFence = reinterpret_cast<PFN_vkDestroyFence>(deviceProc(device, "vkDestroyFence"));
            if (acquireFence) destroyFence(device, acquireFence, nullptr);
            if (renderFence) destroyFence(device, renderFence, nullptr);
            const auto destroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(deviceProc(device, "vkDestroySemaphore"));
            for (auto semaphore : rendered) {
                if (semaphore) destroySemaphore(device, semaphore, nullptr);
            }
            DestroyRetiredSwapchains();
            if (uploadMapping) reinterpret_cast<PFN_vkUnmapMemory>(deviceProc(device, "vkUnmapMemory"))(device, uploadMemory);
            if (uploadBuffer) reinterpret_cast<PFN_vkDestroyBuffer>(deviceProc(device, "vkDestroyBuffer"))(device, uploadBuffer, nullptr);
            if (uploadMemory) reinterpret_cast<PFN_vkFreeMemory>(deviceProc(device, "vkFreeMemory"))(device, uploadMemory, nullptr);
            if (swapchain) reinterpret_cast<PFN_vkDestroySwapchainKHR>(deviceProc(device, "vkDestroySwapchainKHR"))(device, swapchain, nullptr);
            const auto destroyPool = reinterpret_cast<PFN_vkDestroyCommandPool>(deviceProc(device, "vkDestroyCommandPool"));
            const auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(deviceProc(device, "vkDestroyDevice"));
            Graphics::DropPooledStorageImages(device, deviceProc);
            if (pool != VK_NULL_HANDLE) {
                Graphics::DropRecycledCommandBatches(device, pool, deviceProc);
                destroyPool(device, pool, nullptr);
            }
            destroyDevice(device, nullptr);
            Graphics::DeviceFunctionEpoch.fetch_add(1, std::memory_order_acq_rel);
        }
        if (instance != VK_NULL_HANDLE) {
            if (surface) reinterpret_cast<PFN_vkDestroySurfaceKHR>(instanceProc(instance, "vkDestroySurfaceKHR"))(instance, surface, nullptr);
            reinterpret_cast<PFN_vkDestroyInstance>(instanceProc(instance, "vkDestroyInstance"))(instance, nullptr);
        }
        if (library != nullptr) {
            SDL_UnloadObject(library);
        }
    }
};

VulkanDevice::VulkanDevice(const PresentationWindow* window) : state(std::make_unique<State>()) {
#ifdef _WIN32
    state->library = SDL_LoadObject("vulkan-1.dll");
#else
    state->library = SDL_LoadObject("libvulkan.so.1");
#endif
    if (state->library == nullptr) {
        throw std::runtime_error(std::string("Vulkan loader: ") + SDL_GetError());
    }
    state->instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(state->library, "vkGetInstanceProcAddr"));
    if (state->instanceProc == nullptr) {
        throw std::runtime_error("Vulkan loader: vkGetInstanceProcAddr missing");
    }
    // SDL_CreateWindow with SDL_WINDOW_VULKAN loads and queries the Vulkan loader; on Windows that,
    // during vkCreateInstance on another thread, intermittently made the NVIDIA ICD fail instance
    // creation (upstream e419513f). DisplayWindow::create holds the same lock.
    AgcDriverLockVulkanLoader_nid_postfix();
    struct LoaderUnlock {
        ~LoaderUnlock() { AgcDriverUnlockVulkanLoader_nid_postfix(); }
    } loaderUnlock;
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "AnyPS5 libSceAgcDriver";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &application;
    std::vector<const char*> instanceExtensions;
    if (window != nullptr) {
        require(window->context && window->createSurface && window->getDrawableSize && window->width && window->height, "invalid window descriptor");
        instanceExtensions.assign(window->extensions.begin(), window->extensions.end());
        std::uint32_t availableCount = 0;
        const auto enumerateExtensions = state->InstanceFunction<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties");
        check(enumerateExtensions(nullptr, &availableCount, nullptr), "vkEnumerateInstanceExtensionProperties");
        std::vector<VkExtensionProperties> available(availableCount);
        check(enumerateExtensions(nullptr, &availableCount, available.data()), "vkEnumerateInstanceExtensionProperties");
        for (const auto* name : instanceExtensions) {
            require(name != nullptr, "null instance extension");
            if (std::none_of(available.begin(), available.end(), [&](const auto& item) { return std::strcmp(item.extensionName, name) == 0; })) {
                throw std::runtime_error(std::string("Vulkan presentation: required instance extension missing: ") + name);
            }
        }
        create.enabledExtensionCount = static_cast<std::uint32_t>(instanceExtensions.size());
        create.ppEnabledExtensionNames = instanceExtensions.data();
    }
    check(state->InstanceFunction<PFN_vkCreateInstance>("vkCreateInstance")(&create, nullptr, &state->instance), "vkCreateInstance");
    if (window != nullptr) {
        state->surface = window->createSurface(window->context, state->instance);
        require(state->surface != VK_NULL_HANDLE, "window returned a null surface");
        state->window = window->context;
    }
    state->deviceProc = state->InstanceFunction<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    const auto enumerate = state->InstanceFunction<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    std::uint32_t count = 0;
    check(enumerate(state->instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(count);
    check(enumerate(state->instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
    devices.resize(count);
    // Device preference follows shadPS4 (vk_instance.cpp): Vulkan 1.1 support, then discrete GPUs,
    // then anything but a CPU implementation, then the largest device-local heap. The first
    // suitable device in that order is used. ANYPS5_GPU picks devices whose name contains its text,
    // compared without regard to case (upstream e998b2dd); a number below the device count picks a
    // device by enumeration index instead, as before.
    const auto getProperties = state->InstanceFunction<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
    const auto getMemory = state->InstanceFunction<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties");
    const char* requestedName = std::getenv("ANYPS5_GPU");
    if (requestedName != nullptr && *requestedName == '\0') requestedName = nullptr;
    std::string candidateNames;
    for (auto physical : devices) {
        VkPhysicalDeviceProperties properties{};
        getProperties(physical, &properties);
        if (!candidateNames.empty()) candidateNames += ", ";
        candidateNames += properties.deviceName;
    }
    const auto byIndex = [&]() -> std::optional<std::size_t> {
        if (requestedName == nullptr) return std::nullopt;
        const std::string_view text(requestedName);
        if (text.size() > 9 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
        const auto index = static_cast<std::size_t>(std::strtoul(requestedName, nullptr, 10));
        return index < devices.size() ? std::optional<std::size_t>(index) : std::nullopt;
    }();
    if (byIndex) {
        devices = {devices[*byIndex]};
    } else {
        const auto deviceLocalBytes = [&](VkPhysicalDevice physical) {
            VkPhysicalDeviceMemoryProperties memory{};
            getMemory(physical, &memory);
            VkDeviceSize largest = 0;
            for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
                if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) largest = std::max(largest, memory.memoryHeaps[i].size);
            }
            return largest;
        };
        std::stable_sort(devices.begin(), devices.end(), [&](VkPhysicalDevice left, VkPhysicalDevice right) {
            VkPhysicalDeviceProperties l{};
            VkPhysicalDeviceProperties r{};
            getProperties(left, &l);
            getProperties(right, &r);
            const bool lApi = l.apiVersion >= VK_API_VERSION_1_1;
            const bool rApi = r.apiVersion >= VK_API_VERSION_1_1;
            if (lApi != rApi) return lApi;
            const bool lDiscrete = l.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            const bool rDiscrete = r.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            if (lDiscrete != rDiscrete) return lDiscrete;
            const bool lCpu = l.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
            const bool rCpu = r.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
            if (lCpu != rCpu) return rCpu;
            return deviceLocalBytes(left) > deviceLocalBytes(right);
        });
        if (requestedName != nullptr) {
            const auto matches = [&](VkPhysicalDevice physical) {
                VkPhysicalDeviceProperties properties{};
                getProperties(physical, &properties);
                const std::string_view name(properties.deviceName);
                const std::string_view request(requestedName);
                if (request.size() > name.size()) return false;
                for (std::size_t start = 0; start + request.size() <= name.size(); ++start) {
                    std::size_t index = 0;
                    while (index < request.size() && std::tolower(static_cast<unsigned char>(name[start + index])) == std::tolower(static_cast<unsigned char>(request[index]))) ++index;
                    if (index == request.size()) return true;
                }
                return false;
            };
            devices.erase(std::remove_if(devices.begin(), devices.end(), [&](VkPhysicalDevice physical) { return !matches(physical); }), devices.end());
        }
    }
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    const std::array<const char*, 1> presentationExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    for (auto physical : devices) {
        VkPhysicalDeviceProperties properties{};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_1) {
            continue;
        }
        if (window != nullptr) {
            std::uint32_t extensionCount = 0;
            auto enumerateExtensions = state->InstanceFunction<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
            check(enumerateExtensions(physical, nullptr, &extensionCount, nullptr), "vkEnumerateDeviceExtensionProperties");
            std::vector<VkExtensionProperties> extensions(extensionCount);
            check(enumerateExtensions(physical, nullptr, &extensionCount, extensions.data()), "vkEnumerateDeviceExtensionProperties");
            const bool supported = std::all_of(presentationExtensions.begin(), presentationExtensions.end(), [&](const char* name) {
                return std::any_of(extensions.begin(), extensions.end(), [&](const auto& item) { return std::strcmp(item.extensionName, name) == 0; });
            });
            if (!supported) continue;
        }
        std::uint32_t families = 0;
        auto getFamilies = state->InstanceFunction<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        getFamilies(physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        getFamilies(physical, &families, queues.data());
        for (std::uint32_t i = 0; i < families; ++i) {
            if (queues[i].queueCount != 0 && (queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                if (window != nullptr) {
                    VkBool32 supported = VK_FALSE;
                    check(state->InstanceFunction<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>("vkGetPhysicalDeviceSurfaceSupportKHR")(physical, i, state->surface, &supported), "vkGetPhysicalDeviceSurfaceSupportKHR");
                    if (!supported) continue;
                }
                selected = physical;
                family = i;
                break;
            }
        }
        if (selected != VK_NULL_HANDLE) {
            break;
        }
    }
    if (selected == VK_NULL_HANDLE && requestedName != nullptr) {
        throw std::runtime_error(std::string("Vulkan: no usable device whose name contains ANYPS5_GPU=\"") + requestedName + "\"; devices: " + candidateNames);
    }
    if (selected == VK_NULL_HANDLE) {
        throw std::runtime_error(window ? "Vulkan: no Vulkan 1.1 device with graphics, compute and swapchain presentation" : "Vulkan: no Vulkan 1.1 graphics and compute queue");
    }
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &state->subgroup;
    state->InstanceFunction<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(selected, &properties);
    state->properties = properties.properties;
    state->physical = selected;
    std::fprintf(stderr, "[AnyPS5] Vulkan device: %s\n", state->properties.deviceName);
    state->srgbDecodeFormats = Graphics::SrgbDecodeFormats(state->InstanceFunction<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties"), selected);
    if ((state->subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0) {
        state->capabilities.push_back(spv::CapabilityGroupNonUniform);
        if ((state->subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0) state->capabilities.push_back(spv::CapabilityGroupNonUniformBallot);
        if ((state->subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0) state->capabilities.push_back(spv::CapabilityGroupNonUniformShuffle);
    }
    state->InstanceFunction<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(selected, &state->memoryProperties);
    std::uint32_t extensionCount = 0;
    const auto enumerateDeviceExtensions = state->InstanceFunction<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    check(enumerateDeviceExtensions(selected, nullptr, &extensionCount, nullptr), "vkEnumerateDeviceExtensionProperties");
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    check(enumerateDeviceExtensions(selected, nullptr, &extensionCount, availableExtensions.data()), "vkEnumerateDeviceExtensionProperties");
    const auto hasExtension = [&](const char* name) { return std::any_of(availableExtensions.begin(), availableExtensions.end(), [&](const auto& item) { return std::strcmp(item.extensionName, name) == 0; }); };
    auto byteFeatures = QueryBdaByteFeatures(selected, state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2"), availableExtensions);
    auto bdaFeatures = QueryBdaFeatures(selected, state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2"), availableExtensions);
    require(hasExtension(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME), "VK_KHR_shader_float_controls is unavailable");
    VkPhysicalDeviceFloatControlsProperties floatControls{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceProperties2 floatProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &floatControls};
    state->InstanceFunction<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(selected, &floatProperties);
    require(floatControls.shaderSignedZeroInfNanPreserveFloat32 == VK_TRUE, "shaderSignedZeroInfNanPreserveFloat32 is unavailable");
    const std::array<const char*, 2> meshExtensions{VK_EXT_MESH_SHADER_EXTENSION_NAME, VK_KHR_SPIRV_1_4_EXTENSION_NAME};
    const bool meshAvailable = std::all_of(meshExtensions.begin(), meshExtensions.end(), hasExtension);
    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    if (meshAvailable) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &meshFeatures};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        state->meshShader = meshFeatures.meshShader == VK_TRUE;
        VkPhysicalDeviceProperties2 meshProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &state->meshLimits};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(selected, &meshProperties);
    }
    meshFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    meshFeatures.meshShader = state->meshShader;
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentricFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    if (hasExtension(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &barycentricFeatures};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        state->fragmentShaderBarycentric = barycentricFeatures.fragmentShaderBarycentric == VK_TRUE;
    }
    VkPhysicalDeviceShaderClockFeaturesKHR clockFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
    if (hasExtension(VK_KHR_SHADER_CLOCK_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &clockFeatures};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        // Opt-in (ANYPS5_SHADER_CLOCK=1): with VK_KHR_shader_clock enabled, NVIDIA 615.71.09 crashed in
        // a driver thread (libnvidia-glcore+0xe83ded) early in ~1 of 6 Smurfs runs, never without it
        // (0 of 26). Shaders reading the clock (s_memtime) fail to compile without it.
        state->shaderClock = clockFeatures.shaderSubgroupClock == VK_TRUE && clockFeatures.shaderDeviceClock == VK_TRUE && std::getenv("ANYPS5_SHADER_CLOCK") != nullptr;
    }
    std::vector<const char*> deviceExtensions;
    if (window != nullptr) deviceExtensions.assign(presentationExtensions.begin(), presentationExtensions.end());
    if (state->fragmentShaderBarycentric) {
        deviceExtensions.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
        state->capabilities.push_back(spv::CapabilityFragmentBarycentricKHR);
        state->spirvExtensions.push_back("SPV_KHR_fragment_shader_barycentric");
    }
    // Vertex-pipeline stages writing the layer or viewport index: core in SPIR-V 1.5 (Vulkan 1.2),
    // available to the 1.3/1.4 modules this device takes through VK_EXT_shader_viewport_index_layer
    // (Balatro's vertex shaders write one).
    if (hasExtension(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME)) {
        deviceExtensions.push_back(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME);
        state->capabilities.push_back(spv::CapabilityShaderViewportIndexLayerEXT);
        state->spirvExtensions.push_back("SPV_EXT_shader_viewport_index_layer");
        state->viewportIndexLayer = true;
    }
    if (state->shaderClock) {
        deviceExtensions.push_back(VK_KHR_SHADER_CLOCK_EXTENSION_NAME);
        state->capabilities.push_back(spv::CapabilityShaderClockKHR);
        state->spirvExtensions.push_back("SPV_KHR_shader_clock");
    }
    // 64-bit buffer atomics (the recompiler declares Int64Atomics for them) and, where the device
    // has VK_EXT_shader_image_atomic_int64, 64-bit image atomics (the recompiler refuses those
    // unless the target lists Int64ImageEXT).
    VkPhysicalDeviceShaderAtomicInt64FeaturesKHR atomicInt64Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES_KHR};
    if (hasExtension(VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &atomicInt64Features};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
    }
    const bool bufferInt64Atomics = atomicInt64Features.shaderBufferInt64Atomics == VK_TRUE;
    atomicInt64Features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES_KHR};
    atomicInt64Features.shaderBufferInt64Atomics = VK_TRUE;
    if (bufferInt64Atomics) deviceExtensions.push_back(VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME);
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT imageAtomicInt64Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT};
    if (hasExtension(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &imageAtomicInt64Features};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
    }
    state->imageInt64Atomics = imageAtomicInt64Features.shaderImageInt64Atomics == VK_TRUE;
    imageAtomicInt64Features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT};
    imageAtomicInt64Features.shaderImageInt64Atomics = VK_TRUE;
    if (state->imageInt64Atomics) {
        deviceExtensions.push_back(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
        state->capabilities.push_back(spv::CapabilityInt64ImageEXT);
        state->spirvExtensions.push_back("SPV_EXT_shader_image_int64");
    }
    deviceExtensions.push_back(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
    state->capabilities.push_back(spv::CapabilitySignedZeroInfNanPreserve);
    state->spirvExtensions.push_back("SPV_KHR_float_controls");
    deviceExtensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    deviceExtensions.push_back(VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
    state->capabilities.push_back(4448);
    state->spirvExtensions.push_back("SPV_KHR_8bit_storage");
    state->capabilities.push_back(11);
    state->capabilities.push_back(5347);
    state->spirvExtensions.push_back("SPV_KHR_physical_storage_buffer");
    // Guest memory is shared with the GPU by importing its host pages (GuestGpuMemory).
    state->externalMemoryHost = hasExtension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) && hasExtension(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
    if (state->externalMemoryHost) {
        deviceExtensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
        deviceExtensions.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    }
    if (hasExtension(VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME)) {
        VkPhysicalDeviceSamplerFilterMinmaxPropertiesEXT minmaxProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 minmaxQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &minmaxProperties};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(selected, &minmaxQuery);
        state->samplerFilterMinmax = minmaxProperties.filterMinmaxSingleComponentFormats == VK_TRUE && minmaxProperties.filterMinmaxImageComponentMapping == VK_TRUE;
        if (state->samplerFilterMinmax) deviceExtensions.push_back(VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME);
    }
    state->depthRangeUnrestricted = hasExtension(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
    if (state->depthRangeUnrestricted) deviceExtensions.push_back(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
    VkPhysicalDeviceDepthClipControlFeaturesEXT depthClipFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    if (hasExtension(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &depthClipFeatures};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        state->depthClipControl = depthClipFeatures.depthClipControl == VK_TRUE;
        if (state->depthClipControl) deviceExtensions.push_back(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    }
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT listRestartFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
    if (hasExtension(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &listRestartFeatures};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        state->primitiveListRestart = listRestartFeatures.primitiveTopologyListRestart == VK_TRUE;
        if (state->primitiveListRestart) deviceExtensions.push_back(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
    }
    VkPhysicalDeviceImageViewMinLodFeaturesEXT minLodFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT};
    if (hasExtension(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &minLodFeatures};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        state->imageViewMinLod = minLodFeatures.minLod == VK_TRUE;
        if (state->imageViewMinLod) deviceExtensions.push_back(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
    }
    minLodFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT};
    minLodFeatures.minLod = state->imageViewMinLod ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceMaintenance8FeaturesKHR maintenance8Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR};
    if (hasExtension(VK_KHR_MAINTENANCE_8_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &maintenance8Features};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        state->maintenance8 = maintenance8Features.maintenance8 == VK_TRUE;
        if (state->maintenance8) deviceExtensions.push_back(VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
    }
    maintenance8Features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR};
    maintenance8Features.maintenance8 = VK_TRUE;
    listRestartFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
    listRestartFeatures.primitiveTopologyListRestart = state->primitiveListRestart ? VK_TRUE : VK_FALSE;
    if (state->meshShader) {
        deviceExtensions.insert(deviceExtensions.end(), meshExtensions.begin(), meshExtensions.end());
        state->capabilities.push_back(5283);
        state->spirvExtensions.push_back("SPV_EXT_mesh_shader");
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    VkPhysicalDeviceFeatures available{};
    state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(selected, &available);
    require(available.vertexPipelineStoresAndAtomics && available.fragmentStoresAndAtomics, "graphics shader buffer writes and atomics are unavailable");
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderInt64 = VK_TRUE;
    // 64-bit float VALU ops (upstream b0afa04c) need Float64 with preserved signed zeros, infinities and NaNs.
    enabled.shaderFloat64 = available.shaderFloat64 && floatControls.shaderSignedZeroInfNanPreserveFloat64;
    if (enabled.shaderFloat64) state->capabilities.push_back(spv::CapabilityFloat64);
    enabled.vertexPipelineStoresAndAtomics = VK_TRUE;
    enabled.fragmentStoresAndAtomics = VK_TRUE;
    enabled.tessellationShader = available.tessellationShader;
    state->tessellationShader = enabled.tessellationShader == VK_TRUE;
    if (state->tessellationShader) state->capabilities.push_back(3);
    // The Geometry capability also covers pixel shaders reading their render-target layer.
    enabled.geometryShader = available.geometryShader;
    state->geometryShader = enabled.geometryShader == VK_TRUE;
    if (state->geometryShader) state->capabilities.push_back(spv::CapabilityGeometry);
    require(available.samplerAnisotropy && available.textureCompressionBC, "device lacks sampler anisotropy or BC texture compression support required for texture sampling");
    enabled.samplerAnisotropy = VK_TRUE;
    enabled.textureCompressionBC = VK_TRUE;
    // Vertex shaders export clip and cull distances on the CCDIST vectors (PA_CL_VS_OUT_CNTL);
    // a shader using them fails validation on a device without these capabilities.
    enabled.shaderClipDistance = available.shaderClipDistance;
    state->clipDistance = enabled.shaderClipDistance == VK_TRUE;
    if (state->clipDistance) state->capabilities.push_back(spv::CapabilityClipDistance);
    enabled.shaderCullDistance = available.shaderCullDistance;
    state->cullDistance = enabled.shaderCullDistance == VK_TRUE;
    if (state->cullDistance) state->capabilities.push_back(spv::CapabilityCullDistance);
    enabled.depthClamp = available.depthClamp;
    state->depthClamp = enabled.depthClamp == VK_TRUE;
    enabled.depthBounds = available.depthBounds;
    state->depthBounds = enabled.depthBounds == VK_TRUE;
    enabled.depthBiasClamp = available.depthBiasClamp;
    state->depthBiasClamp = enabled.depthBiasClamp == VK_TRUE;
    enabled.independentBlend = available.independentBlend;
    state->independentBlend = enabled.independentBlend == VK_TRUE;
    enabled.shaderImageGatherExtended = available.shaderImageGatherExtended;
    state->imageGatherExtended = enabled.shaderImageGatherExtended == VK_TRUE;
    // Shaders may then declare ImageGatherExtended (a non-constant gather or, with maintenance8,
    // sample offset).
    if (state->imageGatherExtended) state->capabilities.push_back(spv::CapabilityImageGatherExtended);
    enabled.shaderResourceMinLod = available.shaderResourceMinLod;
    // Advertised to the recompiler's target as upstream does (image sample clamps emit MinLod).
    if (enabled.shaderResourceMinLod) state->capabilities.push_back(spv::CapabilityMinLod);
    enabled.occlusionQueryPrecise = available.occlusionQueryPrecise;
    state->occlusionQueryPrecise = enabled.occlusionQueryPrecise == VK_TRUE;
    state->shaderResourceMinLod = enabled.shaderResourceMinLod == VK_TRUE;
    enabled.shaderStorageImageReadWithoutFormat = available.shaderStorageImageReadWithoutFormat;
    enabled.shaderStorageImageWriteWithoutFormat = available.shaderStorageImageWriteWithoutFormat;
    state->storageImageReadWithoutFormat = enabled.shaderStorageImageReadWithoutFormat == VK_TRUE;
    state->storageImageWriteWithoutFormat = enabled.shaderStorageImageWriteWithoutFormat == VK_TRUE;
    state->samplerAnisotropy = true;
    state->textureCompressionBC = true;
    deviceInfo.pEnabledFeatures = &enabled;
    deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    if (state->meshShader) {
        meshFeatures.pNext = const_cast<void*>(deviceInfo.pNext);
        deviceInfo.pNext = &meshFeatures;
    }
    if (state->depthClipControl) {
        depthClipFeatures.pNext = const_cast<void*>(deviceInfo.pNext);
        deviceInfo.pNext = &depthClipFeatures;
    }
    if (state->primitiveListRestart) {
        listRestartFeatures.pNext = const_cast<void*>(deviceInfo.pNext);
        deviceInfo.pNext = &listRestartFeatures;
    }
    if (state->imageViewMinLod) {
        minLodFeatures.pNext = const_cast<void*>(deviceInfo.pNext);
        deviceInfo.pNext = &minLodFeatures;
    }
    if (state->maintenance8) {
        maintenance8Features.pNext = const_cast<void*>(deviceInfo.pNext);
        deviceInfo.pNext = &maintenance8Features;
    }
    byteFeatures.pNext = const_cast<void*>(deviceInfo.pNext);
    if (state->fragmentShaderBarycentric) {
        barycentricFeatures.pNext = byteFeatures.pNext;
        byteFeatures.pNext = &barycentricFeatures;
    }
    if (state->shaderClock) {
        clockFeatures.pNext = byteFeatures.pNext;
        byteFeatures.pNext = &clockFeatures;
    }
    if (bufferInt64Atomics) {
        atomicInt64Features.pNext = byteFeatures.pNext;
        byteFeatures.pNext = &atomicInt64Features;
    }
    if (state->imageInt64Atomics) {
        imageAtomicInt64Features.pNext = byteFeatures.pNext;
        byteFeatures.pNext = &imageAtomicInt64Features;
    }
    bdaFeatures.pNext = &byteFeatures;
    deviceInfo.pNext = &bdaFeatures;
    check(state->InstanceFunction<PFN_vkCreateDevice>("vkCreateDevice")(selected, &deviceInfo, nullptr, &state->device), "vkCreateDevice");
    state->DeviceFunction<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(state->device, family, 0, &state->queue);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = family;
    check(state->DeviceFunction<PFN_vkCreateCommandPool>("vkCreateCommandPool")(state->device, &poolInfo, nullptr, &state->pool), "vkCreateCommandPool");
    state->imageMemory = std::make_shared<Graphics::ImageMemory>(state->device, state->deviceProc, state->memoryProperties);
    if (std::getenv("ANYPS5_GPU_TIMING") != nullptr) {
        if (state->properties.limits.timestampComputeAndGraphics) state->gpuTimestamps = std::make_shared<Graphics::GpuTimestamps>(state->device, state->deviceProc, state->properties.limits.timestampPeriod);
        else std::fprintf(stderr, "[AnyPS5] ANYPS5_GPU_TIMING: the device has no graphics timestamps\n");
    }
    if (std::getenv("APS5_PROFILE_GPU_DRAWS") != nullptr && state->properties.limits.timestampComputeAndGraphics) {
        state->drawProfiler = std::make_shared<Graphics::DrawProfiler>(state->device, state->deviceProc, state->properties.limits.timestampPeriod);
    }
    // Components copy the context when created: guest memory is imported first so every one of
    // them (render and texture caches included) sees it.
    state->guestGpuMemory = Graphics::GuestGpuMemory::Create(graphicsContext());
    state->bufferPool = std::make_shared<Graphics::BufferPool>(graphicsContext());
    state->descriptorCache = std::make_shared<Graphics::DescriptorCache>();
    state->samplerCache = std::make_shared<Graphics::SamplerCache>();
    state->pipelineCache = std::make_unique<Graphics::PipelineCache>(graphicsContext(), state->properties);
    state->detiler = std::make_unique<Graphics::TextureDetiler>(graphicsContext());
    state->drawQueue = std::make_unique<Graphics::DrawQueue>();
    state->renderCache = std::make_unique<Graphics::RenderCache>(graphicsContext());
    state->graphicsPipelines = std::make_unique<Graphics::GraphicsPipelineCache>(graphicsContext());
    state->textureCache = std::make_unique<Graphics::TextureCache>(graphicsContext());
    state->colorTransfer = std::make_unique<Graphics::GpuColorTransfer>(graphicsContext());
    if (window != nullptr) {
        require(window->getDrawableSize != nullptr, "missing window drawable size query");
        std::uint32_t drawableWidth = 0;
        std::uint32_t drawableHeight = 0;
        window->getDrawableSize(window->context, &drawableWidth, &drawableHeight);
        require(drawableWidth != 0 && drawableHeight != 0, "window has a zero drawable size at creation");
        VkSurfaceCapabilitiesKHR surface{};
        check(state->InstanceFunction<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(selected, state->surface, &surface), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        state->extent = {drawableWidth, drawableHeight};
        require(surface.currentExtent.width == std::numeric_limits<std::uint32_t>::max() || (surface.currentExtent.width == drawableWidth && surface.currentExtent.height == drawableHeight), "window extent differs from the real drawable size");
        require(drawableWidth >= surface.minImageExtent.width && drawableWidth <= surface.maxImageExtent.width && drawableHeight >= surface.minImageExtent.height && drawableHeight <= surface.maxImageExtent.height, "unsupported output extent");
        require((surface.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0, "surface does not support transfer destination images");
        require((surface.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0, "opaque composition is unavailable");
        require((surface.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0, "identity surface transform is unavailable");
        std::uint32_t formatCount = 0;
        auto getFormats = state->InstanceFunction<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR");
        check(getFormats(selected, state->surface, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR");
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        check(getFormats(selected, state->surface, &formatCount, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");
        require(std::any_of(formats.begin(), formats.end(), [](const auto& format) { return format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; }), "BGRA8 sRGB-nonlinear surface format is unavailable");
        VkSwapchainCreateInfoKHR swapchain{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        swapchain.surface = state->surface;
        swapchain.minImageCount = surface.minImageCount;
        swapchain.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        swapchain.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        swapchain.imageExtent = state->extent;
        swapchain.imageArrayLayers = 1;
        swapchain.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swapchain.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchain.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        swapchain.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swapchain.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapchain.clipped = VK_FALSE;
        check(state->DeviceFunction<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR")(state->device, &swapchain, nullptr, &state->swapchain), "vkCreateSwapchainKHR");
        std::uint32_t imageCount = 0;
        auto getImages = state->DeviceFunction<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
        check(getImages(state->device, state->swapchain, &imageCount, nullptr), "vkGetSwapchainImagesKHR");
        state->images.resize(imageCount);
        check(getImages(state->device, state->swapchain, &imageCount, state->images.data()), "vkGetSwapchainImagesKHR");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (auto* destination : {&state->acquireFence, &state->renderFence}) {
            check(state->DeviceFunction<PFN_vkCreateFence>("vkCreateFence")(state->device, &fence, nullptr, destination), "vkCreateFence");
        }
        state->images.resize(imageCount);
        state->rendered.resize(imageCount, VK_NULL_HANDLE);
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = state->pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        check(state->DeviceFunction<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(state->device, &allocation, &state->clearCommands), "vkAllocateCommandBuffers");
        state->scaler = std::make_unique<PresentationScaler>(graphicsContext(), VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM);
        state->rgbaScaler = std::make_unique<PresentationScaler>(graphicsContext(), VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM);
    }
}

VulkanDevice::~VulkanDevice() = default;

void VulkanDevice::WaitIdle() {
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    PerformanceTimer timing("Vulkan.WaitIdle");
    // Every GPU write noted so far is recorded (nothing records while the mutex is held): it has
    // landed once the device is idle.
    const auto gpuWrites = WriteTracker::GpuWriteSequence();
    state->drawQueue->Wait();
    timing.Mark("draw_wait");
    check(state->DeviceFunction<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(state->device), "vkDeviceWaitIdle");
    timing.Mark("device_wait");
    WriteTracker::NoteGpuIdle(gpuWrites);
    // With the GPU idle, imports of freed guest memory can go.
    if (state->guestGpuMemory) state->guestGpuMemory->Collect();
}

void VulkanDevice::WaitDraws() {
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    PerformanceTimer timing("Vulkan.WaitDraws");
    state->drawQueue->Wait();
}

std::uint64_t VulkanDevice::CountSamples() {
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    PerformanceTimer timing("Vulkan.CountSamples");
    state->drawQueue->EnableSampleCounting(graphicsContext(), state->occlusionQueryPrecise);
    state->drawQueue->Wait();
    return state->drawQueue->SamplesPassed();
}

void* VulkanDevice::Window() const {
    return state->window;
}

void VulkanDevice::Resize(std::uint32_t width, std::uint32_t height) {
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    require(state->swapchain != VK_NULL_HANDLE, "cannot resize an unavailable swapchain");
    if (width == 0 || height == 0) {
        state->extent = {0, 0};
        return;
    }
    VkSurfaceCapabilitiesKHR surface{};
    check(state->InstanceFunction<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(state->physical, state->surface, &surface), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR resize");
    if (surface.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        width = surface.currentExtent.width;
        height = surface.currentExtent.height;
    }
    if (width == 0 || height == 0) {
        state->extent = {0, 0};
        return;
    }
    if (!state->swapchainState.NeedsRecreation() && state->extent.width == width && state->extent.height == height) return;
    WaitIdle();
    require(width >= surface.minImageExtent.width && width <= surface.maxImageExtent.width && height >= surface.minImageExtent.height && height <= surface.maxImageExtent.height, "unsupported resized output extent");
    require((surface.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 && (surface.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0 && (surface.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0, "resized surface capabilities are unsupported");
    VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    create.surface = state->surface;
    create.minImageCount = surface.minImageCount;
    create.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    create.imageExtent = {width, height};
    create.imageArrayLayers = 1;
    create.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    create.oldSwapchain = state->swapchain;
    state->retiredSwapchains.reserve(state->retiredSwapchains.size() + 1);
    VkSwapchainKHR replacement = VK_NULL_HANDLE;
    check(state->DeviceFunction<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR")(state->device, &create, nullptr, &replacement), "vkCreateSwapchainKHR resize");
    state->retiredSwapchains.push_back({state->swapchain, std::move(state->rendered)});
    state->swapchain = replacement;
    state->extent = create.imageExtent;
    auto getImages = state->DeviceFunction<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
    std::uint32_t count = 0;
    check(getImages(state->device, replacement, &count, nullptr), "vkGetSwapchainImagesKHR resize");
    state->images.resize(count);
    check(getImages(state->device, replacement, &count, state->images.data()), "vkGetSwapchainImagesKHR resize");
    state->images.resize(count);
    state->rendered.assign(count, VK_NULL_HANDLE);
    state->swapchainState.Recreated();
}

bool VulkanDevice::Presentable() const {
    return state->extent.width != 0 && state->extent.height != 0;
}

bool VulkanDevice::SamplerFilterMinmax() const {
    return state->samplerFilterMinmax;
}

bool VulkanDevice::PrimitiveListRestart() const {
    return state->primitiveListRestart;
}

void VulkanDevice::PresentClear(std::uint32_t width, std::uint32_t height, bool opaque) {
    present(width, height, opaque, {});
}

void VulkanDevice::PresentPixels(std::uint32_t width, std::uint32_t height, std::span<const std::byte> pixels) {
    require(width != 0 && height != 0 && width <= 16384 && height <= 16384, "invalid display image extent");
    require(pixels.size() == static_cast<std::uint64_t>(width) * height * 4, "invalid display pixel buffer size");
    present(width, height, true, pixels);
}

namespace {

// ANYPS5_DUMP_FRAMES=N writes every Nth presented display buffer to anyps5-frame-<n>.ppm in the
// working directory, for checking what a title renders.
std::uint32_t frameDumpInterval() {
    static const std::uint32_t interval = [] {
        const char* value = std::getenv("ANYPS5_DUMP_FRAMES");
        return value ? static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
    }();
    return interval;
}

void writeFrame(const DisplayBuffer& buffer, std::span<const std::byte> bgra, std::uint64_t index) {
    char name[64];
    std::snprintf(name, sizeof(name), "anyps5-frame-%06llu.ppm", static_cast<unsigned long long>(index));
    std::FILE* file = std::fopen(name, "wb");
    if (file == nullptr) return;
    std::fprintf(file, "P6\n%u %u\n255\n", buffer.width, buffer.height);
    std::vector<unsigned char> row(static_cast<std::size_t>(buffer.width) * 3u);
    for (std::uint32_t y = 0; y < buffer.height; ++y) {
        for (std::uint32_t x = 0; x < buffer.width; ++x) {
            const auto* pixel = bgra.data() + (static_cast<std::size_t>(y) * buffer.width + x) * 4u;
            row[x * 3u] = static_cast<unsigned char>(pixel[2]);
            row[x * 3u + 1u] = static_cast<unsigned char>(pixel[1]);
            row[x * 3u + 2u] = static_cast<unsigned char>(pixel[0]);
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
}

}

bool VulkanDevice::AsyncFlip() {
    static const bool async = std::getenv("ANYPS5_SYNC_FLIP") == nullptr;
    return async;
}

void VulkanDevice::PresentDisplayBuffer(const DisplayBuffer& buffer) {
    // Linear (tiling mode 1) scanout surfaces are never render-target images: present their pixels.
    if (buffer.tilingMode == 1) {
        ResolveMemory(buffer.address, DisplayBufferSize(buffer), false);
        present(buffer.width, buffer.height, true, ReadDisplayBuffer(buffer));
        return;
    }
    present(buffer.width, buffer.height, true, {}, &buffer);
}

// ANYPS5_DUMP_FRAMES: every Nth flipped display buffer, whether or not the window can show it (a
// minimized window presents nothing).
void VulkanDevice::DumpFrame(const DisplayBuffer& buffer) {
    const auto interval = frameDumpInterval();
    if (interval == 0) return;
    static std::uint64_t flipped = 0;
    if (flipped++ % interval != 0) return;
    WaitDraws();
    ResolveMemory(buffer.address, DisplayBufferSize(buffer), false);
    writeFrame(buffer, ReadDisplayBuffer(buffer), flipped - 1);
}

void VulkanDevice::present(std::uint32_t width, std::uint32_t height, bool opaque, std::span<const std::byte> pixels, const DisplayBuffer* display) {
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    PerformanceTimer timing("Vulkan.Present");
    // The presentation runs after every earlier submission in queue order (its first barrier
    // covers them): queued work is submitted, not waited for. A display buffer read from guest
    // memory waits for its writers in ResolveMemory.
    if (AsyncFlip()) state->drawQueue->Flush();
    else state->drawQueue->Wait();
    timing.Mark("draw_wait");
    require(state->swapchain != VK_NULL_HANDLE, "device has no swapchain");
    require(state->extent.width != 0 && state->extent.height != 0, "output window is minimized");
    std::shared_ptr<Graphics::ResidentColor> resident;
    if (display != nullptr) {
        const auto bytes = DisplayBufferSize(*display);
        resident = state->renderCache->Find(display->address);
        if (resident) {
            const auto& color = resident->Description();
            if (color.extent.width != width || color.extent.height != height || color.bytes != bytes || color.tileMode != Graphics::ColorTileMode::RenderTarget) resident.reset();
        }
        if (!resident) {
            ResolveMemory(display->address, bytes, false);
            state->colorTransfer->Upload(display->address, width, height, Graphics::ColorTileMode::RenderTarget);
        }
    }
    const auto layout = display != nullptr ? DecodeDisplayPixelFormat(display->pixelFormat) : DisplayPixelLayout::Bgra8;
    auto* scaler = resident && layout == DisplayPixelLayout::Rgba8 ? state->rgbaScaler.get() : state->scaler.get();
    if (layout == DisplayPixelLayout::Rgb10A2 || layout == DisplayPixelLayout::Bgr10A2) {
        // The display engine reinterprets the surface bits, so the source image takes the guest
        // format and the blit converts to the swapchain.
        const auto format = layout == DisplayPixelLayout::Rgb10A2 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        auto& packed = state->packedScalers[format];
        if (!packed) packed = std::make_unique<PresentationScaler>(graphicsContext(), format, VK_FORMAT_B8G8R8A8_UNORM);
        scaler = packed.get();
    }
    if (!pixels.empty()) state->Upload(pixels);
    timing.Mark("pixel_upload");
    auto wait = state->DeviceFunction<PFN_vkWaitForFences>("vkWaitForFences");
    auto reset = state->DeviceFunction<PFN_vkResetFences>("vkResetFences");
    // The previous presentation's commands are reused: they must be done (normally long since).
    if (state->renderPending) {
        check(wait(state->device, 1, &state->renderFence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()), "vkWaitForFences previous presentation");
        state->renderPending = false;
    }
    const std::array<VkFence, 2> fences{state->acquireFence, state->renderFence};
    check(reset(state->device, static_cast<std::uint32_t>(fences.size()), fences.data()), "vkResetFences");
    std::uint32_t index = 0;
    timing.Mark("fence_reset");
    if (!state->swapchainState.ProcessResult(state->DeviceFunction<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR")(state->device, state->swapchain, 5'000'000'000ULL, VK_NULL_HANDLE, state->acquireFence, &index), "vkAcquireNextImageKHR")) return;
    timing.Mark("acquire_image");
    check(wait(state->device, 1, &state->acquireFence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()), "vkWaitForFences acquire");
    timing.Mark("acquire_fence_wait");
    require(index < state->images.size() && index < state->rendered.size(), "acquired image index is out of range");
    auto& rendered = state->rendered[index];
    if (rendered != VK_NULL_HANDLE) {
        state->DestroyRetiredSwapchains();
    } else {
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(state->DeviceFunction<PFN_vkCreateSemaphore>("vkCreateSemaphore")(state->device, &semaphore, nullptr, &rendered), "vkCreateSemaphore presentation");
    }
    auto commands = state->clearCommands;
    timing.Mark("retired_swapchains");
    check(state->DeviceFunction<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(commands, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(state->DeviceFunction<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin), "vkBeginCommandBuffer");
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = state->images[index];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    auto pipelineBarrier = state->DeviceFunction<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    pipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    if (pixels.empty() && display == nullptr) {
        VkClearColorValue clear{};
        clear.float32[3] = opaque ? 1.0f : 0.0f;
        state->DeviceFunction<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &barrier.subresourceRange);
    } else {
        require(scaler != nullptr, "presentation scaler is unavailable");
        scaler->EnsureSourceImage(width, height);
        if (resident) {
            resident->Transition(commands, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            scaler->RecordImage(commands, resident->Target().Image());
        } else {
            if (display != nullptr) state->colorTransfer->Detile(commands, layout == DisplayPixelLayout::Rgba8);
            scaler->RecordUpload(commands, display != nullptr ? state->colorTransfer->LinearBuffer() : state->uploadBuffer);
        }
        VkClearColorValue letterbox{};
        letterbox.float32[3] = 1.0f;
        state->DeviceFunction<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &letterbox, 1, &barrier.subresourceRange);
        VkImageMemoryBarrier letterboxBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        letterboxBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        letterboxBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        letterboxBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        letterboxBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        letterboxBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        letterboxBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        letterboxBarrier.image = barrier.image;
        letterboxBarrier.subresourceRange = barrier.subresourceRange;
        pipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &letterboxBarrier);
        scaler->RecordBlit(commands, barrier.image, state->extent.width, state->extent.height);
    }
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    check(state->DeviceFunction<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &rendered;
    timing.Mark("command_record_scale");
    check(state->DeviceFunction<PFN_vkQueueSubmit>("vkQueueSubmit")(state->queue, 1, &submit, state->renderFence), "vkQueueSubmit clear");
    timing.Mark("queue_submit");
    // The present waits for the semaphore on the GPU; the fence is waited for before the commands
    // are reused (next presentation).
    if (AsyncFlip()) {
        state->renderPending = true;
    } else {
        check(wait(state->device, 1, &state->renderFence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()), "vkWaitForFences clear");
    }
    timing.Mark("render_fence_wait");
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &state->swapchain;
    present.pImageIndices = &index;
    state->swapchainState.ProcessResult(state->DeviceFunction<PFN_vkQueuePresentKHR>("vkQueuePresentKHR")(state->queue, &present), "vkQueuePresentKHR");
    timing.Mark("queue_present");
}

ShaderRecompiler::SpirvTarget VulkanDevice::ComputeTarget(std::uint32_t) const {
    return Target();
}

std::string VulkanDevice::DeviceName() const {
    return state->properties.deviceName;
}

ShaderRecompiler::SpirvTarget VulkanDevice::Target() const {
    const auto& limits = state->properties.limits;
    ShaderRecompiler::SpirvTarget target{VK_API_VERSION_1_1, state->meshShader ? 0x00010400u : 0x00010300u, state->subgroup.subgroupSize, ShaderRecompiler::BdaAbi::Version, state->capabilities, state->spirvExtensions, state->fragmentShaderBarycentric, {limits.maxComputeWorkGroupSize[0], limits.maxComputeWorkGroupSize[1], limits.maxComputeWorkGroupSize[2]}, limits.maxComputeWorkGroupInvocations, limits.maxComputeSharedMemorySize, {}, {}};
    target.storageBufferOffsetAlignment = static_cast<std::uint32_t>(limits.minStorageBufferOffsetAlignment);
    target.nonConstantImageOffsets = state->maintenance8;
    target.srgbDecodeFormats = state->srgbDecodeFormats;
    if (state->meshShader) {
        const auto& mesh = state->meshLimits;
        target.mesh = ShaderRecompiler::MeshTargetLimits{{mesh.maxMeshWorkGroupSize[0], mesh.maxMeshWorkGroupSize[1], mesh.maxMeshWorkGroupSize[2]}, mesh.maxMeshWorkGroupInvocations, std::min(mesh.maxMeshSharedMemorySize, mesh.maxMeshPayloadAndSharedMemorySize), mesh.maxMeshOutputVertices, mesh.maxMeshOutputPrimitives, mesh.maxMeshOutputComponents, std::min(mesh.maxMeshOutputMemorySize, mesh.maxMeshPayloadAndOutputMemorySize), mesh.meshOutputPerVertexGranularity, mesh.meshOutputPerPrimitiveGranularity};
    }
    if (state->tessellationShader) target.tessellation = ShaderRecompiler::TessellationTargetLimits{limits.maxTessellationPatchSize, limits.maxTessellationControlPerVertexInputComponents, limits.maxTessellationControlPerVertexOutputComponents, limits.maxTessellationControlPerPatchOutputComponents, limits.maxTessellationControlTotalOutputComponents, limits.maxTessellationEvaluationInputComponents, limits.maxTessellationEvaluationOutputComponents};
    return target;
}

Graphics::Context VulkanDevice::graphicsContext() const {
    auto context = Graphics::Context{
        state->device,
        state->physical,
        state->queue,
        state->pool,
        state->deviceProc,
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties"),
        state->InstanceFunction<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties"),
        state->memoryProperties,
        state->properties.limits,
        state->tessellationShader,
        state->meshShader,
        state->meshLimits,
        state->depthClipControl,
        state->depthRangeUnrestricted,
        true,
        state->subgroup,
        state->fragmentShaderBarycentric,
        state->samplerAnisotropy,
        state->textureCompressionBC,
        state->detiler.get(),
        state->colorTransfer.get(),
        state->bufferPool,
        state->textureCache.get(),
        state->pipelineCache ? state->pipelineCache->Handle() : VK_NULL_HANDLE,
        state->renderCache.get(),
        state->drawQueue.get(),
        state->graphicsPipelines.get(),
        state->descriptorCache,
        state->samplerCache
    };
    context.depthBounds = state->depthBounds;
    context.depthBiasClamp = state->depthBiasClamp;
    context.independentBlend = state->independentBlend;
    context.geometryShader = state->geometryShader;
    context.imageGatherExtended = state->imageGatherExtended;
    context.primitiveListRestart = state->primitiveListRestart;
    context.shaderResourceMinLod = state->shaderResourceMinLod;
    context.imageViewMinLod = state->imageViewMinLod;
    context.imageInt64Atomics = state->imageInt64Atomics;
    context.srgbDecodeFormats = state->srgbDecodeFormats;
    context.samplerFilterMinmax = state->samplerFilterMinmax;
    context.storageImageReadWithoutFormat = state->storageImageReadWithoutFormat;
    context.storageImageWriteWithoutFormat = state->storageImageWriteWithoutFormat;
    context.clipDistance = state->clipDistance;
    context.depthClamp = state->depthClamp;
    context.cullDistance = state->cullDistance;
    context.viewportIndexLayer = state->viewportIndexLayer;
    context.externalMemoryHost = state->externalMemoryHost;
    context.guestGpuMemory = state->guestGpuMemory.get();
    context.imageMemory = state->imageMemory;
    context.gpuTimestamps = state->gpuTimestamps;
    context.drawProfiler = state->drawProfiler;
    return context;
}

void VulkanDevice::ResolveMemory(std::uint64_t address, std::size_t bytes, bool writable) {
    // Runs for every guest range check; the draw queue and render cache time their slow paths.
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    state->drawQueue->Resolve(address, bytes);
    state->renderCache->Resolve(address, bytes, writable);
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> VulkanDevice::DirtyRanges() {
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    if (state->renderCache) state->renderCache->DirtyRanges(ranges);
    return ranges;
}

std::uint64_t VulkanDevice::LastGpuWriter(std::uint64_t address, std::size_t bytes) {
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    return state->drawQueue->LastWriter(address, bytes);
}

bool VulkanDevice::RecordDmaData(const Pm4::DmaCopy& dma, std::uint64_t* serial) {
    static const bool cpuOnly = std::getenv("ANYPS5_CPU_DMA") != nullptr;
    if (cpuOnly || state->guestGpuMemory == nullptr || dma.bytes == 0) return false;
    // vkCmdFillBuffer writes whole dwords; vkCmdCopyBuffer's regions must not overlap.
    if ((dma.immediate || !dma.data.empty()) && ((dma.destination | dma.bytes) & 3u) != 0) return false;
    // vkCmdUpdateBuffer takes at most 64 KiB.
    if (!dma.data.empty() && dma.bytes > 65536) return false;
    if (!dma.immediate && dma.source < dma.destination + dma.bytes && dma.destination < dma.source + dma.bytes) return false;
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    PerformanceTimer timing("Vulkan.DmaData");
    const auto context = graphicsContext();
    {
        // A GPU access: resident surfaces over the ranges are written back (or, for the
        // destination, invalidated) ahead of it on the draw queue.
        const GuestMemory::MemoryAccessScope memoryScope(this, [](void* owner, std::uint64_t address, std::size_t bytes, bool writable) {
            static_cast<VulkanDevice*>(owner)->ResolveMemory(address, bytes, writable);
        });
        const GuestMemory::GpuAccessScope gpuAccess;
        const GuestMemory::AccessSite accessSite("dma_data");
        if (!dma.immediate && dma.data.empty()) GuestMemory::CheckRange(reinterpret_cast<const void*>(dma.source), static_cast<std::size_t>(dma.bytes), 1);
        GuestMemory::CheckRange(reinterpret_cast<void*>(dma.destination), static_cast<std::size_t>(dma.bytes), 1, true);
    }
    const auto destination = state->guestGpuMemory->Resolve(dma.destination, dma.bytes);
    if (!destination || destination->bytes < dma.bytes) return false;
    std::optional<Graphics::GuestGpuMemory::View> source;
    if (!dma.immediate && dma.data.empty()) {
        source = state->guestGpuMemory->Resolve(dma.source, dma.bytes);
        if (!source || source->bytes < dma.bytes) return false;
    }
    const auto commands = state->drawQueue->Begin(context);
    const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier(commands, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    if (!dma.data.empty()) {
        context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(commands, destination->buffer, destination->offset, dma.bytes, dma.data.data());
    } else if (dma.immediate) {
        context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, destination->buffer, destination->offset, dma.bytes, dma.value);
    } else {
        const VkBufferCopy region{source->offset, destination->offset, dma.bytes};
        context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, source->buffer, destination->buffer, 1, &region);
    }
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
    // CPU accesses of the destination wait for the batch; the texture cache sees it as written.
    static const auto keepAlive = std::make_shared<int>(0);
    const auto written = state->drawQueue->NoteGuestWrite(dma.destination, dma.destination + dma.bytes, keepAlive);
    if (serial != nullptr) *serial = written;
    WriteTracker::NoteGpuWrite(dma.destination, dma.bytes);
    timing.Mark("recorded", dma.bytes);
    // Debug aid: APS5_TRACE_DMA=1 logs every DMA_DATA recorded on the GPU.
    static const bool trace = std::getenv("APS5_TRACE_DMA") != nullptr;
    if (trace) std::fprintf(stderr, "[dma] %s 0x%llx -> 0x%llx (%llu bytes)\n", dma.immediate ? "fill" : "copy", static_cast<unsigned long long>(dma.immediate ? dma.value : dma.source), static_cast<unsigned long long>(dma.destination), static_cast<unsigned long long>(dma.bytes));
    return true;
}

void VulkanDevice::ResolveFastClears(const Graphics::State& graphics) {
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    const GuestMemory::MemoryAccessScope memoryScope(this, [](void* context, std::uint64_t address, std::size_t bytes, bool writable) {
        static_cast<VulkanDevice*>(context)->ResolveMemory(address, bytes, writable);
    });
    const GuestMemory::AccessSite accessSite("fast_clear");
    Graphics::ApplyFastClears(graphics);
}

void VulkanDevice::Draw(const Graphics::State& graphics, const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> shaders, std::span<const Graphics::GuestMemorySnapshot> snapshots) {
    EnqueueDraw(graphics, draw, shaders, snapshots);
    WaitIdle();
}

void VulkanDevice::EnqueueDraw(const Graphics::State& graphics, const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> shaders, std::span<const Graphics::GuestMemorySnapshot> snapshots) {
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    const GuestMemory::MemoryAccessScope memoryScope(this, [](void* context, std::uint64_t address, std::size_t bytes, bool writable) {
        static_cast<VulkanDevice*>(context)->ResolveMemory(address, bytes, writable);
    });
    const GuestMemory::AccessSite accessSite("draw");
    const auto context = graphicsContext();
    Graphics::Draw(context, graphics, draw, shaders, snapshots);
}

void VulkanDevice::Dispatch(const ShaderRecompiler::RecompileResult& shader, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::span<const Graphics::GuestMemorySnapshot> snapshots, std::uint64_t) {
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    PerformanceTimer timing("Vulkan.Dispatch");
    const GuestMemory::MemoryAccessScope memoryScope(this, [](void* context, std::uint64_t address, std::size_t bytes, bool writable) {
        static_cast<VulkanDevice*>(context)->ResolveMemory(address, bytes, writable);
    });
    const GuestMemory::AccessSite accessSite("dispatch");
    if (shader.spirv.size() < 5 || shader.spirv[0] != 0x07230203u) {
        throw std::runtime_error("Vulkan dispatch: invalid SPIR-V");
    }
    const std::array<Graphics::CompiledShader, 1> shaders{{{ShaderRecompiler::ShaderStage::Compute, &shader, 0}}};
    const auto pushStages = Graphics::PushConstantStages(shaders);
    if (pushStages != 0 && state->properties.limits.maxPushConstantsSize < Graphics::PipelinePushConstantBytes) {
        throw std::runtime_error("Vulkan dispatch: compute push constant range exceeds device limit");
    }
    const auto pushBytes = Graphics::AssemblePushConstants(shaders);
    const auto context = graphicsContext();
    const auto* limit = state->properties.limits.maxComputeWorkGroupCount;
    if (x > limit[0] || y > limit[1] || z > limit[2]) {
        throw std::runtime_error("Vulkan dispatch: workgroup count exceeds device limits");
    }
    auto resources = std::make_shared<Graphics::ShaderResources>(context, shaders[0], snapshots);
    timing.Mark("shader_resources");
    // One pipeline per compiled variant and descriptor layout (creating and destroying the module,
    // layout and pipeline for every dispatch cost over a millisecond).
    std::string key;
    const auto append = [&](const void* data, std::size_t bytes) { key.append(static_cast<const char*>(data), bytes); };
    append(&shader.variantId, sizeof(shader.variantId));
    if (shader.variantId == 0) append(shader.spirv.data(), shader.spirv.size() * sizeof(std::uint32_t));
    append(&pushStages, sizeof(pushStages));
    const auto& layoutKey = resources->LayoutKey();
    append(layoutKey.data(), layoutKey.size() * sizeof(std::uint32_t));
    auto& cached = state->computePipelines[key];
    if (!cached) {
        auto created = std::make_shared<ComputePipeline>();
        created->device = state->device;
        created->deviceProc = state->deviceProc;
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = shader.spirv.size() * sizeof(std::uint32_t);
        moduleInfo.pCode = shader.spirv.data();
        check(state->DeviceFunction<PFN_vkCreateShaderModule>("vkCreateShaderModule")(state->device, &moduleInfo, nullptr, &created->module), "vkCreateShaderModule");
        const auto setLayout = resources->Layout();
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, Graphics::PipelinePushConstantBytes};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;
        layoutInfo.pushConstantRangeCount = pushStages != 0 ? 1 : 0;
        layoutInfo.pPushConstantRanges = pushStages != 0 ? &push : nullptr;
        check(state->DeviceFunction<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(state->device, &layoutInfo, nullptr, &created->layout), "vkCreatePipelineLayout");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = created->module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = created->layout;
        check(state->DeviceFunction<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(state->device, context.pipelineCache, 1, &pipelineInfo, nullptr, &created->pipeline), "vkCreateComputePipelines");
        cached = std::move(created);
        timing.Mark("pipeline_create");
    }
    auto pipeline = cached;
    timing.Mark("pipeline_cache");
    // Recorded into the draw queue and run asynchronously, in submission order with draws. Full
    // barriers on both sides give it the ordering the synchronous dispatch had (earlier work done
    // before it reads or writes; later work sees its writes and never overtakes its reads); CPU
    // readers of its writes wait through the queue's pending-write index.
    const auto commands = state->drawQueue->Begin(context);
    const auto barrier = state->DeviceFunction<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    std::uint32_t profile = UINT32_MAX;
    if (state->drawProfiler) {
        char key[96];
        std::snprintf(key, sizeof(key), "dispatch cs=%016llx", static_cast<unsigned long long>(shader.variantId));
        profile = state->drawProfiler->Begin(context, commands, key);
    }
    barrier(commands, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    resources->RecordUploads(commands);
    if (state->drawProfiler) state->drawProfiler->Mark(context, commands, profile, 1);
    state->DeviceFunction<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);
    resources->Bind(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout);
    if (pushStages != 0) {
        state->DeviceFunction<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, pipeline->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, Graphics::PipelinePushConstantBytes, pushBytes.data());
    }
    state->DeviceFunction<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, x, y, z);
    if (state->drawProfiler) state->drawProfiler->Mark(context, commands, profile, 2);
    resources->RecordDownloads(commands);
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
    if (state->drawProfiler) state->drawProfiler->Mark(context, commands, profile, 3);
    timing.Mark("command_record");
    state->drawQueue->Enqueue(std::move(resources), std::move(pipeline));
    timing.Mark("enqueue");
}

void VulkanDevice::ReportGpuTime(FrameTiming& frame) {
    if (state->drawProfiler) state->drawProfiler->EndFrame();
    if (!state->gpuTimestamps) return;
    std::uint64_t busy = 0;
    for (const auto& [label, nanoseconds, batches] : state->gpuTimestamps->Drain(busy)) frame.Add("GPU", label, std::chrono::nanoseconds(nanoseconds), batches);
    frame.Add("GPU", "busy", std::chrono::nanoseconds(busy));
}

}
