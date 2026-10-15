// The on-disk pipeline cache (Graphics::PipelineCache): two caches of one process saving the same
// file keep each other's pipelines, a cache that adds nothing leaves the file alone, and an invalid
// file is replaced (upstream 8c8cd86c). Skips (77) without Vulkan or when the driver stores no
// pipelines in a pipeline cache.
#include "prx/libSceAgcDriver/Graphics/include/PipelineCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <SDL_loadso.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

void Expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class Device {
public:
    Device() {
#ifdef _WIN32
        library = SDL_LoadObject("vulkan-1.dll");
#else
        library = SDL_LoadObject("libvulkan.so.1");
#endif
        if (library == nullptr) throw Unavailable("cannot load Vulkan");
        try {
            instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(library, "vkGetInstanceProcAddr"));
            if (instanceProc == nullptr) throw Unavailable("missing Vulkan instance resolver");
            VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            application.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pApplicationInfo = &application;
            if (function<PFN_vkCreateInstance>("vkCreateInstance")(&info, nullptr, &instance) != VK_SUCCESS) throw Unavailable("vkCreateInstance failed");
            std::uint32_t count = 0;
            const auto enumerate = function<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
            Check(enumerate(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
            if (count == 0) throw Unavailable("no Vulkan device");
            std::vector<VkPhysicalDevice> devices(count);
            Check(enumerate(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
            context.physical = devices.front();
            const auto queues = function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
            queues(context.physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            queues(context.physical, &count, families.data());
            std::uint32_t family = 0;
            while (family < count && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) ++family;
            if (family >= count) throw Unavailable("no Vulkan compute queue");
            const float priority = 1;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            device.queueCreateInfoCount = 1;
            device.pQueueCreateInfos = &queue;
            Check(function<PFN_vkCreateDevice>("vkCreateDevice")(context.physical, &device, nullptr, &context.device), "vkCreateDevice");
            context.deviceProc = function<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
            function<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(context.physical, &context.memory);
            VkPhysicalDeviceProperties properties{};
            function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(context.physical, &properties);
            context.limits = properties.limits;
            context.Function<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(context.device, family, 0, &context.queue);
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = family;
            Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &pool, nullptr, &context.pool), "vkCreateCommandPool");
        } catch (...) {
            release();
            throw;
        }
    }

    ~Device() { release(); }
    const Context& GetContext() const { return context; }
    VkPhysicalDeviceProperties Properties() const {
        VkPhysicalDeviceProperties properties{};
        function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(context.physical, &properties);
        return properties;
    }

    struct Unavailable : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

private:
    template<typename TFunction>
    TFunction function(const char* name) const {
        const auto result = reinterpret_cast<TFunction>(instanceProc(instance, name));
        if (result == nullptr) throw Unavailable(name);
        return result;
    }

    void release() noexcept {
        if (context.pool != VK_NULL_HANDLE) context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, context.pool, nullptr);
        context.bufferPool.reset();
        if (context.device != VK_NULL_HANDLE) function<PFN_vkDestroyDevice>("vkDestroyDevice")(context.device, nullptr);
        if (instance != VK_NULL_HANDLE) function<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        if (library != nullptr) SDL_UnloadObject(library);
    }

    void* library = nullptr;
    PFN_vkGetInstanceProcAddr instanceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    Context context{};
};

// An empty compute shader of the given workgroup width: distinct widths are distinct pipelines.
std::vector<std::uint32_t> EmptyCompute(std::uint32_t width) {
    return {
        0x07230203u, 0x00010000u, 0u, 5u, 0u,
        0x00020011u, 1u,                                   // OpCapability Shader
        0x0003000eu, 0u, 1u,                               // OpMemoryModel Logical GLSL450
        0x0005000fu, 5u, 3u, 0x6e69616du, 0u,              // OpEntryPoint GLCompute %3 "main"
        0x00060010u, 3u, 17u, width, 1u, 1u,               // OpExecutionMode %3 LocalSize width 1 1
        0x00020013u, 1u,                                   // %1 = OpTypeVoid
        0x00030021u, 2u, 1u,                               // %2 = OpTypeFunction %1
        0x00050036u, 1u, 3u, 0u, 2u,                       // %3 = OpFunction %1 None %2
        0x000200f8u, 4u,                                   // OpLabel
        0x000100fdu,                                       // OpReturn
        0x00010038u,                                       // OpFunctionEnd
    };
}

int run(const Context& context, const VkPhysicalDeviceProperties& properties) {
    const auto directory = std::filesystem::temp_directory_path() / ("aps5-pipeline-cache-file-test-" + std::to_string(std::random_device{}()));
    struct RemoveDirectory {
        std::filesystem::path directory;
        ~RemoveDirectory() {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }
    } removeDirectory{directory};
    setenv("ANYPS5_NO_SHADER_CACHE", "0", 1);
    setenv("ANYPS5_SHADER_CACHE_DIR", directory.string().c_str(), 1);
    struct Objects {
        const Context& context;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        std::array<VkShaderModule, 2> modules{};
        ~Objects() {
            for (const auto module : modules) {
                if (module != VK_NULL_HANDLE) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
            }
            if (layout != VK_NULL_HANDLE) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, layout, nullptr);
        }
    } objects{context};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &objects.layout), "vkCreatePipelineLayout");
    for (std::size_t index = 0; index < objects.modules.size(); ++index) {
        const auto code = EmptyCompute(static_cast<std::uint32_t>(index + 1) * 32u);
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = code.size() * sizeof(std::uint32_t);
        moduleInfo.pCode = code.data();
        Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &objects.modules[index]), "vkCreateShaderModule");
    }
    const auto build = [&](VkPipelineCache cache, std::size_t index) {
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, objects.modules[index], "main", nullptr};
        info.layout = objects.layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, cache, 1, &info, nullptr, &pipeline), "vkCreateComputePipelines");
        context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
    };
    const auto bytes = [&](VkPipelineCache cache) {
        std::size_t size = 0;
        Check(context.Function<PFN_vkGetPipelineCacheData>("vkGetPipelineCacheData")(context.device, cache, &size, nullptr), "vkGetPipelineCacheData");
        return size;
    };
    // A cache holds a pipeline when building it again adds nothing.
    const auto holds = [&](VkPipelineCache cache, std::size_t index) {
        const auto before = bytes(cache);
        build(cache, index);
        return bytes(cache) == before;
    };
    const auto cacheFile = [&] {
        std::vector<std::filesystem::path> found;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) found.push_back(entry.path());
        Expect(found.size() == 1 && found.front().filename().string().starts_with("vk-pipelines-"), "the pipeline cache directory does not hold exactly one cache file");
        return found.front();
    };
    const auto contents = [](const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };
    {
        VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        VkPipelineCache probe = VK_NULL_HANDLE;
        Check(context.Function<PFN_vkCreatePipelineCache>("vkCreatePipelineCache")(context.device, &info, nullptr, &probe), "vkCreatePipelineCache");
        const bool stored = !holds(probe, 0);
        context.Function<PFN_vkDestroyPipelineCache>("vkDestroyPipelineCache")(context.device, probe, nullptr);
        if (!stored) {
            std::puts("skipped, the driver keeps no pipelines in a pipeline cache");
            return 77;
        }
    }
    for (const bool firstSavesFirst : {true, false}) {
        std::filesystem::remove_all(directory);
        auto first = std::make_unique<PipelineCache>(context, properties);
        auto second = std::make_unique<PipelineCache>(context, properties);
        build(first->Handle(), 0);
        build(second->Handle(), 1);
        if (firstSavesFirst) {
            first.reset();
            second.reset();
        } else {
            second.reset();
            first.reset();
        }
        PipelineCache third(context, properties);
        Expect(holds(third.Handle(), 0), firstSavesFirst ? "the cache file lost the pipeline of the object that saved first" : "the cache file lost the pipeline of the object that saved last");
        Expect(holds(third.Handle(), 1), firstSavesFirst ? "the cache file lost the pipeline of the object that saved last" : "the cache file lost the pipeline of the object that saved first");
    }
    const auto path = cacheFile();
    const auto stored = contents(path);
    {
        PipelineCache idle(context, properties);
    }
    Expect(contents(path) == stored, "an object that added no pipelines rewrote the cache file");
    {
        std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
        corrupt << "not a pipeline cache";
    }
    {
        PipelineCache replacing(context, properties);
        build(replacing.Handle(), 1);
    }
    {
        PipelineCache reloaded(context, properties);
        Expect(holds(reloaded.Handle(), 1), "an invalid cache file was not replaced by the saving object's pipelines");
    }
    std::puts("AGC driver pipeline cache file tests passed");
    return 0;
}

}

int main() {
    try {
        std::unique_ptr<Device> device;
        try {
            device = std::make_unique<Device>();
        } catch (const Device::Unavailable& error) {
            if (std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr) throw;
            std::printf("skipped, no usable Vulkan device: %s\n", error.what());
            return 77;
        }
        return run(device->GetContext(), device->Properties());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
