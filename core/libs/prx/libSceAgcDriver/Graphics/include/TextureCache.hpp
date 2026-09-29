#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURECACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURECACHE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include <array>
#include <chrono>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Graphics {

class TextureCache {
public:
    explicit TextureCache(const Context& context);
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;
    std::shared_ptr<Texture> Get(std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components);

private:
    // Descriptor words plus the view dimension, which the shader's image shape may change.
    using Key = std::array<std::uint32_t, 9>;
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept;
    };
    struct Entry {
        Key descriptor;
        std::vector<std::byte> snapshot;
        std::shared_ptr<Texture> texture;
        std::weak_ptr<ResidentColor> source;
        // Non-zero for entries copied from a resident target: that target's generation at the copy.
        std::uint64_t generation = 0;
        std::weak_ptr<ResidentDepth> depthSource;
        // Write tracking of the snapshot's guest range (see WriteTracker): the snapshot is still the
        // guest's contents while no write can have reached the range since it was taken or compared.
        std::uint64_t address = 0;
        std::uint64_t bytes = 0;
        std::uint64_t aliasGeneration = 0;
        std::uint64_t cpuGeneration = 0;
        std::uint64_t gpuGeneration = ~0ull;
        std::uint64_t viewGeneration = ~0ull;
        bool gpuWritten = true;
        bool singleView = false;
        // Tracked entries keep no snapshot (write tracking alone vouches for them); a possibly
        // written tracked entry is recreated.
        bool tracked = false;
    };
    using Entries = std::list<Entry>;

    void trim();
    void erase(Entries::iterator it);
    bool unchanged(Entry& entry);

    Context context;
    Entries entries;
    std::unordered_map<Key, Entries::iterator, KeyHash> index;
    // Evicted textures whose upload may still run on the GPU, released once it completed.
    std::vector<std::shared_ptr<Texture>> retiring;
    // Textures whose upload buffers are freed once the GPU completes the upload.
    std::vector<std::shared_ptr<Texture>> uploading;
    std::uint64_t retainedBytes = 0;
    static constexpr std::uint64_t budget = 1024ull * 1024 * 1024;
    static constexpr std::size_t maxEntries = 8192;
};

}

#endif
