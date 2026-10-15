#include <chrono>
#include "prx/libSceAgcDriver/Graphics/include/GuestGpuMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <algorithm>
#include <cstdio>
#include <dlfcn.h>
#include <execinfo.h>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include "BdaAbi.hpp"

namespace AgcDriver::Graphics {
namespace {

// Imports cover whole chunks of a segment so neighbouring ranges share them; a range crossing chunk
// boundaries gets one import spanning them (imports may overlap).
constexpr std::uint64_t kChunkBytes = 32ull << 20;
// Mirror pages: the write tracker's block size.
constexpr std::uint64_t kMirrorPageShift = 16;
// Ranges up to this size are read through the mirror.
constexpr std::uint64_t kMirrorMaxRange = 8ull << 20;
// A range with a page written this recently is read in place: data the game rewrites every frame
// (constants, streamed vertices) would otherwise be copied before every use, each copy behind full
// barriers (Hellboy: GPU 48 -> 70 ms a frame with every stale page copied). Static data (meshes,
// indices) is copied once and read from device memory, as KytyPS5's buffer cache keeps it.
// ANYPS5_MIRROR_DYNAMIC_MS sets the window (0 copies every stale page, as before).
std::uint32_t dynamicWindowMs() {
    static const std::uint32_t window = [] {
        const char* value = std::getenv("ANYPS5_MIRROR_DYNAMIC_MS");
        return value != nullptr ? static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10)) : 500u;
    }();
    return window;
}

std::uint32_t nowMs() {
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}

std::unique_ptr<GuestGpuMemory> GuestGpuMemory::Create(const Context& context) {
    if (!context.externalMemoryHost || !context.bufferDeviceAddress) return nullptr;
    // Debug aid: ANYPS5_GPU_MEMORY_COPY=1 keeps the per-draw copies instead.
    if (std::getenv("ANYPS5_GPU_MEMORY_COPY") != nullptr) return nullptr;
    std::fprintf(stderr, "[AnyPS5] guest memory is shared with the GPU (host memory import)\n");
    return std::unique_ptr<GuestGpuMemory>(new GuestGpuMemory(context, 0));
}

GuestGpuMemory::GuestGpuMemory(const Context& context, std::uint32_t memoryTypeMask) : context(context), memoryTypeMask(memoryTypeMask) {
    // ANYPS5_GPU_MIRROR=1: GPU reads use a device-local mirror refreshed from the write tracker
    // instead of reading guest memory over PCIe. Off by default: on an RTX 3050 laptop the copies
    // cost more than the in-place reads they replace (see docs/research-notes.md).
    mirrorEnabled = WriteTracker::Available() && std::getenv("ANYPS5_GPU_MIRROR") != nullptr;
    if (mirrorEnabled) {
        // A quarter of the largest device-local heap, at most 1 GiB: the rest is the game's.
        const auto& memory = context.memory;
        for (std::uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap)
            if ((memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) mirrorBudget = std::max<std::uint64_t>(mirrorBudget, memory.memoryHeaps[heap].size / 4);
        mirrorBudget = std::min<std::uint64_t>(mirrorBudget, 1ull << 30);
        std::fprintf(stderr, "[AnyPS5] GPU reads of guest memory use a device-local mirror (up to %llu MiB)\n", static_cast<unsigned long long>(mirrorBudget >> 20));
    }
    // Pinned budget for fault-driven registered memory (see FaultDriven).
    if (const char* value = std::getenv("ANYPS5_GPU_PINNED_BUDGET_MB"); value != nullptr) {
        pinnedBudget = static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10)) << 20;
    } else {
        const auto pages = sysconf(_SC_PHYS_PAGES);
        const auto pageBytes = sysconf(_SC_PAGESIZE);
        pinnedBudget = pages > 0 && pageBytes > 0 ? static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageBytes) / 10 * 4 : 4ull << 30;
    }
    // Mirrors and image copies follow every write the tracker learns of.
    listening = WriteTracker::Available();
    if (listening) WriteTracker::SetWriteListener(&GuestGpuMemory::invalidateWrites, this);
}

void GuestGpuMemory::invalidateWrites(void* self, std::uint64_t address, std::uint64_t bytes) {
    static_cast<GuestGpuMemory*>(self)->invalidate(address, bytes);
}

void GuestGpuMemory::invalidate(std::uint64_t address, std::uint64_t bytes) {
    {
        std::lock_guard lock(mutex);
        const auto end = address + bytes;
        for (auto it = imageCopies.begin(); it != imageCopies.end() && it->first < end; ++it) {
            auto& image = it->second;
            const auto imageEnd = it->first + image.range->bytes;
            if (imageEnd <= address) continue;
            const auto from = (std::max(address, it->first) - it->first) >> kMirrorPageShift;
            const auto to = (std::min(end, imageEnd) - it->first - 1) >> kMirrorPageShift;
            for (auto block = from; block <= to && block < image.stale.size(); ++block) image.stale[block] = true;
        }
    }
    if (!mirrorEnabled) return;
    // Every view of the written bytes: a range may span segments, and imports of a chunk may overlap.
    auto cursor = address;
    const auto end = address + bytes;
    while (cursor < end) {
        GuestMemoryBacking::Translation translation{};
        if (!GuestMemoryBacking::GuestVirtualTranslate_nid_postfix(cursor, end - cursor, &translation) || translation.bytes == 0) {
            cursor = (cursor | ((std::uint64_t{1} << kMirrorPageShift) - 1)) + 1;
            continue;
        }
        const auto begin = translation.offset;
        const auto last = translation.offset + translation.bytes;
        std::lock_guard lock(mutex);
        const auto clear = [&](Import& import, std::uint64_t first) {
            if (import.current.empty() || last <= first || begin >= import.end) return;
            const auto from = (std::max(begin, first) - first) >> kMirrorPageShift;
            const auto to = (std::min(last, import.end) - first - 1) >> kMirrorPageShift;
            const auto now = nowMs();
            for (auto page = from; page <= to && page < import.current.size(); ++page) {
                import.current[page] = false;
                if (page < import.written.size()) import.written[page] = now;
            }
        };
        for (auto& [key, import] : imports)
            if (key.first == translation.segment) clear(import, key.second);
        cursor += translation.bytes;
    }
}

std::optional<GuestGpuMemory::View> GuestGpuMemory::ResolveRead(std::uint64_t address, std::uint64_t bytes, DrawQueue* queue) {
    // Large ranges (heaps bound whole, of which a draw reads little) are read in place: copying
    // every stale page would move far more than the shader reads.
    if (!mirrorEnabled || queue == nullptr || bytes > kMirrorMaxRange) return Resolve(address, bytes);
    // Collected before the lock: collecting reports CPU writes to invalidate (which locks).
    if (!WriteTracker::CpuCollect(address, bytes)) {
        static const bool traceMirror = std::getenv("APS5_TRACE_MIRROR") != nullptr;
        if (traceMirror) std::fprintf(stderr, "[mirror] unwatched 0x%llx+0x%llx\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
        return Resolve(address, bytes);
    }
    GuestMemoryBacking::Translation translation{};
    if (!GuestMemoryBacking::GuestVirtualTranslate_nid_postfix(address, bytes, &translation)) return std::nullopt;
    const auto begin = translation.offset;
    const auto end = translation.offset + translation.bytes;
    std::lock_guard lock(mutex);
    const auto first = begin & ~(kChunkBytes - 1u);
    const auto last = std::min((end + kChunkBytes - 1u) & ~(kChunkBytes - 1u), translation.segmentBytes);
    auto& import = importRange(translation.segment, translation.segmentAlias, translation.segmentBytes, first, last);
    // Mirrored imports are kept: the mirror's copies read them in queue order.
    if (import.evictable) {
        import.evictable = false;
        import.state = ChunkState::Present;
        ++generation;
    }
    const auto size = import.end - first;
    if (!import.mirror) {
        if (mirrorBytes + size > mirrorBudget) return View{import.buffer, begin - first, import.address + (begin - first), translation.bytes};
        try {
            import.mirror = std::make_shared<Buffer>(context, static_cast<std::size_t>(size), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        } catch (const std::exception&) {
            // Device memory is exhausted: stop mirroring, read in place.
            mirrorBudget = mirrorBytes;
            return View{import.buffer, begin - first, import.address + (begin - first), translation.bytes};
        }
        mirrorBytes += size;
        import.current.assign(static_cast<std::size_t>((size + (std::uint64_t{1} << kMirrorPageShift) - 1) >> kMirrorPageShift), false);
        import.written.assign(import.current.size(), nowMs() - dynamicWindowMs() - 1u);
    }
    // A range with a stale page written lately is dynamic: read in place.
    if (const auto window = dynamicWindowMs(); window != 0) {
        const auto now = nowMs();
        for (auto page = (begin - first) >> kMirrorPageShift; page <= (end - first - 1) >> kMirrorPageShift; ++page)
            if (!import.current[page] && now - import.written[page] <= window) return View{import.buffer, begin - first, import.address + (begin - first), translation.bytes};
    }
    // Copies the stale pages of the range, as runs.
    std::vector<VkBufferCopy> copies;
    const auto pageBytes = std::uint64_t{1} << kMirrorPageShift;
    for (auto page = (begin - first) >> kMirrorPageShift; page <= (end - first - 1) >> kMirrorPageShift; ++page) {
        if (import.current[page]) continue;
        import.current[page] = true;
        const auto offset = page << kMirrorPageShift;
        const auto length = std::min(pageBytes, size - offset);
        if (!copies.empty() && copies.back().srcOffset + copies.back().size == offset) copies.back().size += length;
        else copies.push_back({offset, offset, length});
    }
    if (!copies.empty()) {
        const auto commands = queue->Begin(context);
        const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        // CPU writes before submission and earlier GPU writes reach the copy; earlier reads of the
        // mirror finish before it is overwritten.
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier(commands, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
        context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, import.buffer, import.mirror->Handle(), static_cast<std::uint32_t>(copies.size()), copies.data());
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
    }
    return View{import.mirror->Handle(), begin - first, import.mirror->DeviceAddress() + (begin - first), translation.bytes};
}

std::optional<GuestGpuMemory::View> GuestGpuMemory::ResolveImage(const std::shared_ptr<const GuestAllocations::Range>& range) {
    // ANYPS5_NO_IMAGE_COPY=1 copies executable ranges for each use instead.
    static const bool disabled = std::getenv("ANYPS5_NO_IMAGE_COPY") != nullptr;
    if (disabled || range == nullptr || range->bytes == 0) return std::nullopt;
    PerformanceTimer timing("Graphics.ImageCopy");
    // Collected before the lock: collecting reports CPU writes to invalidate (which locks).
    if (range->writable) static_cast<void>(WriteTracker::CpuCollect(range->address, range->bytes));
    timing.Mark("collect");
    std::lock_guard lock(mutex);
    auto found = imageCopies.find(range->address);
    if (found == imageCopies.end() || found->second.range != range) {
        // New, or the registry changed the range (protection): replaces every copy it overlaps.
        const auto end = range->address + range->bytes;
        for (auto it = imageCopies.begin(); it != imageCopies.end();) {
            if (it->first < end && it->first + it->second.range->bytes > range->address) it = imageCopies.erase(it);
            else ++it;
        }
        ImageCopy image;
        image.range = range;
        try {
            image.copy = std::make_unique<Buffer>(context, range->bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        } catch (const std::exception&) {
            return std::nullopt;
        }
        image.stale.assign(static_cast<std::size_t>((range->bytes + (std::uint64_t{1} << kMirrorPageShift) - 1) >> kMirrorPageShift), true);
        // Watched from now on: writes after the first copy below are seen by the next collection.
        image.watched = range->writable && listening && GuestMemoryBacking::GuestWriteWatchAddHost_nid_postfix(range->address, range->bytes);
        std::fprintf(stderr, "[AnyPS5] executable range 0x%llx+0x%llx: GPU copy kept%s\n", static_cast<unsigned long long>(range->address), static_cast<unsigned long long>(range->bytes), !range->writable ? " (read-only)" : image.watched ? ", written blocks refreshed" : ", recopied for each use (cannot watch writes)");
        found = imageCopies.insert_or_assign(range->address, std::move(image)).first;
    }
    auto& image = found->second;
    if (range->writable && !image.watched) std::fill(image.stale.begin(), image.stale.end(), true);
    auto* destination = image.copy->Bytes().data();
    const auto* source = reinterpret_cast<const std::byte*>(range->address);
    const auto blockBytes = std::uint64_t{1} << kMirrorPageShift;
    std::uint64_t copied = 0;
    for (std::size_t block = 0; block < image.stale.size(); ++block) {
        if (!image.stale[block]) continue;
        image.stale[block] = false;
        const auto offset = block * blockBytes;
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(blockBytes, range->bytes - offset));
        std::memcpy(destination + offset, source + offset, length);
        copied += length;
    }
    timing.Mark("copy", copied);
    return View{image.copy->Handle(), 0, image.copy->DeviceAddress(), range->bytes};
}

GuestGpuMemory::~GuestGpuMemory() {
    if (listening) WriteTracker::SetWriteListener(nullptr, nullptr);
    for (auto& [key, import] : imports) release(import);
    for (auto& [segment, import] : retired) release(import);
}

std::optional<GuestGpuMemory::View> GuestGpuMemory::Resolve(std::uint64_t address, std::uint64_t bytes, std::uint64_t lease) {
    GuestMemoryBacking::Translation translation{};
    if (!GuestMemoryBacking::GuestVirtualTranslate_nid_postfix(address, bytes, &translation)) return std::nullopt;
    const auto begin = translation.offset;
    const auto end = translation.offset + translation.bytes;
    std::lock_guard lock(mutex);
    const auto first = begin & ~(kChunkBytes - 1u);
    const auto last = std::min((end + kChunkBytes - 1u) & ~(kChunkBytes - 1u), translation.segmentBytes);
    auto& import = importRange(translation.segment, translation.segmentAlias, translation.segmentBytes, first, last);
    if (import.evictable) {
        // Without a lease nothing reports when the GPU is done with the view: kept for good. A lease
        // using a chunk on its way out keeps it.
        if (lease == 0) {
            import.evictable = false;
            import.state = ChunkState::Present;
            ++generation;
        } else if (import.state == ChunkState::Evicting) {
            import.state = ChunkState::Present;
            import.lastUseMs = nowMs();
            ++generation;
        }
    }
    return View{import.buffer, begin - first, import.address + (begin - first), translation.bytes};
}

GuestGpuMemory::Import& GuestGpuMemory::importRange(std::uint64_t segment, void* alias, std::uint64_t segmentBytes, std::uint64_t first, std::uint64_t last) {
    const auto found = imports.find({segment, first});
    if (found != imports.end() && found->second.end >= last) return found->second;
    // A longer range from the same chunk replaces the key's import; the shorter one stays alive, as
    // in-flight GPU work may use it, until its segment is gone.
    if (found != imports.end()) {
        retired.emplace_back(segment, found->second);
        imports.erase(found);
    }
    Import import;
    import.end = last;
    import.bytes = last - first;
    import.lastUseMs = nowMs();
    auto* pointer = static_cast<std::byte*>(alias) + first;
    const auto bytes = last - first;
    try {
        VkMemoryHostPointerPropertiesEXT properties{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        Check(context.Function<PFN_vkGetMemoryHostPointerPropertiesEXT>("vkGetMemoryHostPointerPropertiesEXT")(context.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, pointer, &properties), "vkGetMemoryHostPointerPropertiesEXT");
        VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &bufferInfo, nullptr, &import.buffer), "vkCreateBuffer guest import");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, import.buffer, &requirements);
        VkImportMemoryHostPointerInfoEXT host{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        host.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        host.pHostPointer = pointer;
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &host};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
        allocate.allocationSize = bytes;
        // Coherent memory needs no flushes between CPU writes and GPU reads.
        allocate.memoryTypeIndex = context.MemoryType(properties.memoryTypeBits & requirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocate, nullptr, &import.memory), "vkAllocateMemory guest import");
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, import.buffer, import.memory, 0), "vkBindBufferMemory guest import");
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = import.buffer;
        import.address = context.Function<PFN_vkGetBufferDeviceAddressKHR>("vkGetBufferDeviceAddressKHR")(context.device, &addressInfo);
    } catch (...) {
        std::uint64_t live = 0;
        for (const auto& [key, entry] : imports) live += entry.end - key.second;
        std::fprintf(stderr, "[guest-gpu-memory] import of 0x%llx bytes (segment 0x%llx of 0x%llx, offset 0x%llx) failed; %zu imports cover 0x%llx bytes, %zu retired\n", static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(segment), static_cast<unsigned long long>(segmentBytes), static_cast<unsigned long long>(first), imports.size(), static_cast<unsigned long long>(live), retired.size());
        // Who asked for the range: module+offset per frame, for addr2line.
        void* frames[32];
        const int count = backtrace(frames, 32);
        for (int index = 1; index < count; ++index) {
            Dl_info info{};
            if (dladdr(frames[index], &info) != 0 && info.dli_fname != nullptr) std::fprintf(stderr, "[guest-gpu-memory]   %s+0x%llx\n", info.dli_fname, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frames[index]) - reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
        }
        import.bytes = 0;
        release(import);
        throw;
    }
    pinnedBytes += import.bytes;
    ++generation;
    return imports.insert_or_assign({segment, first}, import).first->second;
}

void GuestGpuMemory::Collect() {
    std::lock_guard lock(mutex);
    for (auto it = imports.begin(); it != imports.end();) {
        if (GuestMemoryBacking::GuestSegmentAlive_nid_postfix(it->first.first)) {
            ++it;
            continue;
        }
        release(it->second);
        it = imports.erase(it);
        ++generation;
    }
    std::erase_if(retired, [this](auto& entry) {
        if (GuestMemoryBacking::GuestSegmentAlive_nid_postfix(entry.first)) return false;
        release(entry.second);
        return true;
    });
}

void GuestGpuMemory::release(Import& import) noexcept {
    if (import.memory != VK_NULL_HANDLE) pinnedBytes -= std::min(pinnedBytes, import.bytes);
    if (import.buffer != VK_NULL_HANDLE) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, import.buffer, nullptr);
    if (import.memory != VK_NULL_HANDLE) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, import.memory, nullptr);
    import.buffer = VK_NULL_HANDLE;
    import.memory = VK_NULL_HANDLE;
}


namespace {

// A Tracked chunk no shader used for this long is left out of new tables.
constexpr std::uint32_t kProbationMs = 1000;
// Budget upkeep runs at most this often (and after every fault-driven import).
constexpr std::uint32_t kMaintainMs = 50;

struct SegmentRun {
    std::uint64_t address;
    GuestMemoryBacking::Translation translation;
};

// The segment runs of a guest range, or false when part of it is not segment memory.
bool TranslateRuns(std::uint64_t address, std::uint64_t bytes, std::vector<SegmentRun>& runs) {
    auto cursor = address;
    const auto end = address + bytes;
    while (cursor < end) {
        GuestMemoryBacking::Translation translation{};
        if (!GuestMemoryBacking::GuestVirtualTranslate_nid_postfix(cursor, end - cursor, &translation) || translation.bytes == 0) return false;
        runs.push_back({cursor, translation});
        cursor += translation.bytes;
    }
    return true;
}

bool TraceRegistered() {
    static const bool trace = std::getenv("APS5_TRACE_REGISTERED_FAULTS") != nullptr;
    return trace;
}

}

bool GuestGpuMemory::FaultDriven(std::uint64_t registeredBytes) {
    static const bool leaseAll = std::getenv("ANYPS5_LEASE_ALL_REGISTERED") != nullptr;
    static const bool forced = std::getenv("ANYPS5_REGISTERED_FAULT_IN") != nullptr;
    if (leaseAll) return false;
    // The common answers without the lock (every address-table draw asks).
    if (faultDriven.load(std::memory_order_relaxed)) return true;
    if (faultDrivenReported.load(std::memory_order_relaxed) && !forced && registeredBytes <= pinnedBudgetSeen.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(mutex);
    pinnedBudgetSeen.store(pinnedBudget, std::memory_order_relaxed);
    if (!faultDriven && (forced || registeredBytes > pinnedBudget)) {
        faultDriven.store(true);
        std::fprintf(stderr, "[AnyPS5] registered guest memory (%llu MiB) is imported for the GPU where shaders reach it (pinned budget %llu MiB)\n", static_cast<unsigned long long>(registeredBytes >> 20), static_cast<unsigned long long>(pinnedBudget >> 20));
    } else if (!faultDriven && !faultDrivenReported) {
        faultDrivenReported.store(true);
        std::fprintf(stderr, "[AnyPS5] registered guest memory (%llu MiB) fits the pinned budget (%llu MiB): leased whole\n", static_cast<unsigned long long>(registeredBytes >> 20), static_cast<unsigned long long>(pinnedBudget >> 20));
    }
    return faultDriven.load();
}

bool GuestGpuMemory::RegisteredPieces(std::uint64_t address, std::uint64_t bytes, std::vector<RegisteredPiece>& pieces) {
    // Mappings change the translation, imports the flags.
    const auto mappings = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    {
        std::lock_guard lock(mutex);
        const auto found = pieceCache.find(address);
        if (found != pieceCache.end() && found->second.bytes == bytes && found->second.generation == generation && found->second.mappings == mappings) {
            pieces.insert(pieces.end(), found->second.pieces.begin(), found->second.pieces.end());
            return !found->second.pieces.empty();
        }
    }
    // Translated outside the lock (the backing has its own).
    std::vector<SegmentRun> runs;
    const bool segment = TranslateRuns(address, bytes, runs);
    std::lock_guard lock(mutex);
    CachedPieces cached;
    cached.bytes = bytes;
    cached.generation = generation;
    cached.mappings = mappings;
    if (segment) {
        for (const auto& [base, translation] : runs) {
            auto offset = translation.offset;
            const auto runEnd = translation.offset + translation.bytes;
            while (offset < runEnd) {
                const auto first = offset & ~(kChunkBytes - 1u);
                const auto stop = std::min(first + kChunkBytes, runEnd);
                const auto last = std::min(first + kChunkBytes, translation.segmentBytes);
                std::uint32_t flags = ShaderRecompiler::BdaAbi::Absent;
                const auto found = imports.find({translation.segment, first});
                if (found != imports.end() && found->second.end >= last) {
                    const auto state = found->second.evictable ? found->second.state : ChunkState::Present;
                    flags = state == ChunkState::Present ? 0u : state == ChunkState::Tracked ? ShaderRecompiler::BdaAbi::Tracked : ShaderRecompiler::BdaAbi::Absent;
                }
                cached.pieces.push_back({base + (offset - translation.offset), base + (stop - translation.offset), flags});
                offset = stop;
            }
        }
    }
    pieces.insert(pieces.end(), cached.pieces.begin(), cached.pieces.end());
    const bool result = !cached.pieces.empty();
    pieceCache.insert_or_assign(address, std::move(cached));
    return result;
}

std::uint64_t GuestGpuMemory::BeginLease() {
    std::lock_guard lock(mutex);
    const auto lease = nextLease++;
    liveLeases.insert(lease);
    maintain(nowMs(), false);
    return lease;
}

void GuestGpuMemory::EndLease(std::uint64_t lease) noexcept {
    std::lock_guard lock(mutex);
    liveLeases.erase(lease);
}

bool GuestGpuMemory::FaultIn(std::uint64_t address, std::uint64_t bytes) {
    std::vector<SegmentRun> runs;
    if (bytes == 0 || !TranslateRuns(address, bytes, runs)) return false;
    std::lock_guard lock(mutex);
    const auto now = nowMs();
    bool changed = false;
    for (const auto& [base, translation] : runs) {
        auto offset = translation.offset;
        const auto runEnd = translation.offset + translation.bytes;
        while (offset < runEnd) {
            const auto first = offset & ~(kChunkBytes - 1u);
            const auto last = std::min(first + kChunkBytes, translation.segmentBytes);
            offset = std::min(first + kChunkBytes, runEnd);
            const auto found = imports.find({translation.segment, first});
            if (found != imports.end() && found->second.end >= last) {
                auto& import = found->second;
                if (import.evictable && import.state != ChunkState::Present) {
                    import.state = ChunkState::Present;
                    import.lastUseMs = now;
                    ++generation;
                    changed = true;
                }
                continue;
            }
            try {
                auto& import = importRange(translation.segment, translation.segmentAlias, translation.segmentBytes, first, last);
                import.evictable = true;
                import.state = ChunkState::Present;
                import.lastUseMs = now;
                changed = true;
                if (TraceRegistered()) std::fprintf(stderr, "[guest-gpu-memory] fault-in: segment 0x%llx offset 0x%llx (guest 0x%llx); %llu MiB pinned\n", static_cast<unsigned long long>(translation.segment), static_cast<unsigned long long>(first), static_cast<unsigned long long>(base + (first > translation.offset ? first - translation.offset : 0)), static_cast<unsigned long long>(pinnedBytes >> 20));
            } catch (const std::exception& error) {
                std::fprintf(stderr, "[guest-gpu-memory] fault-driven import failed (%s); %llu MiB pinned\n", error.what(), static_cast<unsigned long long>(pinnedBytes >> 20));
                maintain(now, true);
                return changed;
            }
        }
    }
    maintain(now, true);
    return changed;
}

void GuestGpuMemory::Touch(std::uint64_t address) {
    GuestMemoryBacking::Translation translation{};
    if (!GuestMemoryBacking::GuestVirtualTranslate_nid_postfix(address, 1, &translation)) return;
    std::lock_guard lock(mutex);
    const auto found = imports.find({translation.segment, translation.offset & ~(kChunkBytes - 1u)});
    if (found == imports.end() || !found->second.evictable || found->second.state != ChunkState::Tracked) return;
    found->second.state = ChunkState::Present;
    found->second.lastUseMs = nowMs();
    ++generation;
}

void GuestGpuMemory::SetPinnedBudget(std::uint64_t bytes) {
    std::lock_guard lock(mutex);
    pinnedBudget = bytes;
    pinnedBudgetSeen.store(0);
}

std::uint64_t GuestGpuMemory::PinnedBytes() {
    std::lock_guard lock(mutex);
    return pinnedBytes;
}

void GuestGpuMemory::maintain(std::uint32_t now, bool force) {
    if (!faultDriven.load(std::memory_order_relaxed) || (!force && now - lastMaintainMs < kMaintainMs)) return;
    lastMaintainMs = now;
    const auto oldestLive = liveLeases.empty() ? nextLease : *liveLeases.begin();
    std::uint64_t leaving = 0;
    for (auto it = imports.begin(); it != imports.end();) {
        auto& import = it->second;
        if (!import.evictable) {
            ++it;
            continue;
        }
        if (import.state == ChunkState::Evicting && import.evictLease <= oldestLive) {
            // No work that may use it is in flight: every lease from evictLease on left it out.
            if (TraceRegistered()) std::fprintf(stderr, "[guest-gpu-memory] released chunk: segment 0x%llx offset 0x%llx\n", static_cast<unsigned long long>(it->first.first), static_cast<unsigned long long>(it->first.second));
            release(import);
            it = imports.erase(it);
            ++generation;
            continue;
        }
        if (import.state == ChunkState::Tracked && now - import.trackedMs > kProbationMs) {
            import.state = ChunkState::Evicting;
            import.evictLease = nextLease;
            ++generation;
        }
        if (import.state != ChunkState::Present) leaving += import.bytes;
        ++it;
    }
    if (pinnedBytes <= pinnedBudget + leaving) return;
    // Least recently used chunks go on probation until the rest fits the budget.
    std::vector<std::pair<std::uint32_t, Import*>> candidates;
    for (auto& [key, import] : imports)
        if (import.evictable && import.state == ChunkState::Present) candidates.emplace_back(now - import.lastUseMs, &import);
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) { return left.first > right.first; });
    for (auto& [age, import] : candidates) {
        if (pinnedBytes <= pinnedBudget + leaving) break;
        import->state = ChunkState::Tracked;
        import->trackedMs = now;
        leaving += import->bytes;
        ++generation;
    }
}

}
