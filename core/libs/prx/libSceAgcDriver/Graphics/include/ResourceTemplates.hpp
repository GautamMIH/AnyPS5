#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESOURCETEMPLATES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESOURCETEMPLATES_HPP

#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AgcDriver::Graphics {

class ShaderResources;
class Texture;

// Draws whose shader resources were built from exactly the same inputs (programs, guest descriptor
// words, targets, index range, rendered depth) reuse the built ShaderResources: its descriptor set,
// textures, samplers, data buffers and in-place guest buffer views. Only resources that write
// nothing on the GPU are kept (ShaderResources::Reusable); a hit is revalidated before use
// (ShaderResources::Revalidate). Least recently used entries go first.
class ResourceTemplates {
public:
    static std::uint64_t Hash(const std::vector<std::uint64_t>& key) {
        std::uint64_t hash = 0xcbf29ce484222325ull;
        for (const auto word : key) hash = (hash ^ word) * 0x100000001b3ull;
        return hash;
    }
    std::shared_ptr<ShaderResources> Find(const std::vector<std::uint64_t>& key, std::uint64_t hash);
    void Store(std::vector<std::uint64_t> key, std::uint64_t hash, std::shared_ptr<ShaderResources> resources);
    void Erase(const std::vector<std::uint64_t>& key, std::uint64_t hash);
    // The texture cache evicts the texture: templates binding it go too (a reuse would rebuild them
    // anyway), so they do not keep evicted textures' memory alive.
    void DropTexture(const Texture* texture);
    void Clear();

private:
    struct Entry {
        std::vector<std::uint64_t> key;
        std::shared_ptr<ShaderResources> resources;
    };
    using Entries = std::list<Entry>;
    static constexpr std::size_t Capacity = 32768;
    Entries entries;
    std::unordered_multimap<std::uint64_t, Entries::iterator> index;
    // The templates binding each texture (a shared texture is bound by thousands: removal is by entry).
    std::unordered_map<const Texture*, std::unordered_map<const Entry*, Entries::iterator>> textureIndex;
    void eraseEntry(Entries::iterator entry, std::uint64_t hash);
};

}

#endif
