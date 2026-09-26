#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LAYEREDCOLORTRANSFER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LAYEREDCOLORTRANSFER_HPP

#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
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
    LayeredColorTransfer(const LayeredColorTransfer&) = delete;
    LayeredColorTransfer& operator=(const LayeredColorTransfer&) = delete;
    RenderTarget& Target() { return *target; }
    // Reads the rendered slices from guest memory and detiles them into the linear buffer.
    void Upload();
    void RecordToImage(VkCommandBuffer commands);
    void RecordFromImage(VkCommandBuffer commands);
    // Tiles the linear buffer back into the slices and writes them to guest memory.
    void WriteBackTracked();

private:
    std::uint64_t texelOffset(std::uint32_t layer, std::uint32_t x, std::uint32_t y) const;
    Context context;
    ColorTarget color;
    TileMipLayout mip{};
    std::unique_ptr<RenderTarget> target;
    std::unique_ptr<Buffer> linear;
    std::vector<std::byte> tiled;
};

}

#endif
