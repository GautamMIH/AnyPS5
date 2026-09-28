#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RENDERCACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RENDERCACHE_HPP

#include "prx/libSceAgcDriver/Graphics/include/GpuColorTransfer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/LayeredColorTransfer.hpp"
#include "prx/libc/include/GuestMemoryTracking.hpp"
#include <map>
#include <vector>

namespace AgcDriver::Graphics {

class ResidentColor {
public:
    ResidentColor(const Context& context, const ColorTarget& color);
    ~ResidentColor();
    void Begin(VkCommandBuffer commands);
    void Download(VkCommandBuffer commands);
    void Commit();
    void Transition(VkCommandBuffer commands, VkImageLayout layout);
    RenderTarget& Target() { return layered ? layered->Target() : transfer.Target(color, false); }
    const ColorTarget& Description() const { return color; }
    bool Valid() const { return valid; }
    bool Dirty() const { return dirty; }
    std::uint64_t Generation() const { return generation; }
    void Invalidate();
    void ReleaseMemory();
    bool SharesPages(const ColorTarget& other) const;

private:
    void resolveCpuAccess(GuestMemoryTracking::Access access);
    Context context;
    ColorTarget color;
    GpuColorTransfer transfer;
    // Array and volume slices use CPU transfers instead of the GPU detile path.
    std::unique_ptr<LayeredColorTransfer> layered;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool valid = false;
    bool dirty = false;
    std::uint64_t generation = 0;
    std::unique_ptr<GuestMemoryTracking::Watch> memoryWatch;
};

// A depth/stencil surface kept on the GPU between draws. Guest memory holds the PS5 tiled depth
// and stencil planes; they are detiled on upload and written back when the guest touches them.
class ResidentDepth {
public:
    ResidentDepth(const Context& context, const DepthTarget& depth);
    ~ResidentDepth();
    void Begin(VkCommandBuffer commands);
    void Download(VkCommandBuffer commands);
    void Commit();
    RenderTarget& Target() { return *target; }
    const DepthTarget& Description() const { return depth; }
    bool Valid() const { return valid; }
    bool Dirty() const { return dirty; }
    void Invalidate();
    void ReleaseMemory();
    bool Overlaps(std::uint64_t address, std::size_t bytes) const;
    bool SharesPages(const DepthTarget& other) const;
    // Changes whenever a draw may have written the depth image.
    std::uint64_t Generation() const { return generation; }
    // Bytes per depth texel in the host image (and in CopyDepth's output).
    std::uint32_t HostDepthBytes() const;
    // Records a copy of the depth plane, row-major and unpadded, into buffer.
    void CopyDepth(VkCommandBuffer commands, VkBuffer buffer);

private:
    void transition(VkCommandBuffer commands, VkImageLayout next);
    void copy(VkCommandBuffer commands, bool toImage);
    void protect(GuestMemoryTracking::Protection protection);
    void resolveCpuAccess(GuestMemoryTracking::Access access);
    Context context;
    DepthTarget depth;
    std::unique_ptr<RenderTarget> target;
    std::unique_ptr<Buffer> depthLinear;
    std::unique_ptr<Buffer> stencilLinear;
    // Guest planes as last uploaded; the guest cannot change them while they are resident, so
    // write-back tiles into these copies instead of reading protected memory.
    std::vector<std::byte> depthTiled;
    std::vector<std::byte> stencilTiled;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool valid = false;
    bool dirty = false;
    std::uint64_t generation = 1;
    std::vector<std::unique_ptr<GuestMemoryTracking::Watch>> watches;
};

class RenderCache {
public:
    explicit RenderCache(const Context& context) : context(context) {}
    ~RenderCache();
    std::shared_ptr<ResidentColor> Get(const ColorTarget& color, bool blending);
    std::shared_ptr<ResidentDepth> GetDepth(const DepthTarget& depth);
    std::shared_ptr<ResidentColor> Find(std::uint64_t address) const;
    // The resident depth surface whose depth plane starts at address, if its image is current.
    std::shared_ptr<ResidentDepth> FindDepth(std::uint64_t address) const;
    void Resolve(std::uint64_t address, std::size_t bytes, bool writable);
    void Flush();

private:
    Context context;
    std::map<std::uint64_t, std::shared_ptr<ResidentColor>> entries;
    std::vector<std::shared_ptr<ResidentDepth>> depthEntries;
};

}

#endif
