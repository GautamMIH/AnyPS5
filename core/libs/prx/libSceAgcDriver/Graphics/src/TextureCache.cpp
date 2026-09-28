#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureAddressing.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RenderCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace AgcDriver::Graphics {

namespace {

// Soft-dirty bits are cleared once enough lookups had to compare pages written since the last clear,
// and not more often than kClearInterval (a clear walks every page table of the process). A clear
// also runs every kIdleClearInterval, so snapshots of unchanged textures are released.
constexpr std::uint64_t kComparesBeforeClear = 64;
constexpr auto kClearInterval = std::chrono::seconds(1);
constexpr auto kIdleClearInterval = std::chrono::seconds(2);

}

std::size_t TextureCache::KeyHash::operator()(const Key& key) const noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto word : key) {
        hash ^= word;
        hash *= 0x100000001b3ull;
    }
    return static_cast<std::size_t>(hash);
}

TextureCache::TextureCache(const Context& context) : context(context) {
    Require(context.detiler != nullptr, "texture cache requires a device detiler");
    WriteTracker::AddClearListener(this, [this] { recordWritesBeforeClear(); });
}

TextureCache::~TextureCache() {
    WriteTracker::RemoveClearListener(this);
}

void TextureCache::erase(Entries::iterator it) {
    retainedBytes -= it->snapshot.size() + it->texture->AllocationBytes();
    if (it->texture.use_count() == 1 && !it->texture->UploadComplete()) retiring.push_back(std::move(it->texture));
    index.erase(it->descriptor);
    entries.erase(it);
}

void TextureCache::trim() {
    std::erase_if(retiring, [](const std::shared_ptr<Texture>& texture) { return texture->UploadComplete(); });
    for (auto it = entries.begin(); it != entries.end() && (retainedBytes > budget || entries.size() > maxEntries);) {
        if (it->texture.use_count() != 1) {
            ++it;
            continue;
        }
        const auto next = std::next(it);
        erase(it);
        it = next;
    }
}

// Whether the guest range of a snapshot certainly still holds the snapshot's bytes.
bool TextureCache::unchanged(Entry& entry) {
    if (!WriteTracker::Available() || entry.mustCompare || entry.stale) return false;
    const auto bytes = entry.bytes;
    // Writes through another mapping of the same memory would not mark these pages.
    const auto memoryGeneration = GuestMemoryBacking::GuestMemoryBackingGeneration_nid_postfix();
    if (entry.viewGeneration != memoryGeneration) {
        entry.singleView = GuestMemoryBacking::GuestVirtualSingleView_nid_postfix(entry.address, bytes);
        entry.viewGeneration = memoryGeneration;
    }
    if (!entry.singleView) return false;
    const auto gpuGeneration = WriteTracker::GpuWriteGeneration();
    if (entry.gpuGeneration != gpuGeneration) {
        entry.gpuWritten = WriteTracker::GpuWritten(entry.address, bytes);
        entry.gpuGeneration = gpuGeneration;
    }
    if (entry.gpuWritten || WriteTracker::AliasWrittenSince(entry.address, bytes, entry.aliasGeneration)) return false;
    if (WriteTracker::CpuWritten(entry.address, bytes)) {
        ++dirtyCompares;
        return false;
    }
    return true;
}

// Runs before the soft-dirty bits are cleared: entries written since the previous clear must not
// look clean afterwards, and entries confirmed unchanged no longer need their snapshots.
void TextureCache::recordWritesBeforeClear() {
    for (auto& entry : entries) {
        if (entry.bytes == 0 || entry.mustCompare || entry.stale) continue;
        const auto before = dirtyCompares;
        const bool clean = unchanged(entry);
        dirtyCompares = before;
        if (clean) {
            if (!entry.tracked) {
                retainedBytes -= entry.snapshot.size();
                std::vector<std::byte>().swap(entry.snapshot);
                entry.tracked = true;
            }
        } else if (entry.tracked) {
            entry.stale = true;
        } else if (WriteTracker::CpuWritten(entry.address, entry.bytes)) {
            entry.mustCompare = true;
        }
    }
}

void TextureCache::clearWhenUseful() {
    if (!WriteTracker::Available()) return;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - lastClear;
    if (elapsed < kClearInterval || (dirtyCompares < kComparesBeforeClear && elapsed < kIdleClearInterval)) return;
    PerformanceTimer timing("Graphics.TextureCache.SoftDirtyClear");
    WriteTracker::Clear();
    timing.Mark("clear");
    dirtyCompares = 0;
    lastClear = now;
}

std::shared_ptr<Texture> TextureCache::Get(std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components) {
    Require(words.size() == 8, "texture cache descriptor must contain eight DWORDs");
    PerformanceTimer timing("Graphics.TextureCache");
    trim();
    clearWhenUseful();
    Key key;
    std::copy(words.begin(), words.end(), key.begin());
    key[8] = static_cast<std::uint32_t>(resource.dimension) | (static_cast<std::uint32_t>(resource.viewDimension) << 8u);
    auto source = context.renderCache ? context.renderCache->Find(resource.baseAddress) : nullptr;
    if (source) {
        const auto& color = source->Description();
        const auto compatibleTiling = (color.tileMode == ColorTileMode::RenderTarget && resource.tileMode == TextureTileMode::RenderTarget64KB) || (color.tileMode == ColorTileMode::Linear && resource.tileMode == TextureTileMode::kLinear);
        if (!compatibleTiling || resource.width != color.extent.width || resource.height != color.extent.height || resource.dimension != TextureDimension::k2D || resource.mipCount != 1 || resource.baseLevel != 0 || resource.baseArray != 0 || IsBlockCompressed(resource.format) || BytesPerElement(resource.format) != color.elementBytes) source.reset();
    }
    // A texture over a resident depth plane in its depth layout is copied from the depth image.
    std::shared_ptr<ResidentDepth> depthSource;
    if (!source && context.renderCache && resource.tileMode == TextureTileMode::Depth64KB) {
        depthSource = context.renderCache->FindDepth(resource.baseAddress);
        if (depthSource) {
            const auto& depth = depthSource->Description();
            if (resource.dimension != TextureDimension::k2D || resource.mipCount != 1 || resource.baseLevel != 0 || resource.baseArray != 0 || IsBlockCompressed(resource.format) || resource.width != depth.extent.width || resource.height != depth.extent.height || BytesPerElement(resource.format) != depth.depthElementBytes || depthSource->HostDepthBytes() != depth.depthElementBytes) depthSource.reset();
        }
    }
    // Why a lookup misses, for the frame profile: the entry's memory changed, or no entry exists.
    const char* missReason = "miss_absent";
    if (const auto found = index.find(key); found != index.end()) {
        const auto it = found->second;
        missReason = "miss_changed";
        if (source || depthSource || it->generation != 0) {
            const bool current = (source && it->source.lock() == source && it->generation == source->Generation()) || (depthSource && it->depthSource.lock() == depthSource && it->generation == depthSource->Generation());
            if (current) {
                auto result = it->texture;
                entries.splice(entries.end(), entries, it);
                return result;
            }
            erase(it);
        } else {
            // Resident render targets over the range reach guest memory first (written through the
            // alias, which the write tracker records).
            const GuestMemory::AccessSite site("cpu_wait_texture");
            GuestMemory::CheckRange(reinterpret_cast<const void*>(resource.baseAddress), static_cast<std::size_t>(it->bytes), 1);
            timing.Mark("range_check");
            if (unchanged(*it)) {
                timing.Mark("validate_tracked");
                auto result = it->texture;
                entries.splice(entries.end(), entries, it);
                return result;
            }
            if (it->tracked) {
                // No snapshot to compare with: a possibly written tracked texture is recreated.
                ++dirtyCompares;
                erase(it);
                goto create;
            }
            const auto aliasGeneration = WriteTracker::AliasWriteGeneration();
            const bool same = std::memcmp(reinterpret_cast<const void*>(resource.baseAddress), it->snapshot.data(), it->snapshot.size()) == 0;
            timing.Mark("validate_compare", it->snapshot.size());
            if (same) {
                it->mustCompare = false;
                it->aliasGeneration = aliasGeneration;
                auto result = it->texture;
                entries.splice(entries.end(), entries, it);
                return result;
            }
            erase(it);
        }
    }
create:
    timing.Mark("lookup");
    if (depthSource) {
        auto texture = std::make_shared<Texture>(context, depthSource, resource, components);
        timing.Mark("depth_texture");
        Entry entry{key, {}, texture, {}, depthSource->Generation()};
        entry.depthSource = depthSource;
        entries.push_back(std::move(entry));
        index[key] = std::prev(entries.end());
        retainedBytes += texture->AllocationBytes();
        trim();
        return texture;
    }
    if (source) {
        auto texture = std::make_shared<Texture>(context, source, resource, components);
        timing.Mark("render_texture");
        Entry entry{key, {}, texture, source, source->Generation()};
        entries.push_back(std::move(entry));
        index[key] = std::prev(entries.end());
        retainedBytes += texture->AllocationBytes();
        trim();
        return texture;
    }
    const auto bytes = GuestTextureBytes(resource);
    Require(bytes != 0 && bytes <= std::numeric_limits<std::size_t>::max(), "texture cache surface size overflow");
    std::vector<std::byte> snapshot(static_cast<std::size_t>(bytes));
    const auto aliasGeneration = WriteTracker::AliasWriteGeneration();
    {
        const GuestMemory::AccessSite site("cpu_wait_texture_miss");
        GuestMemory::Read(resource.baseAddress, snapshot, 1);
    }
    timing.Mark("miss_read", snapshot.size());
    timing.Mark(missReason, snapshot.size());
    // Debug aid: ANYPS5_TRACE_TEXTURE_MISSES=<n> logs the first n misses.
    static const long traceMisses = [] {
        const char* value = std::getenv("ANYPS5_TRACE_TEXTURE_MISSES");
        return value != nullptr ? std::atol(value) : 0L;
    }();
    static long tracedMisses = 0;
    if (tracedMisses < traceMisses) {
        ++tracedMisses;
        std::size_t sameAddress = 0;
        for (const auto& entry : entries) sameAddress += entry.address == resource.baseAddress ? 1 : 0;
        std::fprintf(stderr, "[texture-miss] %s address=0x%llx bytes=%zu %ux%u mips=%u format=%u dim=%u entries=%zu retained=%llu MiB same-address=%zu words=%08x %08x %08x %08x %08x %08x %08x %08x\n",
            missReason, static_cast<unsigned long long>(resource.baseAddress), snapshot.size(), resource.width, resource.height, resource.mipCount,
            static_cast<unsigned>(resource.format), static_cast<unsigned>(resource.dimension), entries.size(),
            static_cast<unsigned long long>(retainedBytes >> 20u), sameAddress, key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7]);
    }
    auto texture = std::make_shared<Texture>(context, *context.detiler, resource, components, snapshot);
    timing.Mark("miss_create");
    const auto retained = snapshot.size() + texture->AllocationBytes();
    Entry entry{key, std::move(snapshot), texture};
    entry.address = resource.baseAddress;
    entry.bytes = bytes;
    entry.aliasGeneration = aliasGeneration;
    // Written pages already marked before this snapshot would otherwise look like later writes;
    // they are compared once and then cleared by the next soft-dirty clear.
    entries.push_back(std::move(entry));
    index[key] = std::prev(entries.end());
    retainedBytes += retained;
    trim();
    return texture;
}

}
