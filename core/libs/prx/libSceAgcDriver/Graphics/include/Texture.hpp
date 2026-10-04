#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <optional>
#include <vector>

namespace AgcDriver::Graphics {

class ResidentColor;
class ResidentDepth;
class CommandBatch;
class Buffer;

// The layout in which a depth image is a read-only depth attachment and sampled at once.
inline VkImageLayout ReadOnlyDepthLayout(bool hasStencil) {
    return hasStencil ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
}

class Texture {
public:
    // depthCompare: sampled with a depth comparison, which Vulkan defines on depth formats only, so an
    // R32 float or R16 unorm texture is uploaded into a D32 float or D16 unorm image.
    Texture(const Context& context, TextureDetiler& detiler, const GuestTextureResource& descriptor, VkComponentMapping components, std::span<const std::byte> snapshot, bool depthCompare = false);
    Texture(const Context& context, const std::shared_ptr<ResidentColor>& source, const GuestTextureResource& descriptor, VkComponentMapping components);
    // Samples the depth plane of a resident depth surface (the texture format reinterprets its bits).
    Texture(const Context& context, const std::shared_ptr<ResidentDepth>& depthSource, const GuestTextureResource& descriptor, VkComponentMapping components);
    // Views the resident depth image itself (the texture format samples depth values as they are).
    struct DirectView {};
    Texture(const Context& context, const std::shared_ptr<ResidentDepth>& depthSource, VkComponentMapping components, DirectView);
    // Views the depth image the same work renders to without writing depth: sampled inside the
    // render pass in its read-only depth layout (the pass makes the transitions).
    struct FeedbackView {};
    Texture(const Context& context, const std::shared_ptr<ResidentDepth>& depthSource, VkComponentMapping components, FeedbackView);
    bool Direct() const { return direct; }
    bool DepthFeedback() const { return feedback; }
    // The layout the image is in while shaders sample it.
    VkImageLayout SampledLayout() const;
    // Records what sampling needs before the work's render pass (direct views: the layout).
    void PrepareSampling(VkCommandBuffer commands);
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
    // Allocates and binds the image's memory (from the device's ImageMemory when it has one).
    void bindImageMemory(const char* operation);

    Context context;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::optional<ImageMemory::Allocation> imageAllocation;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceSize allocationBytes = 0;
    std::shared_ptr<ResidentColor> source;
    std::shared_ptr<ResidentDepth> depthSource;
    std::unique_ptr<Buffer> staging;
    std::unique_ptr<CommandBatch> upload;
    // Copies still running on the GPU when a refresh needed a new batch.
    std::vector<std::unique_ptr<CommandBatch>> previousUploads;
    VkExtent3D extent{};
    // A view of another object's image: only the view is owned.
    bool direct = false;
    bool feedback = false;
    // Snapshot uploads: detiling input and output, and the detiler's descriptor pool.
    std::unique_ptr<Buffer> uploadStaging;
    std::unique_ptr<Buffer> uploadLinear;
    TextureDetiler* uploadDetiler = nullptr;
    VkDescriptorPool uploadPool = VK_NULL_HANDLE;
};

}

#endif
