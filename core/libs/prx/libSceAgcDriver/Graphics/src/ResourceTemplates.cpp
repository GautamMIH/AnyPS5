#include "prx/libSceAgcDriver/Graphics/include/ResourceTemplates.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"

namespace AgcDriver::Graphics {

std::shared_ptr<ShaderResources> ResourceTemplates::Find(const std::vector<std::uint64_t>& key, std::uint64_t hash) {
    const auto [first, last] = index.equal_range(hash);
    for (auto it = first; it != last; ++it) {
        if (it->second->key != key) continue;
        entries.splice(entries.end(), entries, it->second);
        return it->second->resources;
    }
    return nullptr;
}

void ResourceTemplates::Store(std::vector<std::uint64_t> key, std::uint64_t hash, std::shared_ptr<ShaderResources> resources) {
    PerformanceTimer timing("Graphics.ResourceTemplate.Store");
    Erase(key, hash);
    timing.Mark("erase");
    if (entries.size() >= Capacity) eraseEntry(entries.begin(), Hash(entries.begin()->key));
    timing.Mark("evict");
    entries.push_back({std::move(key), std::move(resources)});
    const auto entry = std::prev(entries.end());
    index.emplace(hash, entry);
    timing.Mark("insert");
    for (const auto& texture : entry->resources->Textures()) textureIndex[texture.get()].emplace(&*entry, entry);
    timing.Mark("texture_index");
}

void ResourceTemplates::Erase(const std::vector<std::uint64_t>& key, std::uint64_t hash) {
    const auto [first, last] = index.equal_range(hash);
    for (auto it = first; it != last; ++it) {
        if (it->second->key != key) continue;
        eraseEntry(it->second, hash);
        return;
    }
}

void ResourceTemplates::DropTexture(const Texture* texture) {
    for (auto found = textureIndex.find(texture); found != textureIndex.end(); found = textureIndex.find(texture)) {
        const auto entry = found->second.begin()->second;
        eraseEntry(entry, Hash(entry->key));
    }
}

void ResourceTemplates::eraseEntry(Entries::iterator entry, std::uint64_t hash) {
    PerformanceTimer timing("Graphics.ResourceTemplate.EraseEntry");
    const auto [first, last] = index.equal_range(hash);
    for (auto it = first; it != last; ++it) {
        if (it->second != entry) continue;
        index.erase(it);
        break;
    }
    for (const auto& texture : entry->resources->Textures()) {
        const auto found = textureIndex.find(texture.get());
        if (found == textureIndex.end()) continue;
        found->second.erase(&*entry);
        if (found->second.empty()) textureIndex.erase(found);
    }
    timing.Mark("unindex");
    entries.erase(entry);
    timing.Mark("destroy");
}

void ResourceTemplates::Clear() {
    textureIndex.clear();
    index.clear();
    entries.clear();
}

}
