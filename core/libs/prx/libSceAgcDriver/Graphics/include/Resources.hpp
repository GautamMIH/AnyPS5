#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESOURCES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESOURCES_HPP

#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstdint>
#include <tuple>
#include <array>
#include <deque>
#include <string>
#include <atomic>
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
    // Whether shaders can sample the image (depth targets whose format supports it).
    bool Sampled() const { return sampled; }

private:
    void create(VkFormat format, VkExtent2D extent, std::size_t bytes, VkImageUsageFlags attachment, VkImageAspectFlags aspect, std::uint32_t layers = 1);
    void release() noexcept;
    Context context;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool sampled = false;
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

// GPU time of command batches: each batch writes timestamps at its start and end; completed
// batches add their duration to a total per label, drained into each frame's timing.
class GpuTimestamps {
public:
    GpuTimestamps(VkDevice device, PFN_vkGetDeviceProcAddr deviceProc, float period);
    ~GpuTimestamps();
    GpuTimestamps(const GpuTimestamps&) = delete;
    GpuTimestamps& operator=(const GpuTimestamps&) = delete;
    VkQueryPool Pool() const { return pool; }
    // The first of two consecutive queries (start, end).
    std::uint32_t Acquire();
    void Report(std::uint32_t first, const char* label);
    // Totals since the last drain, as (label, nanoseconds, batches); busy is the union of the
    // batches' GPU intervals (batches overlap, so the labels' sum can exceed it).
    std::vector<std::tuple<const char*, std::uint64_t, std::uint64_t>> Drain(std::uint64_t& busy);

private:
    static constexpr std::uint32_t Pairs = 4096;
    VkDevice device;
    PFN_vkGetDeviceProcAddr deviceProc;
    double period;
    VkQueryPool pool = VK_NULL_HANDLE;
    std::atomic<std::uint32_t> next{0};
    std::mutex mutex;
    std::map<const char*, std::pair<std::uint64_t, std::uint64_t>> totals;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> intervals;
};

// Debug aid: APS5_PROFILE_GPU_DRAWS=1 times every draw and dispatch on the GPU in three phases
// (setup: barriers, uploads and target transitions; work: the render pass or dispatch; teardown:
// downloads and barriers), totalled per key (the shaders and targets) and printed every
// APS5_PROFILE_GPU_FRAMES (60) frames, heaviest first.
class DrawProfiler {
public:
    static constexpr std::uint32_t Marks = 4;
    DrawProfiler(VkDevice device, PFN_vkGetDeviceProcAddr deviceProc, float period);
    ~DrawProfiler();
    DrawProfiler(const DrawProfiler&) = delete;
    DrawProfiler& operator=(const DrawProfiler&) = delete;
    // Starts a record (outside a render pass): resets its queries and writes mark 0. Returns
    // UINT32_MAX when every slot is waiting to be read.
    std::uint32_t Begin(const Context& context, VkCommandBuffer commands, std::string key);
    // Writes mark 1-3 of a record (any mark may be inside a render pass).
    void Mark(const Context& context, VkCommandBuffer commands, std::uint32_t record, std::uint32_t mark) const;
    // Reads finished records; prints the totals at the end of each reporting interval.
    void EndFrame();

private:
    static constexpr std::uint32_t Records = 16384;
    struct Pending {
        std::uint32_t record;
        std::string key;
    };
    struct Total {
        std::array<std::uint64_t, Marks - 1> phases{};
        std::uint64_t count = 0;
    };
    VkDevice device;
    PFN_vkGetDeviceProcAddr deviceProc;
    double period;
    VkQueryPool pool = VK_NULL_HANDLE;
    std::mutex mutex;
    std::uint32_t next = 0;
    std::deque<Pending> pending;
    std::map<std::string, Total> totals;
    std::uint32_t frames = 0;
    std::uint32_t interval = 60;
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
    // What the batch does, for GPU timing (a string literal).
    void Label(const char* name) { label = name; }

private:
    void release() noexcept;
    Context context;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    bool submitted = false;
    const char* label = "other";
    std::uint32_t timestamp = UINT32_MAX;
    void beginTiming();
    void reportTiming() noexcept;
};

}

#endif
