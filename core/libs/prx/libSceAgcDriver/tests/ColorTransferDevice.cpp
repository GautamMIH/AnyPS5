// GPU colour target transfers (ColorTransfer.comp) against the CPU layout (ColorTargetLayout):
// linear, SW_64KB_R_X and SW_4KB_S surfaces and mip-tail levels. Skips (77) without Vulkan.
#include "prx/libSceAgcDriver/Graphics/include/GpuColorTransfer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <SDL_loadso.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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

// Detiles a patterned surface on the GPU and tiles other texels back over it: the linear texels and
// every tiled byte (texels where the CPU layout puts them, padding unchanged) must match the CPU.
void checkTransfer(const Context& context, std::uint32_t width, std::uint32_t height, ColorTileMode mode, std::uint32_t elementBytes, ColorTail tail = {}) {
    const auto what = "mode " + std::to_string(static_cast<std::uint32_t>(mode)) + " " + std::to_string(width) + "x" + std::to_string(height) + " " + std::to_string(elementBytes) + " B" + (tail.present ? " tail (" + std::to_string(tail.x) + ", " + std::to_string(tail.y) + ")" : "");
    const ColorTargetLayout layout(width, height, mode, elementBytes, tail);
    std::vector<std::byte> storage(layout.Bytes() + layout.Alignment());
    void* aligned = storage.data();
    auto available = storage.size();
    Expect(std::align(layout.Alignment(), layout.Bytes(), aligned, available) != nullptr, "test surface alignment failed");
    const auto address = reinterpret_cast<std::uintptr_t>(aligned);
    std::span<std::byte> guest(static_cast<std::byte*>(aligned), layout.Bytes());
    for (std::size_t i = 0; i < guest.size(); ++i) guest[i] = static_cast<std::byte>((i * 151u + 7u) & 255u);
    std::vector<std::byte> expectedLinear(layout.LinearBytes());
    layout.Detile(std::vector<std::byte>(guest.begin(), guest.end()), expectedLinear);
    std::vector<std::byte> written(layout.LinearBytes());
    for (std::size_t i = 0; i < written.size(); ++i) written[i] = static_cast<std::byte>((i * 73u + i / 257u + 1u) & 255u);
    std::vector<std::byte> expectedTiled(guest.begin(), guest.end());
    layout.Tile(written, expectedTiled);

    GpuColorTransfer transfer(context);
    transfer.Upload(address, width, height, mode, elementBytes, tail);
    Buffer readback(context, layout.LinearBytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buffer source(context, layout.LinearBytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(source.Bytes().data(), written.data(), written.size());
    {
        CommandBatch batch(context);
        const auto commands = batch.Handle();
        transfer.Detile(commands);
        const VkBufferCopy copy{0, 0, layout.LinearBytes()};
        context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, transfer.LinearBuffer(), readback.Handle(), 1, &copy);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, source.Handle(), transfer.LinearBuffer(), 1, &copy);
        transfer.Tile(commands);
        batch.SubmitAndWait();
    }
    readback.Invalidate();
    Expect(std::equal(expectedLinear.begin(), expectedLinear.end(), readback.Bytes().begin()), what + ": GPU detile differs from the CPU layout");
    transfer.WriteBack(address);
    Expect(std::equal(expectedTiled.begin(), expectedTiled.end(), guest.begin()), what + ": GPU tile differs from the CPU layout or changed padding");
}

void run(const Context& context) {
    for (const std::uint32_t elementBytes : {1u, 2u, 4u, 8u, 16u}) {
        checkTransfer(context, 100, 70, ColorTileMode::Standard4KB, elementBytes);
        checkTransfer(context, 100, 70, ColorTileMode::RenderTarget, elementBytes);
        checkTransfer(context, 100, 70, ColorTileMode::Linear, elementBytes);
    }
    // Mip-tail levels of a 4 KiB standard chain (slot origins from addrlib's tail table for 32 bpp).
    checkTransfer(context, 8, 8, ColorTileMode::Standard4KB, 4, {true, 16, 0});
    checkTransfer(context, 4, 4, ColorTileMode::Standard4KB, 4, {true, 0, 24});
    checkTransfer(context, 16, 16, ColorTileMode::RenderTarget, 4, {true, 64, 0});
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
        run(device->GetContext());
        std::puts("AGC driver colour transfer device tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
