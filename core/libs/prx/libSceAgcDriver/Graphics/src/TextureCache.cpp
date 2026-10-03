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
}

TextureCache::~TextureCache() = default;

void TextureCache::erase(Entries::iterator it) {
    retainedBytes -= it->snapshot.size() + it->texture->AllocationBytes();
    if (it->texture.use_count() == 1 && !it->texture->UploadComplete()) retiring.push_back(std::move(it->texture));
    index.erase(it->descriptor);
    entries.erase(it);
}

void TextureCache::trim() {
    std::erase_if(retiring, [](const std::shared_ptr<Texture>& texture) { return texture->UploadComplete(); });
    std::erase_if(uploading, [](const std::shared_ptr<Texture>& texture) { return texture->ReleaseUpload(); });
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
namespace {
// Debug aid: APS5_TRACE_TEXMEMO=1 counts why tracked validation fails (the texture is then compared
// or recreated) and prints the totals every 2000 failures.
void noteUnchangedFailure(int reason, std::uint64_t bytes) {
    static const bool trace = std::getenv("APS5_TRACE_TEXMEMO") != nullptr;
    if (!trace) return;
    static std::array<std::uint64_t, 5> counts{};
    static std::array<std::uint64_t, 5> totals{};
    ++counts[reason];
    totals[reason] += bytes;
    static std::uint64_t failures = 0;
    if (++failures % 2000 != 0) return;
    static constexpr const char* names[] = {"untracked", "views", "gpu", "alias", "cpu"};
    std::string line;
    for (int i = 0; i < 5; ++i) line += std::string(" ") + names[i] + "=" + std::to_string(counts[i]) + "/" + std::to_string(totals[i] >> 20) + "MiB";
    std::fprintf(stderr, "[texmemo]%s\n", line.c_str());
}
}

bool TextureCache::unchanged(Entry& entry) {
    PerformanceTimer timing("Graphics.TextureMemo");
    if (!WriteTracker::Available()) {
        noteUnchangedFailure(0, entry.bytes);
        return false;
    }
    const auto bytes = entry.bytes;
    // Writes through another mapping of the same memory would not mark these pages.
    const auto memoryGeneration = GuestMemoryBacking::GuestMemoryBackingGeneration_nid_postfix();
    const std::array<std::uint64_t, 4> at{WriteTracker::Epoch(), WriteTracker::GpuWriteGeneration(), WriteTracker::AliasWriteGeneration(), memoryGeneration};
    // Read before any check: a driver write after it is seen by the next validation.
    const auto driverSequence = WriteTracker::DriverWriteSequence();
    timing.Mark("generations");
    if (entry.unchangedAt == at && !WriteTracker::DriverWrittenSince(entry.address, bytes, entry.unchangedDriverSequence)) {
        entry.unchangedDriverSequence = driverSequence;
        timing.Mark("memo_hit");
        return true;
    }
    timing.Mark("memo_miss");
    if (entry.viewGeneration != memoryGeneration) {
        entry.singleView = GuestMemoryBacking::GuestVirtualSingleView_nid_postfix(entry.address, bytes);
        entry.viewGeneration = memoryGeneration;
    }
    if (!entry.singleView) {
        noteUnchangedFailure(1, bytes);
        return false;
    }
    // Written by the GPU since it was validated (shadPS4 keeps a dirty flag per image for this; an
    // "ever written" mark made every texture in such memory compare on each use).
    // ANYPS5_STICKY_GPU_WRITES=1: any GPU write ever made to the range counts (the former rule).
    static const bool sticky = std::getenv("ANYPS5_STICKY_GPU_WRITES") != nullptr;
    if (sticky ? WriteTracker::GpuWritten(entry.address, bytes) : WriteTracker::GpuWrittenSince(entry.address, bytes, entry.gpuSequence)) {
        noteUnchangedFailure(2, bytes);
        return false;
    }
    if (WriteTracker::AliasWrittenSince(entry.address, bytes, entry.aliasGeneration)) {
        noteUnchangedFailure(3, bytes);
        return false;
    }
    if (WriteTracker::CpuWrittenSince(entry.address, bytes, entry.cpuGeneration)) {
        noteUnchangedFailure(4, bytes);
        return false;
    }
    entry.unchangedAt = at;
    entry.unchangedDriverSequence = driverSequence;
    return true;
}

void TextureCache::NoteDrawWrites(std::uint64_t begin, std::uint64_t end) {
    for (auto& entry : entries) {
        if (entry.bytes == 0 || !(entry.address < end && begin < entry.address + entry.bytes)) continue;
        entry.gpuSequence = 0;
        entry.unchangedAt = {};
    }
}

std::shared_ptr<Texture> TextureCache::Get(std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components, bool depthCompare) {
    Require(words.size() == 8, "texture cache descriptor must contain eight DWORDs");
    PerformanceTimer timing("Graphics.TextureCache");
    trim();
    Key key;
    std::copy(words.begin(), words.end(), key.begin());
    key[8] = static_cast<std::uint32_t>(resource.dimension) | (static_cast<std::uint32_t>(resource.viewDimension) << 8u) | (depthCompare ? 1u << 16u : 0u);
    // Comparison sampling needs a depth view: a resident depth plane's own (below) or an uploaded
    // depth-format image, never a copy into a color image.
    auto source = context.renderCache && !depthCompare ? context.renderCache->Find(resource.baseAddress) : nullptr;
    if (source) {
        const auto& color = source->Description();
        const auto compatibleTiling = (color.tileMode == ColorTileMode::RenderTarget && resource.tileMode == TextureTileMode::RenderTarget64KB) || (color.tileMode == ColorTileMode::Linear && resource.tileMode == TextureTileMode::kLinear);
        timing.Mark("find_color");
        if (!compatibleTiling || resource.width != color.extent.width || resource.height != color.extent.height || resource.dimension != TextureDimension::k2D || resource.mipCount != 1 || resource.baseLevel != 0 || resource.baseArray != 0 || IsBlockCompressed(resource.format) || BytesPerElement(resource.format) != color.elementBytes) source.reset();
    }
    // A texture over a resident depth plane in its depth layout is copied from the depth image.
    std::shared_ptr<ResidentDepth> depthSource;
    if (!source && context.renderCache && resource.tileMode == TextureTileMode::Depth64KB) {
        depthSource = context.renderCache->FindDepth(resource.baseAddress);
        timing.Mark("find_depth");
        if (depthSource) {
            const auto& depth = depthSource->Description();
            if (resource.dimension != TextureDimension::k2D || resource.mipCount != 1 || resource.baseLevel != 0 || resource.baseArray != 0 || IsBlockCompressed(resource.format) || resource.width != depth.extent.width || resource.height != depth.extent.height || BytesPerElement(resource.format) != depth.depthElementBytes || depthSource->HostDepthBytes() != depth.depthElementBytes) depthSource.reset();
        }
    }
    // A depth plane sampled as it is (a float view of D32/D16) is read through a view of the
    // resident image, unless this work renders to it.
    // Debug aid: ANYPS5_NO_DIRECT_DEPTH=1 copies every depth texture.
    static const bool copyDepth = std::getenv("ANYPS5_NO_DIRECT_DEPTH") != nullptr;
    const auto directDepth = [&] {
        if (copyDepth || !depthSource || !depthSource->Sampleable() || depthSource->Description().depthAddress == renderedDepth) return false;
        const auto format = ResolveTextureFormat(resource.format);
        const auto depthFormat = depthSource->Description().format;
        return (format == VK_FORMAT_R32_SFLOAT && (depthFormat == VK_FORMAT_D32_SFLOAT || depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT)) || (format == VK_FORMAT_R16_UNORM && depthFormat == VK_FORMAT_D16_UNORM);
    }();
    // Why a lookup misses, for the frame profile: the entry's memory changed, or no entry exists.
    const char* missReason = "miss_absent";
    if (const auto found = index.find(key); found != index.end()) {
        const auto it = found->second;
        missReason = "miss_changed";
        if (source || depthSource || it->generation != 0) {
            const bool sameSource = ((source && it->source.lock() == source) || (depthSource && it->depthSource.lock() == depthSource)) && (!depthSource || it->texture->Direct() == directDepth);
            if (sameSource) {
                // Render targets change every frame: the copy is refreshed in place instead of
                // recreated (allocating, destroying and a new command batch cost ~2 ms each time).
                const auto generation = source ? source->Generation() : depthSource->Generation();
                if (it->generation != generation) {
                    it->texture->Refresh();
                    it->generation = generation;
                    timing.Mark("refresh");
                }
                auto result = it->texture;
                entries.splice(entries.end(), entries, it);
                return result;
            }
            erase(it);
            timing.Mark("stale_erase");
        } else {
            // Resident render targets over the range reach guest memory first (written through the
            // alias, which the write tracker records).
            const GuestMemory::AccessSite site("cpu_wait_texture");
            GuestMemory::CheckRange(reinterpret_cast<const void*>(resource.baseAddress), static_cast<std::size_t>(it->bytes), 1);
            timing.Mark("range_check");
            if (unchanged(*it)) {
                timing.Mark("validate_tracked");
                // Debug aid: APS5_VERIFY_TEXTURE_TRACKING=1 keeps every snapshot and compares it with
                // guest memory whenever write tracking vouches for the entry (a mismatch is a missed write).
                static const bool verify = std::getenv("APS5_VERIFY_TEXTURE_TRACKING") != nullptr;
                if (verify && !it->snapshot.empty()) {
                    static std::uint64_t checks = 0;
                    static std::uint64_t mismatches = 0;
                    ++checks;
                    if (std::memcmp(reinterpret_cast<const void*>(resource.baseAddress), it->snapshot.data(), it->snapshot.size()) != 0) {
                        ++mismatches;
                        std::fprintf(stderr, "[verify-texture] MISSED WRITE at 0x%llx+0x%zx\n", static_cast<unsigned long long>(resource.baseAddress), it->snapshot.size());
                    }
                    if (checks % 2000 == 0) std::fprintf(stderr, "[verify-texture] %llu checks, %llu mismatches\n", static_cast<unsigned long long>(checks), static_cast<unsigned long long>(mismatches));
                }
                // Write tracking vouches for the entry from now on, so it keeps no snapshot.
                if (!it->tracked && !verify) {
                    retainedBytes -= it->snapshot.size();
                    std::vector<std::byte>().swap(it->snapshot);
                    it->tracked = true;
                }
                auto result = it->texture;
                entries.splice(entries.end(), entries, it);
                return result;
            }
            if (it->tracked) {
                // No snapshot to compare with: a possibly written tracked texture is recreated.
                erase(it);
                goto create;
            }
            const auto aliasGeneration = WriteTracker::AliasWriteGeneration();
            const auto gpuSequence = WriteTracker::GpuWriteSequence();
            const auto cpuGeneration = WriteTracker::CpuMark(resource.baseAddress, it->snapshot.size());
            const bool same = std::memcmp(reinterpret_cast<const void*>(resource.baseAddress), it->snapshot.data(), it->snapshot.size()) == 0;
            timing.Mark("validate_compare", it->snapshot.size());
            if (same) {
                it->aliasGeneration = aliasGeneration;
                it->gpuSequence = gpuSequence;
                it->cpuGeneration = cpuGeneration;
                auto result = it->texture;
                entries.splice(entries.end(), entries, it);
                return result;
            }
            erase(it);
        }
    }
create:
    timing.Mark("lookup");
    if (depthCompare && depthSource && !directDepth) {
        throw std::runtime_error("AGC graphics: comparison sampling of a resident depth plane that cannot be viewed directly (being rendered, or ANYPS5_NO_DIRECT_DEPTH) is not implemented");
    }
    if (depthSource) {
        // Why a depth texture is copied rather than viewed (frame profile).
        if (!directDepth) timing.Mark(depthSource->Description().depthAddress == renderedDepth ? "depth_copy_rendered" : !depthSource->Sampleable() ? "depth_copy_unsampleable" : "depth_copy_format");
        auto texture = directDepth ? std::make_shared<Texture>(context, depthSource, components, Texture::DirectView{}) : std::make_shared<Texture>(context, depthSource, resource, components);
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
    const auto gpuSequence = WriteTracker::GpuWriteSequence();
    const auto cpuGeneration = WriteTracker::CpuMark(resource.baseAddress, bytes);
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
    auto texture = std::make_shared<Texture>(context, *context.detiler, resource, components, snapshot, depthCompare);
    uploading.push_back(texture);
    timing.Mark("miss_create");
    const auto retained = snapshot.size() + texture->AllocationBytes();
    Entry entry{key, std::move(snapshot), texture};
    entry.address = resource.baseAddress;
    entry.bytes = bytes;
    entry.aliasGeneration = aliasGeneration;
    entry.gpuSequence = gpuSequence;
    entry.cpuGeneration = cpuGeneration;
    entries.push_back(std::move(entry));
    index[key] = std::prev(entries.end());
    retainedBytes += retained;
    trim();
    return texture;
}

}
