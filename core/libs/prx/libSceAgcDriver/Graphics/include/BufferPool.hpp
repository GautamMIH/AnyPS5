#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BUFFERPOOL_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BUFFERPOOL_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Graphics {

struct BufferAllocation {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapping;
    VkDeviceAddress address;
    VkDeviceSize allocationBytes;
    std::size_t bytes;
    VkBufferUsageFlags usage;
    VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    // Bound at `offset` into a slot of a shared slab block (TakeSlot) instead of owning `memory`.
    VkDeviceSize offset = 0;
    bool slab = false;
};

struct SlabSlot {
    VkDeviceMemory memory;
    VkDeviceSize offset;
    void* mapping;
};

class BufferPool {
public:
    explicit BufferPool(const Context& context);
    ~BufferPool();
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    std::optional<BufferAllocation> Take(std::size_t bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void Put(const BufferAllocation& allocation) noexcept;
    // Small host-visible buffers are bound into slots of shared memory blocks (upstream 765619a0):
    // one vkAllocateMemory and vkMapMemory per 2 MiB block (at least 8 slots) instead of per buffer.
    // A slot is `capacity` bytes (a power of two at least the buffer's alignment and the
    // non-coherent atom), per memory type, slot size and device-address use; a block is freed when
    // it empties and its slab already keeps an empty one. ANYPS5_NO_BUFFER_SLABS=1 turns it off.
    static std::size_t SlabCapacity(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize atom);
    SlabSlot TakeSlot(const Context& context, std::uint32_t memoryType, std::size_t capacity, bool addressable);
    void PutSlot(VkDeviceMemory memory, VkDeviceSize offset) noexcept;

private:
    struct SlabBlock {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::byte* mapping = nullptr;
        VkDeviceSize slotBytes = 0;
        std::vector<std::uint32_t> free;
        std::uint32_t used = 0;
        std::uint64_t slab = 0;
    };
    struct Slab {
        std::vector<SlabBlock*> available;
        std::size_t emptyBlocks = 0;
    };
    static std::uint64_t slabKey(std::uint32_t memoryType, std::size_t capacity, bool addressable);
    void freeBlock(SlabBlock& block) noexcept;
    PFN_vkAllocateMemory allocateMemory;
    PFN_vkMapMemory mapMemory;
    std::mutex slabMutex;
    std::unordered_map<std::uint64_t, Slab> slabs;
    std::unordered_map<VkDeviceMemory, std::unique_ptr<SlabBlock>> slabBlocks;
    static constexpr std::size_t capacity = 1024;
    static constexpr std::size_t bucketCount = 256;
    static constexpr std::size_t none = capacity;
    struct Slot {
        std::optional<BufferAllocation> allocation;
        std::size_t next = none;
        std::size_t older = none;
        std::size_t newer = none;
    };
    static std::size_t bucket(std::size_t bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) noexcept;
    BufferAllocation remove(std::size_t index) noexcept;
    void destroy(const BufferAllocation& allocation) noexcept;
    VkDevice device;
    PFN_vkUnmapMemory unmap;
    PFN_vkDestroyBuffer destroyBuffer;
    PFN_vkFreeMemory freeMemory;
    std::mutex mutex;
    std::array<Slot, capacity> slots;
    std::array<std::size_t, bucketCount> buckets;
    std::array<std::size_t, capacity> freeSlots;
    std::size_t freeCount = capacity;
    std::size_t oldest = none;
    std::size_t newest = none;
    VkDeviceSize retainedBytes = 0;
    static constexpr VkDeviceSize budget = 512ull * 1024 * 1024;
};

std::shared_ptr<BufferPool> GetBufferPool(const Context& context);

}

#endif
