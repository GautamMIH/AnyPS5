#include "prx/libSceAgcDriver/Graphics/include/DescriptorCache.hpp"
#include <algorithm>
#include <iterator>
#include <utility>

namespace AgcDriver::Graphics {

DescriptorAllocation::DescriptorAllocation(const Context& context, std::span<const std::uint32_t> key, std::span<const VkDescriptorSetLayoutBinding> bindings, std::span<const VkDescriptorPoolSize> sizes, std::span<const VkDescriptorBindingFlags> flags, std::optional<std::uint32_t> variableCount) : device(context.device), destroyPool(context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")), destroyLayout(context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")), key(key.begin(), key.end()) {
    try {
        Require(flags.empty() || flags.size() == bindings.size(), "descriptor binding flags disagree with the bindings");
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bindingFlags.bindingCount = static_cast<std::uint32_t>(flags.size());
        bindingFlags.pBindingFlags = flags.data();
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        if (!flags.empty()) info.pNext = &bindingFlags;
        info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(device, &info, nullptr, &layout), "vkCreateDescriptorSetLayout");
        if (bindings.empty()) return;
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
        const std::uint32_t variable = variableCount.value_or(0u);
        VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
        variableInfo.descriptorSetCount = 1;
        variableInfo.pDescriptorCounts = &variable;
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        if (variableCount.has_value()) allocation.pNext = &variableInfo;
        allocation.descriptorPool = pool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &layout;
        Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(device, &allocation, &set), "vkAllocateDescriptorSets");
    } catch (...) {
        release();
        throw;
    }
}

DescriptorAllocation::~DescriptorAllocation() {
    release();
}

void DescriptorAllocation::release() noexcept {
    if (pool) destroyPool(device, pool, nullptr);
    if (layout) destroyLayout(device, layout, nullptr);
}

std::vector<DescriptorAllocation::WrittenElement>& DescriptorAllocation::Written(std::uint32_t binding) {
    for (auto& [number, elements] : written) {
        if (number == binding) return elements;
    }
    return written.emplace_back(binding, std::vector<WrittenElement>{}).second;
}

bool DescriptorAllocation::Matches(std::span<const std::uint32_t> candidate) const {
    return std::equal(key.begin(), key.end(), candidate.begin(), candidate.end());
}

std::uint64_t DescriptorCache::hash(std::span<const std::uint32_t> key) {
    std::uint64_t value = 0xcbf29ce484222325ull;
    for (const auto word : key) value = (value ^ word) * 0x100000001b3ull;
    return value;
}

std::unique_ptr<DescriptorAllocation> DescriptorCache::Take(std::span<const std::uint32_t> key) {
    std::lock_guard lock(mutex);
    const auto found = available.find(hash(key));
    if (found == available.end()) return nullptr;
    auto& list = found->second;
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        if (!(*it)->Matches(key)) continue;
        auto allocation = std::move(*it);
        list.erase(std::next(it).base());
        --count;
        return allocation;
    }
    return nullptr;
}

void DescriptorCache::Put(std::unique_ptr<DescriptorAllocation> allocation) {
    Require(allocation != nullptr, "cannot cache an empty descriptor allocation");
    std::unique_ptr<DescriptorAllocation> dropped;
    {
        std::lock_guard lock(mutex);
        // Full: the retired allocation is freed (outside the lock) instead of kept.
        if (count >= Capacity) {
            dropped = std::move(allocation);
        } else {
            available[hash(allocation->Key())].push_back(std::move(allocation));
            ++count;
        }
    }
}

}
