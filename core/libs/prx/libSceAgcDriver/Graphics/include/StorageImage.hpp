#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STORAGEIMAGE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STORAGEIMAGE_HPP

#include <array>
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestGpuMemory.hpp"
#include <optional>
#include <memory>
#include <vector>

namespace AgcDriver::Graphics {

// One mip of a guest texture that shaders write (image_store). The guest texels are detiled into a
// host image before the draw or dispatch and tiled back into guest memory once it completes. When
// the guest memory is shared with the GPU (GuestGpuMemory) both happen on the GPU in the same
// command buffer; otherwise on the CPU around it.
class StorageImage {
public:
    StorageImage(const Context& context, const GuestTextureResource& resource);
    ~StorageImage();
    StorageImage(const StorageImage&) = delete;
    StorageImage& operator=(const StorageImage&) = delete;
    VkImageView View() const { return view; }
    void RecordUpload(VkCommandBuffer commands);
    void RecordDownload(VkCommandBuffer commands);
    void WriteBack();
    bool Overlaps(std::uint64_t address, std::size_t bytes) const;
    // The guest range the image is written back to: [first, second).
    std::pair<std::uint64_t, std::uint64_t> Range() const;

private:
    void release() noexcept;
    void recordGpuUpload(VkCommandBuffer commands);
    void recordGpuDownload(VkCommandBuffer commands);
    std::vector<VkBufferImageCopy> imageCopies() const;
    Context context;
    GuestTextureResource resource;
    TileMipLayout mip{};
    std::uint32_t elementBytes = 0;
    std::uint32_t layers = 1;
    bool volume = false;
    bool thick = false;
    std::uint64_t sliceBytes = 0;
    std::uint64_t guestBase = 0;
    std::vector<std::byte> tiled;
    std::unique_ptr<Buffer> staging;
    // GPU tiling: guest memory, the texels tiled on the device with a mask of the bytes they
    // cover (write-back replaces only those, so images sharing a mip tail keep each other's
    // texels), an all-ones linear image for tiling the mask, and the passes' descriptor sets.
    std::optional<GuestGpuMemory::View> guest;
    std::uint64_t guestBytes = 0;
    std::unique_ptr<Buffer> tiledStaging;
    std::unique_ptr<Buffer> tiledMask;
    std::unique_ptr<Buffer> ones;
    VkDescriptorPool tilingPool = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    // The pool shape the image returns to on release (ANYPS5_STORAGE_IMAGE_POOL=1).
    std::optional<std::array<std::uint64_t, 7>> poolShape;
};

}

#endif
