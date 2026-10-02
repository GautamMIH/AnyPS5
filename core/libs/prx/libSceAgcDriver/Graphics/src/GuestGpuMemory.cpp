#include "prx/libSceAgcDriver/Graphics/include/GuestGpuMemory.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::Graphics {
namespace {

// Imports cover whole chunks of a segment so neighbouring ranges share them; a range crossing chunk
// boundaries gets one import spanning them (imports may overlap).
constexpr std::uint64_t kChunkBytes = 32ull << 20;
// Mirror pages: the write tracker's block size.
constexpr std::uint64_t kMirrorPageShift = 16;
// Ranges up to this size are read through the mirror.
constexpr std::uint64_t kMirrorMaxRange = 8ull << 20;

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
            for (auto page = from; page <= to && page < import.current.size(); ++page) import.current[page] = false;
        };
        for (auto& [key, import] : imports)
            if (key.first == translation.segment) clear(import, key.second);
        cursor += translation.bytes;
    }
}

std::optional<GuestGpuMemory::View> GuestGpuMemory::ResolveRead(std::uint64_t address, std::uint64_t bytes, VkCommandBuffer commands) {
    // Large ranges (heaps bound whole, of which a draw reads little) are read in place: copying
    // every stale page would move far more than the shader reads.
    if (!mirrorEnabled || commands == VK_NULL_HANDLE || bytes > kMirrorMaxRange) return Resolve(address, bytes);
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
    auto& import = const_cast<Import&>(importRange(translation.segment, translation.segmentAlias, translation.segmentBytes, first, last));
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

std::optional<GuestGpuMemory::View> GuestGpuMemory::Resolve(std::uint64_t address, std::uint64_t bytes) {
    GuestMemoryBacking::Translation translation{};
    if (!GuestMemoryBacking::GuestVirtualTranslate_nid_postfix(address, bytes, &translation)) return std::nullopt;
    const auto begin = translation.offset;
    const auto end = translation.offset + translation.bytes;
    std::lock_guard lock(mutex);
    const auto first = begin & ~(kChunkBytes - 1u);
    const auto last = std::min((end + kChunkBytes - 1u) & ~(kChunkBytes - 1u), translation.segmentBytes);
    const auto& import = importRange(translation.segment, translation.segmentAlias, translation.segmentBytes, first, last);
    return View{import.buffer, begin - first, import.address + (begin - first), translation.bytes};
}

const GuestGpuMemory::Import& GuestGpuMemory::importRange(std::uint64_t segment, void* alias, std::uint64_t segmentBytes, std::uint64_t first, std::uint64_t last) {
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
        release(import);
        throw;
    }
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
    }
    std::erase_if(retired, [this](auto& entry) {
        if (GuestMemoryBacking::GuestSegmentAlive_nid_postfix(entry.first)) return false;
        release(entry.second);
        return true;
    });
}

void GuestGpuMemory::release(Import& import) noexcept {
    if (import.buffer != VK_NULL_HANDLE) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, import.buffer, nullptr);
    if (import.memory != VK_NULL_HANDLE) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, import.memory, nullptr);
    import.buffer = VK_NULL_HANDLE;
    import.memory = VK_NULL_HANDLE;
}

}
