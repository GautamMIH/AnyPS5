#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STORAGEIMAGE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STORAGEIMAGE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include <memory>
#include <vector>

namespace AgcDriver::Graphics {

// One mip of a guest texture that shaders write (image_store). The guest texels are detiled into a
// host image before the draw or dispatch and tiled back into guest memory once it completes.
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

private:
    void release() noexcept;
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
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

}

#endif
