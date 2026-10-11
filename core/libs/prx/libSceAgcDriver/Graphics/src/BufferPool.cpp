#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>

namespace AgcDriver::Graphics {

namespace {

// Buffers below this share slab blocks.
constexpr std::size_t SlabLimit = 512u * 1024u;

VkDeviceSize slabBlockBytes(std::size_t capacity) {
    return std::max<VkDeviceSize>(VkDeviceSize{2} << 20u, static_cast<VkDeviceSize>(capacity) * 8u);
}

}

BufferPool::BufferPool(const Context& context) : allocateMemory(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")), mapMemory(context.Function<PFN_vkMapMemory>("vkMapMemory")), device(context.device), unmap(context.Function<PFN_vkUnmapMemory>("vkUnmapMemory")), destroyBuffer(context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")), freeMemory(context.Function<PFN_vkFreeMemory>("vkFreeMemory")) {
    buckets.fill(none);
    for (std::size_t index = 0; index < capacity; ++index) freeSlots[index] = index;
}

BufferPool::~BufferPool() {
    for (const auto& slot : slots) {
        if (slot.allocation) destroy(*slot.allocation);
    }
    for (auto& [memory, block] : slabBlocks) freeBlock(*block);
}

std::size_t BufferPool::SlabCapacity(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize atom) {
    static const bool disabled = std::getenv("ANYPS5_NO_BUFFER_SLABS") != nullptr;
    if (disabled || size == 0 || size >= SlabLimit) return 0;
    if ((alignment != 0 && !std::has_single_bit(alignment)) || (atom > 1 && !std::has_single_bit(atom))) return 0;
    const auto capacity = std::bit_ceil(std::max<VkDeviceSize>({size, alignment, atom, VkDeviceSize{256}}));
    return capacity < SlabLimit ? static_cast<std::size_t>(capacity) : 0u;
}

std::uint64_t BufferPool::slabKey(std::uint32_t memoryType, std::size_t capacity, bool addressable) {
    return (static_cast<std::uint64_t>(memoryType) << 40u) | (static_cast<std::uint64_t>(addressable ? 1u : 0u) << 39u) | static_cast<std::uint64_t>(capacity);
}

SlabSlot BufferPool::TakeSlot(const Context& context, std::uint32_t memoryType, std::size_t capacity, bool addressable) {
    const auto key = slabKey(memoryType, capacity, addressable);
    const auto takeFrom = [&](SlabBlock& block, Slab& slab) {
        const auto index = block.free.back();
        block.free.pop_back();
        if (block.used++ == 0) --slab.emptyBlocks;
        if (block.free.empty()) slab.available.erase(std::find(slab.available.begin(), slab.available.end(), &block));
        const auto offset = static_cast<VkDeviceSize>(index) * block.slotBytes;
        return SlabSlot{block.memory, offset, block.mapping != nullptr ? block.mapping + offset : nullptr};
    };
    {
        std::lock_guard lock(slabMutex);
        auto& slab = slabs[key];
        if (!slab.available.empty()) return takeFrom(*slab.available.back(), slab);
    }
    const auto blockBytes = slabBlockBytes(capacity);
    auto block = std::make_unique<SlabBlock>();
    block->slotBytes = capacity;
    block->slab = key;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    if (addressable) allocation.pNext = &flags;
    allocation.allocationSize = blockBytes;
    allocation.memoryTypeIndex = memoryType;
    Check(allocateMemory(device, &allocation, nullptr, &block->memory), "vkAllocateMemory buffer slab");
    if ((context.memory.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        void* mapping = nullptr;
        const auto mapped = mapMemory(device, block->memory, 0, VK_WHOLE_SIZE, 0, &mapping);
        if (mapped != VK_SUCCESS) freeMemory(device, block->memory, nullptr);
        Check(mapped, "vkMapMemory buffer slab");
        block->mapping = static_cast<std::byte*>(mapping);
    }
    const auto count = static_cast<std::uint32_t>(blockBytes / capacity);
    block->free.reserve(count);
    for (std::uint32_t index = count; index-- != 0;) block->free.push_back(index);
    std::lock_guard lock(slabMutex);
    auto& slab = slabs[key];
    auto& inserted = *block;
    slabBlocks.emplace(inserted.memory, std::move(block));
    slab.available.push_back(&inserted);
    ++slab.emptyBlocks;
    return takeFrom(*slab.available.back(), slab);
}

void BufferPool::freeBlock(SlabBlock& block) noexcept {
    if (block.mapping != nullptr) unmap(device, block.memory);
    freeMemory(device, block.memory, nullptr);
}

void BufferPool::PutSlot(VkDeviceMemory memory, VkDeviceSize offset) noexcept {
    std::unique_ptr<SlabBlock> released;
    {
        std::lock_guard lock(slabMutex);
        const auto found = slabBlocks.find(memory);
        if (found == slabBlocks.end()) return;
        auto& block = *found->second;
        auto& slab = slabs[block.slab];
        if (block.free.empty()) slab.available.push_back(&block);
        block.free.push_back(static_cast<std::uint32_t>(offset / block.slotBytes));
        if (--block.used != 0) return;
        if (slab.emptyBlocks == 0) {
            ++slab.emptyBlocks;
            return;
        }
        slab.available.erase(std::find(slab.available.begin(), slab.available.end(), &block));
        released = std::move(found->second);
        slabBlocks.erase(found);
    }
    freeBlock(*released);
}

std::size_t BufferPool::bucket(std::size_t bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) noexcept {
    auto hash = bytes ^ (bytes >> 16u);
    hash = hash * 16777619u ^ usage;
    hash = hash * 16777619u ^ properties;
    return (hash ^ (hash >> 8u) ^ (hash >> 16u)) % bucketCount;
}

BufferAllocation BufferPool::remove(std::size_t index) noexcept {
    auto& slot = slots[index];
    const auto allocation = *slot.allocation;
    auto* link = &buckets[bucket(allocation.bytes, allocation.usage, allocation.properties)];
    while (*link != index) link = &slots[*link].next;
    *link = slot.next;
    if (slot.older != none) slots[slot.older].newer = slot.newer;
    else oldest = slot.newer;
    if (slot.newer != none) slots[slot.newer].older = slot.older;
    else newest = slot.older;
    retainedBytes -= allocation.allocationBytes;
    slot = Slot{};
    freeSlots[freeCount++] = index;
    return allocation;
}

void BufferPool::destroy(const BufferAllocation& allocation) noexcept {
    if (allocation.slab) {
        destroyBuffer(device, allocation.buffer, nullptr);
        PutSlot(allocation.memory, allocation.offset);
        return;
    }
    if (allocation.mapping != nullptr) unmap(device, allocation.memory);
    destroyBuffer(device, allocation.buffer, nullptr);
    freeMemory(device, allocation.memory, nullptr);
}

std::optional<BufferAllocation> BufferPool::Take(std::size_t bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    std::lock_guard lock(mutex);
    for (auto index = buckets[bucket(bytes, usage, properties)]; index != none; index = slots[index].next) {
        const auto& allocation = *slots[index].allocation;
        if (allocation.bytes == bytes && allocation.usage == usage && allocation.properties == properties) return remove(index);
    }
    return std::nullopt;
}

void BufferPool::Put(const BufferAllocation& allocation) noexcept {
    std::lock_guard lock(mutex);
    if (allocation.allocationBytes > budget) {
        destroy(allocation);
        return;
    }
    while (retainedBytes > budget - allocation.allocationBytes || freeCount == 0) destroy(remove(oldest));
    const auto index = freeSlots[--freeCount];
    auto& slot = slots[index];
    auto& head = buckets[bucket(allocation.bytes, allocation.usage, allocation.properties)];
    slot.allocation = allocation;
    slot.next = head;
    slot.older = newest;
    if (newest != none) slots[newest].newer = index;
    else oldest = index;
    newest = index;
    head = index;
    retainedBytes += allocation.allocationBytes;
}

std::shared_ptr<BufferPool> GetBufferPool(const Context& context) {
    if (!context.bufferPool) context.bufferPool = std::make_shared<BufferPool>(context);
    return context.bufferPool;
}

}
