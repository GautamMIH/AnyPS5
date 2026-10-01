#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUCOLORTRANSFER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUCOLORTRANSFER_HPP

#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <memory>

namespace AgcDriver::Graphics {

class GpuColorTransfer {
public:
    explicit GpuColorTransfer(const Context& context);
    ~GpuColorTransfer();
    GpuColorTransfer(const GpuColorTransfer&) = delete;
    GpuColorTransfer& operator=(const GpuColorTransfer&) = delete;
    void Upload(std::uint64_t address, std::uint32_t width, std::uint32_t height, ColorTileMode mode, std::uint32_t elementBytes = 4, ColorTail tail = {});
    void Detile(VkCommandBuffer commands, bool swapRedBlue = false);
    void Tile(VkCommandBuffer commands);
    // GPU-only transfers between the tiled surface and a buffer holding guest memory (imported):
    // UploadFrom copies the guest bytes in and detiles them; TileTo tiles the linear image data
    // and copies it out. Both are recorded in queue order; the CPU copies nothing.
    void UploadFrom(VkCommandBuffer commands, VkBuffer source, VkDeviceSize offset, std::uint32_t width, std::uint32_t height, ColorTileMode mode, std::uint32_t elementBytes, ColorTail tail);
    void TileTo(VkCommandBuffer commands, VkBuffer destination, VkDeviceSize offset);
    std::uint64_t TiledBytes() const;
    void WriteBack(std::uint64_t address);
    void WriteBackTracked(std::uint64_t address);
    bool MatchesGuest(std::uint64_t address);
    VkBuffer LinearBuffer() const;
    RenderTarget& Target(const ColorTarget& color, bool blending);

private:
    void writeBack(std::uint64_t address, bool tracked);
    void prepare(std::uint32_t width, std::uint32_t height, ColorTileMode mode, std::uint32_t elementBytes, ColorTail tail);
    void convert(VkCommandBuffer commands, bool toTiled, bool swapRedBlue);
    void release() noexcept;
    Context context;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::unique_ptr<Buffer> tiled;
    std::unique_ptr<Buffer> linear;
    std::unique_ptr<Buffer> readback;
    std::unique_ptr<Buffer> upload;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    ColorTileMode mode = ColorTileMode::Linear;
    std::uint32_t elementBytes = 4;
    ColorTail tail;
    std::unique_ptr<RenderTarget> target;
    VkExtent2D targetExtent{};
    VkFormat targetFormat = VK_FORMAT_UNDEFINED;
    bool targetBlending = false;
};

}

#endif
