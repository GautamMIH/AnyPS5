#include "prx/libSceAgcDriver/Graphics/include/TextureCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureAddressing.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RenderCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace AgcDriver::Graphics {

TextureCache::TextureCache(const Context& context) : context(context) {
    Require(context.detiler != nullptr, "texture cache requires a device detiler");
}

void TextureCache::trim() {
    for (auto it = entries.begin(); it != entries.end() && (retainedBytes > budget || entries.size() > 1024);) {
        if (it->texture.use_count() != 1) {
            ++it;
            continue;
        }
        retainedBytes -= it->snapshot.size() + it->texture->AllocationBytes();
        it = entries.erase(it);
    }
}

std::shared_ptr<Texture> TextureCache::Get(std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components) {
    Require(words.size() == 8, "texture cache descriptor must contain eight DWORDs");
    PerformanceTimer timing("Graphics.TextureCache");
    trim();
    std::array<std::uint32_t, 9> key;
    std::copy(words.begin(), words.end(), key.begin());
    key[8] = static_cast<std::uint32_t>(resource.dimension) | (static_cast<std::uint32_t>(resource.viewDimension) << 8u);
    auto source = context.renderCache ? context.renderCache->Find(resource.baseAddress) : nullptr;
    if (source) {
        const auto& color = source->Description();
        const auto compatibleTiling = (color.tileMode == ColorTileMode::RenderTarget && resource.tileMode == TextureTileMode::RenderTarget64KB) || (color.tileMode == ColorTileMode::Linear && resource.tileMode == TextureTileMode::kLinear);
        if (!compatibleTiling || resource.width != color.extent.width || resource.height != color.extent.height || resource.dimension != TextureDimension::k2D || resource.mipCount != 1 || resource.baseLevel != 0 || resource.baseArray != 0 || IsBlockCompressed(resource.format) || BytesPerElement(resource.format) != color.elementBytes) source.reset();
    }
    // Why a lookup misses, for the frame profile: the entry's memory changed, or no entry exists.
    const char* missReason = "miss_absent";
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->descriptor != key) continue;
        missReason = "miss_changed";
        if (source || it->generation != 0) {
            if (source && it->source.lock() == source && it->generation == source->Generation()) {
                auto result = it->texture;
                entries.splice(entries.end(), entries, it);
                return result;
            }
            retainedBytes -= it->snapshot.size() + it->texture->AllocationBytes();
            entries.erase(it);
            break;
        }
        GuestMemory::CheckRange(reinterpret_cast<const void*>(resource.baseAddress), it->snapshot.size(), 1);
        timing.Mark("range_check");
        const bool same = std::memcmp(reinterpret_cast<const void*>(resource.baseAddress), it->snapshot.data(), it->snapshot.size()) == 0;
        timing.Mark("validate_compare", it->snapshot.size());
        if (same) {
            auto result = it->texture;
            entries.splice(entries.end(), entries, it);
            return result;
        }
        retainedBytes -= it->snapshot.size() + it->texture->AllocationBytes();
        entries.erase(it);
        break;
    }
    timing.Mark("lookup");
    if (source) {
        auto texture = std::make_shared<Texture>(context, source, resource, components);
        timing.Mark("render_texture");
        entries.push_back({key, {}, texture, source, source->Generation()});
        retainedBytes += texture->AllocationBytes();
        trim();
        return texture;
    }
    const auto bytes = GuestTextureBytes(resource);
    Require(bytes != 0 && bytes <= std::numeric_limits<std::size_t>::max(), "texture cache surface size overflow");
    std::vector<std::byte> snapshot(static_cast<std::size_t>(bytes));
    GuestMemory::Read(resource.baseAddress, snapshot, 1);
    timing.Mark("miss_read", snapshot.size());
    timing.Mark(missReason, snapshot.size());
    auto texture = std::make_shared<Texture>(context, *context.detiler, resource, components, snapshot);
    timing.Mark("miss_create");
    const auto retained = snapshot.size() + texture->AllocationBytes();
    entries.push_back({key, std::move(snapshot), texture});
    retainedBytes += retained;
    trim();
    return texture;
}

}
