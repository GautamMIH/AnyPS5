#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"

namespace AgcDriver::Graphics {

void TextureDetiler::BeginBatch() {
    for (const auto pool : descriptorPools) {
        Check(context.Function<PFN_vkResetDescriptorPool>("vkResetDescriptorPool")(context.device, pool, 0), "vkResetDescriptorPool texture detiler");
    }
    allocatedSets = 0;
}

VkDescriptorPool TextureDetiler::CreatePool(std::uint32_t sets) {
    // Tiling sets bind two buffers, merge sets three.
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets * 3};
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = sets;
    info.poolSizeCount = 1;
    info.pPoolSizes = &size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &info, nullptr, &pool), "vkCreateDescriptorPool texture tiling");
    return pool;
}

void TextureDetiler::DestroyPool(VkDescriptorPool pool) noexcept {
    if (pool != VK_NULL_HANDLE) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
}

VkDescriptorSet TextureDetiler::allocateSet(VkDescriptorPool pool) {
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = pool;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &descriptorLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocation, &set), "vkAllocateDescriptorSets texture tiling");
    return set;
}

VkDescriptorSet TextureDetiler::allocateSet() {
    constexpr std::uint32_t setsPerPool = 64;
    const auto poolIndex = allocatedSets / setsPerPool;
    if (poolIndex == descriptorPools.size()) {
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setsPerPool * 2};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = setsPerPool;
        info.poolSizeCount = 1;
        info.pPoolSizes = &size;
        descriptorPools.reserve(descriptorPools.size() + 1);
        VkDescriptorPool pool = VK_NULL_HANDLE;
        Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &info, nullptr, &pool), "vkCreateDescriptorPool texture detiler");
        descriptorPools.push_back(pool);
    }
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = descriptorPools.at(poolIndex);
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &descriptorLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocation, &set), "vkAllocateDescriptorSets texture detiler");
    ++allocatedSets;
    return set;
}

}
