#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LAYEREDCOLORTRANSFER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LAYEREDCOLORTRANSFER_HPP

#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestGpuMemory.hpp"
#include <memory>
#include <vector>

namespace AgcDriver::Graphics {

// Guest transfers of a colour target that is one or more slices of an array or volume surface
// (ColorTarget::Layered). Slices are detiled on the CPU with the texture addressing used for
// sampling (TexelOffset with the absolute slice index), into a layer-major linear buffer that is
// copied to and from a 2D-array render target.
class LayeredColorTransfer {
public:
    LayeredColorTransfer(const Context& context, const ColorTarget& color);
    ~LayeredColorTransfer();
    LayeredColorTransfer(const LayeredColorTransfer&) = delete;
    LayeredColorTransfer& operator=(const LayeredColorTransfer&) = delete;
    RenderTarget& Target() { return *target; }
    // Reads the rendered slices from guest memory and detiles them into the linear buffer.
    void Upload();
    void RecordToImage(VkCommandBuffer commands);
    void RecordFromImage(VkCommandBuffer commands);
    // Tiles the linear buffer back into the slices and writes them to guest memory.
    void WriteBackTracked();

    // GPU transfers over imported guest memory: one detile (or tile) pass per slice between the
    // guest view and a device-local linear buffer, recorded in queue order. PrepareGpu (re)builds
    // the passes for the target's current guest view; false when the GPU path does not apply.
    bool PrepareGpu();
    // Detiles the slices from guest memory into the image (left in TRANSFER_DST_OPTIMAL by the caller's transition).
    void RecordGpuUpload(VkCommandBuffer commands);
    // Copies the image (in TRANSFER_SRC_OPTIMAL) into the slices in guest memory.
    void RecordGpuWriteBack(VkCommandBuffer commands);
    VkBuffer GpuLinearBuffer() const { return gpuLinear->Handle(); }

private:
    std::uint64_t texelOffset(std::uint32_t layer, std::uint32_t x, std::uint32_t y) const;
    Context context;
    ColorTarget color;
    TileMipLayout mip{};
    std::unique_ptr<RenderTarget> target;
    std::unique_ptr<Buffer> linear;
    std::vector<std::byte> tiled;
    std::unique_ptr<Buffer> gpuLinear;
    VkDescriptorPool gpuPool = VK_NULL_HANDLE;
    VkBuffer gpuGuest = VK_NULL_HANDLE;
    VkDeviceSize gpuGuestOffset = 0;
    std::vector<TextureDetiler::PreparedPass> detilePasses;
    std::vector<TextureDetiler::PreparedPass> tilePasses;
};

}

#endif
