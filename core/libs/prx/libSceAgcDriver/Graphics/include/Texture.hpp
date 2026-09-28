#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"

namespace AgcDriver::Graphics {

class ResidentColor;
class ResidentDepth;
class CommandBatch;
class Buffer;

class Texture {
public:
    Texture(const Context& context, TextureDetiler& detiler, const GuestTextureResource& descriptor, VkComponentMapping components, std::span<const std::byte> snapshot);
    Texture(const Context& context, const std::shared_ptr<ResidentColor>& source, const GuestTextureResource& descriptor, VkComponentMapping components);
    // Samples the depth plane of a resident depth surface (the texture format reinterprets its bits).
    Texture(const Context& context, const std::shared_ptr<ResidentDepth>& depthSource, const GuestTextureResource& descriptor, VkComponentMapping components);
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    VkImageView View() const;
    VkDeviceSize AllocationBytes() const { return allocationBytes; }
    // Whether the GPU finished filling the image (destroying it earlier waits for the queue).
    bool UploadComplete() const;

private:
    void release() noexcept;

    Context context;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceSize allocationBytes = 0;
    std::shared_ptr<ResidentColor> source;
    std::shared_ptr<ResidentDepth> depthSource;
    std::unique_ptr<Buffer> staging;
    std::unique_ptr<CommandBatch> upload;
};

}

#endif
