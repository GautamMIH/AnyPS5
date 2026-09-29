#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESOURCES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESOURCES_HPP

#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

class Buffer {
public:
    Buffer(const Context& context, std::size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    VkBuffer Handle() const;
    VkDeviceAddress DeviceAddress() const;
    std::span<std::byte> Bytes();
    void Invalidate();

private:
    void initializeAddress(VkBufferUsageFlags usage);
    void release() noexcept;
    Context context;
    VkDeviceAddress deviceAddress = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapping = nullptr;
    std::size_t size;
    VkDeviceSize allocationBytes = 0;
    VkBufferUsageFlags usage;
    VkMemoryPropertyFlags properties;
    bool reusable = false;
    std::shared_ptr<BufferPool> cache;
};

class RenderTarget {
public:
    RenderTarget(const Context& context, const ColorTarget& target, bool blending);
    RenderTarget(const Context& context, const DepthTarget& target);
    ~RenderTarget();
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;
    VkImage Image() const;
    VkImageView View() const;

private:
    void create(VkFormat format, VkExtent2D extent, std::size_t bytes, VkImageUsageFlags attachment, VkImageAspectFlags aspect, std::uint32_t layers = 1);
    void release() noexcept;
    Context context;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

// Device-local memory for images, sub-allocated from large blocks: one allocation per image cost
// ~1 ms on some drivers, and entering a level creates thousands of textures. Images larger than a
// quarter block get their own allocation. Blocks hold optimal-tiling images only (no linear
// resources share them, so bufferImageGranularity does not apply).
class ImageMemory {
public:
    struct Allocation {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
        std::size_t block = SIZE_MAX;
    };
    ImageMemory(VkDevice device, PFN_vkGetDeviceProcAddr deviceProc, const VkPhysicalDeviceMemoryProperties& properties);
    ~ImageMemory();
    ImageMemory(const ImageMemory&) = delete;
    ImageMemory& operator=(const ImageMemory&) = delete;
    Allocation Allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags flags);
    void Free(const Allocation& allocation) noexcept;

private:
    struct Block {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::uint32_t type = 0;
        VkDeviceSize used = 0;
        std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size
    };
    static constexpr VkDeviceSize BlockBytes = 64ull * 1024 * 1024;
    VkDeviceMemory allocate(std::uint32_t type, VkDeviceSize size);
    VkDevice device;
    PFN_vkGetDeviceProcAddr deviceProc;
    VkPhysicalDeviceMemoryProperties properties;
    std::mutex mutex;
    std::vector<Block> blocks;
};

// Frees the command buffers and fences recycled from destroyed command batches of the pool (before
// the pool or its device is destroyed).
void DropRecycledCommandBatches(VkDevice device, VkCommandPool pool, PFN_vkGetDeviceProcAddr deviceProc);

class CommandBatch {
public:
    explicit CommandBatch(const Context& context);
    ~CommandBatch();
    CommandBatch(const CommandBatch&) = delete;
    CommandBatch& operator=(const CommandBatch&) = delete;
    VkCommandBuffer Handle() const;
    void SubmitAndWait();
    void Submit();
    void Wait();
    bool IsComplete();
    void Reset();

private:
    void release() noexcept;
    Context context;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    bool submitted = false;
};

}

#endif
