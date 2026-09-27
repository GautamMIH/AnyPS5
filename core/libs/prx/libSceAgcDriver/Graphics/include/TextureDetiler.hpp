#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTUREDETILER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTUREDETILER_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include <cstdint>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

    class TextureDetiler {
    public:
        explicit TextureDetiler(const Context& context);
        ~TextureDetiler();
        TextureDetiler(const TextureDetiler&) = delete;
        TextureDetiler& operator=(const TextureDetiler&) = delete;

        // Detiles (or, with tile, tiles) one mip layer between buffers. Descriptor sets come from the
        // detiler's batch pools, which BeginBatch recycles, or from pool when the caller owns one
        // that outlives the command buffer.
        void Dispatch(VkCommandBuffer commands, TextureTileMode tileMode, std::uint32_t elementBytes, VkBuffer source, std::uint64_t sourceOffset, VkBuffer destination, std::uint64_t destinationOffset, const TileMipLayout& layout, std::uint32_t arrayLayer, bool tile = false, VkDescriptorPool pool = VK_NULL_HANDLE);
        void BeginBatch();
        // Copies bytes of source into guest where mask bytes are set (one pass per dword).
        void Merge(VkCommandBuffer commands, VkBuffer source, std::uint64_t sourceOffset, VkBuffer mask, std::uint64_t maskOffset, VkBuffer guest, std::uint64_t guestOffset, std::uint64_t bytes, VkDescriptorPool pool);
        // A pool for the caller's own tiling and merge passes.
        VkDescriptorPool CreatePool(std::uint32_t sets);
        void DestroyPool(VkDescriptorPool pool) noexcept;

    private:
        VkPipeline pipeline(TextureTileMode tileMode, std::uint32_t elementBytes, bool tile);
        void release() noexcept;
        VkDescriptorSet allocateSet();
        VkDescriptorSet allocateSet(VkDescriptorPool pool);

        const Context context;
        VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkShaderModule module = VK_NULL_HANDLE;
        VkDescriptorSetLayout mergeDescriptorLayout = VK_NULL_HANDLE;
        VkPipelineLayout mergePipelineLayout = VK_NULL_HANDLE;
        VkShaderModule mergeModule = VK_NULL_HANDLE;
        VkPipeline mergePipeline = VK_NULL_HANDLE;
        void createMerge();
        std::vector<std::pair<std::uint32_t, VkPipeline>> pipelines;
        std::vector<VkDescriptorPool> descriptorPools;
        std::size_t allocatedSets = 0;
    };

}

#endif
