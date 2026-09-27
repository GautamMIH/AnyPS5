#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURECACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURECACHE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include <array>
#include <list>
#include <memory>
#include <vector>

namespace AgcDriver::Graphics {

class TextureCache {
public:
    explicit TextureCache(const Context& context);
    std::shared_ptr<Texture> Get(std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components);

private:
    struct Entry {
        // Descriptor words plus the view dimension, which the shader's image shape may change.
        std::array<std::uint32_t, 9> descriptor;
        std::vector<std::byte> snapshot;
        std::shared_ptr<Texture> texture;
        std::weak_ptr<ResidentColor> source;
        std::uint64_t generation = 0;
    };
    void trim();
    Context context;
    std::list<Entry> entries;
    std::uint64_t retainedBytes = 0;
    static constexpr std::uint64_t budget = 1024ull * 1024 * 1024;
};

}

#endif
