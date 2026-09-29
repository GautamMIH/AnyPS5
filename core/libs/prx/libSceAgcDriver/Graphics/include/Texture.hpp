#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include <vector>

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
    // Resident copies: copies the source again into the same image after the source changed.
    void Refresh();
    // Frees the buffers of a completed upload; false while it still runs.
    bool ReleaseUpload();

private:
    void release() noexcept;
    void recordCopy();
    void releaseUploadResources() noexcept;

    Context context;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceSize allocationBytes = 0;
    std::shared_ptr<ResidentColor> source;
    std::shared_ptr<ResidentDepth> depthSource;
    std::unique_ptr<Buffer> staging;
    std::unique_ptr<CommandBatch> upload;
    // Copies still running on the GPU when a refresh needed a new batch.
    std::vector<std::unique_ptr<CommandBatch>> previousUploads;
    VkExtent3D extent{};
    // Snapshot uploads: detiling input and output, and the detiler's descriptor pool.
    std::unique_ptr<Buffer> uploadStaging;
    std::unique_ptr<Buffer> uploadLinear;
    TextureDetiler* uploadDetiler = nullptr;
    VkDescriptorPool uploadPool = VK_NULL_HANDLE;
};

}

#endif
