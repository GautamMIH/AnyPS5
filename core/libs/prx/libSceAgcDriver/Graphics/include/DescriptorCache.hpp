#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DESCRIPTORCACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DESCRIPTORCACHE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Graphics {

class DescriptorAllocation {
public:
    // flags: per binding (empty: none); variableCount: the set's count of its variable-count binding
    // (the image table), part of the key the allocation is cached under.
    DescriptorAllocation(const Context& context, std::span<const std::uint32_t> key, std::span<const VkDescriptorSetLayoutBinding> bindings, std::span<const VkDescriptorPoolSize> sizes, std::span<const VkDescriptorBindingFlags> flags = {}, std::optional<std::uint32_t> variableCount = std::nullopt);
    ~DescriptorAllocation();
    DescriptorAllocation(const DescriptorAllocation&) = delete;
    DescriptorAllocation& operator=(const DescriptorAllocation&) = delete;
    VkDescriptorSetLayout Layout() const { return layout; }
    VkDescriptorSet Set() const { return set; }
    bool Matches(std::span<const std::uint32_t> candidate) const;
    std::span<const std::uint32_t> Key() const { return key; }
    // What a table binding's elements of the set hold (written by an earlier user of a cached
    // allocation): an element still holding the same object's view or sampler in the same layout
    // need not be written again. The owner is held weakly, so an element whose object was destroyed
    // (and whose handle may have been reused) never matches.
    struct WrittenElement {
        std::weak_ptr<const void> owner;
        const void* object = nullptr;
        std::uint64_t handle = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        [[nodiscard]] bool Holds(const void* candidate, std::uint64_t candidateHandle, VkImageLayout candidateLayout) const {
            return object == candidate && handle == candidateHandle && layout == candidateLayout && !owner.expired();
        }
    };
    std::vector<WrittenElement>& Written(std::uint32_t binding);

private:
    void release() noexcept;
    VkDevice device;
    PFN_vkDestroyDescriptorPool destroyPool;
    PFN_vkDestroyDescriptorSetLayout destroyLayout;
    std::vector<std::uint32_t> key;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<std::pair<std::uint32_t, std::vector<WrittenElement>>> written;
};

// Retired descriptor allocations by layout key, for draws with the same layout. A draw's set returns
// only once the GPU finished the draw, so a frame keeps many sets of each layout in flight: a single
// 256-entry ring shared by every layout missed for most of Zorro's ~1300 draws a frame (a layout, a
// pool and a set created per miss, ~1 ms a frame) and its linear key scan cost ~0.3 ms.
class DescriptorCache {
public:
    std::unique_ptr<DescriptorAllocation> Take(std::span<const std::uint32_t> key);
    void Put(std::unique_ptr<DescriptorAllocation> allocation);

private:
    static std::uint64_t hash(std::span<const std::uint32_t> key);
    // Allocations kept in all (a bound on descriptor pools held idle); beyond it, retired ones are freed.
    static constexpr std::size_t Capacity = 4096;
    std::mutex mutex;
    // By key hash; entries of colliding keys share a list and are told apart by Matches.
    std::unordered_map<std::uint64_t, std::vector<std::unique_ptr<DescriptorAllocation>>> available;
    std::size_t count = 0;
};

}

#endif
