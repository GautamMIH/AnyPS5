#include <cstdlib>
#include <optional>
#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include <stdexcept>
#include <string>
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>

namespace AgcDriver::Graphics {

GuestBufferMemory::GuestBufferMemory(const Context& context) : context(context) {}

bool GuestBufferMemory::WritesOverlap(std::uint64_t address, std::size_t bytes) const {
    const auto overlaps = [&](std::uint64_t begin, std::uint64_t end) { return address < end && begin < address + bytes; };
    return std::any_of(writes.begin(), writes.end(), [&](const auto& range) { return overlaps(range.first, range.second); }) ||
        std::any_of(detached.begin(), detached.end(), [&](const Region& region) { return overlaps(region.begin, region.end); });
}

void GuestBufferMemory::AppendWrites(std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) const {
    ranges.insert(ranges.end(), writes.begin(), writes.end());
    for (const auto& region : detached) ranges.emplace_back(region.begin, region.end);
}

void GuestBufferMemory::AddDetached(std::uint64_t address, std::size_t bytes) {
    validate(address, bytes);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) std::fprintf(stderr, "[AnyPS5] storage buffer view at 0x%llx is misaligned by a non-dword amount; binding a detached copy\n", static_cast<unsigned long long>(address));
    // Detached views are copied (read and written back by the CPU), not used in place.
    GuestMemory::CheckRange(reinterpret_cast<const void*>(address), bytes, 1, true);
    for (auto& region : detached) {
        if (region.begin != address) continue;
        region.end = std::max<std::uint64_t>(region.end, address + bytes);
        return;
    }
    detached.push_back({address, address + bytes, true, {}, nullptr});
}

void GuestBufferMemory::validate(std::uint64_t address, std::size_t bytes) const {
    Require(!uploaded, "guest memory ownership is frozen for GPU execution");
    Require(address != 0 && bytes != 0, "empty guest memory range");
    Require(bytes <= std::numeric_limits<std::uint64_t>::max() - address, "guest memory range overflow");
}

void GuestBufferMemory::AddWritable(std::uint64_t address, std::size_t bytes, std::size_t leading, bool checked) {
    validate(address, bytes);
    Require(leading <= address, "guest memory range underflow");
    // With imported guest memory the GPU writes the range itself (an upload fallback reads it
    // through GuestMemory::Read, which waits as a CPU access).
    std::optional<GuestMemory::GpuAccessScope> gpuAccess;
    if (context.guestGpuMemory != nullptr) gpuAccess.emplace();
    if (!checked) GuestMemory::CheckRange(reinterpret_cast<const void*>(address), bytes, 1, true);
    if (leading != 0) GuestMemory::CheckRange(reinterpret_cast<const void*>(address - leading), leading, 1, false);
    regions.push_back({address - leading, address + bytes, true, {}, nullptr});
    writes.emplace_back(address, address + bytes);
}

void GuestBufferMemory::AddReadOnly(std::uint64_t address, std::size_t bytes, std::size_t leading, bool checked) {
    validate(address, bytes);
    Require(leading <= address, "guest memory range underflow");
    std::optional<GuestMemory::GpuAccessScope> gpuAccess;
    if (context.guestGpuMemory != nullptr) gpuAccess.emplace();
    if (!checked) GuestMemory::CheckRange(reinterpret_cast<const void*>(address - leading), bytes + leading, 1, false);
    else if (leading != 0) GuestMemory::CheckRange(reinterpret_cast<const void*>(address - leading), leading, 1, false);
    regions.push_back({address - leading, address + bytes, false, {}, nullptr, true});
}

void GuestBufferMemory::AddSnapshot(const GuestMemorySnapshot& snapshot) {
    validate(snapshot.address, snapshot.bytes.size());
    // With a mirror the GPU reads memory as it is at execution, like the console; the capture
    // taken for recompilation only names the range.
    if (context.guestGpuMemory != nullptr) {
        regions.push_back({snapshot.address, snapshot.address + snapshot.bytes.size(), false, {}, nullptr, true});
        return;
    }
    for (const auto& region : regions) {
        if (region.begin <= snapshot.address && snapshot.address + snapshot.bytes.size() <= region.end) {
            const auto offset = static_cast<std::size_t>(snapshot.address - region.begin);
            const auto* source = region.writable ? reinterpret_cast<const std::byte*>(snapshot.address) : region.snapshot.data() + offset;
            Require(std::memcmp(source, snapshot.bytes.data(), snapshot.bytes.size()) == 0, "guest snapshot differs from registered memory");
            return;
        }
    }
    regions.push_back({snapshot.address, snapshot.address + snapshot.bytes.size(), false, {snapshot.bytes.begin(), snapshot.bytes.end()}, nullptr});
}

void GuestBufferMemory::Upload(bool addressable) {
    Require(!uploaded, "guest memory was already uploaded");
    uploaded = true;
    std::sort(regions.begin(), regions.end(), [](const Region& left, const Region& right) { return left.begin < right.begin; });
    std::vector<Region> merged;
    for (auto& region : regions) {
        if (!merged.empty() && region.begin < merged.back().end) {
            auto& previous = merged.back();
            const bool captured = !previous.writable && !previous.live && !region.writable && !region.live;
            Require(captured || previous.live || region.live || previous.writable == region.writable, "writable guest memory overlaps an immutable snapshot");
            if (captured) {
                const auto offset = static_cast<std::size_t>(region.begin - previous.begin);
                const auto overlap = static_cast<std::size_t>(std::min(previous.end, region.end) - region.begin);
                Require(std::memcmp(previous.snapshot.data() + offset, region.snapshot.data(), overlap) == 0, "inconsistent overlapping guest snapshots");
                if (region.end > previous.end) previous.snapshot.insert(previous.snapshot.end(), region.snapshot.begin() + overlap, region.snapshot.end());
            } else {
                // Contents are read from guest memory at upload, which covers both.
                previous.image.reset();
                previous.writable = previous.writable || region.writable;
                previous.live = true;
                previous.snapshot.clear();
            }
            previous.end = std::max(previous.end, region.end);
        } else {
            merged.push_back(std::move(region));
        }
    }
    regions = std::move(merged);
    // Storage buffer views must start at multiples of minStorageBufferOffsetAlignment from their
    // buffer, so each buffer starts at an aligned guest address: the region is extended down to
    // it unless that would reach into the previous region (addresses keep a single owner). The
    // padding stays within the region's first page; it is never written back.
    const auto alignment = std::max<std::uint64_t>(context.limits.minStorageBufferOffsetAlignment, 1u);
    std::uint64_t previousEnd = 0;
    for (auto& region : regions) {
        const auto aligned = region.begin & ~(alignment - 1u);
        const auto padding = aligned >= previousEnd ? region.begin - aligned : 0u;
        previousEnd = region.end;
        region.begin -= padding;
        const auto bytes = region.end - region.begin;
        Require(bytes <= std::numeric_limits<std::size_t>::max(), "guest GPU allocation size overflow");
        // Debug aid: APS5_TRACE_BUFFER_VIEWS=1 logs every region a draw or dispatch binds.
        static const bool traceViews = std::getenv("APS5_TRACE_BUFFER_VIEWS") != nullptr;
        if (traceViews) std::fprintf(stderr, "[buffer-view] 0x%llx+0x%llx %s\n", static_cast<unsigned long long>(region.begin), static_cast<unsigned long long>(bytes), region.writable ? "writable" : region.live ? "live" : "snapshot");
        if (context.guestGpuMemory != nullptr && (region.writable || region.live)) {
            // Resident render targets over the range reach guest memory first, as a read would.
            {
                const GuestMemory::GpuAccessScope gpuAccess;
                GuestMemory::CheckRange(reinterpret_cast<const void*>(region.begin), static_cast<std::size_t>(bytes), 1, region.writable);
            }
            // Read-only regions read the device-local mirror (stale pages copied first, in queue order).
            region.view = region.writable || context.drawQueue == nullptr || !context.guestGpuMemory->Mirrors() ? context.guestGpuMemory->Resolve(region.begin, bytes) : context.guestGpuMemory->ResolveRead(region.begin, bytes, context.drawQueue->Begin(context));
            if (!region.view && !region.writable && region.image && region.begin == region.image->address && bytes == region.image->bytes) region.view = context.guestGpuMemory->ResolveImage(region.image);
            if (region.view && region.view->bytes == bytes) {
                // Shaders write writable regions straight into guest memory.
                if (region.writable) WriteTracker::NoteGpuWrite(region.begin, bytes);
                continue;
            }
            if (traceViews) std::fprintf(stderr, "[buffer-view] 0x%llx+0x%llx copied: %s\n", static_cast<unsigned long long>(region.begin), static_cast<unsigned long long>(bytes), region.view ? "translation shorter than the region" : "not segment memory");
            region.view.reset();
        }
        const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | (addressable ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0u);
        region.buffer = std::make_unique<Buffer>(context, static_cast<std::size_t>(bytes), usage);
        if (region.writable || region.live) {
            try {
                GuestMemory::Read(region.begin, region.buffer->Bytes());
            } catch (const std::exception& error) {
                char context[96];
                std::snprintf(context, sizeof(context), " (uploading writable guest region 0x%llx+0x%llx)", static_cast<unsigned long long>(region.begin), static_cast<unsigned long long>(bytes));
                throw std::runtime_error(std::string(error.what()) + context);
            }
        } else {
            std::memset(region.buffer->Bytes().data(), 0, static_cast<std::size_t>(padding));
            std::memcpy(region.buffer->Bytes().data() + padding, region.snapshot.data(), region.snapshot.size());
        }
        region.snapshot.clear();
    }
    for (auto& region : detached) {
        const auto bytes = static_cast<std::size_t>(region.end - region.begin);
        region.buffer = std::make_unique<Buffer>(context, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        GuestMemory::Read(region.begin, region.buffer->Bytes());
        region.snapshot.assign(region.buffer->Bytes().begin(), region.buffer->Bytes().end());
    }
}

VkDescriptorBufferInfo GuestBufferMemory::Descriptor(std::uint64_t address, std::size_t bytes) const {
    Require(uploaded && !committed, "guest GPU memory is not available");
    Require(bytes != 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address, "invalid guest buffer view");
    for (const auto& region : detached) {
        if (region.begin == address && address + bytes <= region.end) {
            Require(bytes <= context.limits.maxStorageBufferRange, "guest buffer view exceeds descriptor range limit");
            return {region.buffer->Handle(), 0, bytes};
        }
    }
    const auto found = std::upper_bound(regions.begin(), regions.end(), address, [](std::uint64_t value, const Region& region) { return value < region.begin; });
    Require(found != regions.begin(), "guest buffer has no GPU owner");
    const auto& region = *std::prev(found);
    Require(address >= region.begin && address + bytes <= region.end && (region.buffer != nullptr || region.view), "guest buffer view exceeds its GPU owner");
    const auto offset = (region.view ? region.view->offset : 0u) + (address - region.begin);
    if (context.limits.minStorageBufferOffsetAlignment == 0 || offset % context.limits.minStorageBufferOffsetAlignment != 0) {
        char message[200];
        std::snprintf(message, sizeof(message), "guest buffer view at 0x%llx (region 0x%llx) violates storage buffer offset alignment %llu", static_cast<unsigned long long>(address), static_cast<unsigned long long>(region.begin), static_cast<unsigned long long>(context.limits.minStorageBufferOffsetAlignment));
        Require(false, message);
    }
    Require(bytes <= context.limits.maxStorageBufferRange, "guest buffer view exceeds descriptor range limit");
    return {region.view ? region.view->buffer : region.buffer->Handle(), offset, bytes};
}

std::vector<ShaderRecompiler::BdaAbi::Range> GuestBufferMemory::AddressRanges() const {
    Require(uploaded && !committed, "guest GPU address ranges are not available");
    std::vector<ShaderRecompiler::BdaAbi::Range> result;
    for (const auto& region : regions) {
        Require(region.buffer != nullptr || region.view, "incomplete guest GPU upload");
        const auto address = region.view ? region.view->address : region.buffer->DeviceAddress();
        Require(region.end - region.begin <= std::numeric_limits<std::uint64_t>::max() - address, "GPU address range overflow");
        // Stores through the table (GPU-selected V#s) land in guest memory only where the range is the
        // allocation itself: a writable one served in place by its import. Copies would drop them, so
        // a store there faults.
        const auto permissions = ShaderRecompiler::BdaAbi::Read | (region.view && region.writable ? ShaderRecompiler::BdaAbi::Write : 0u);
        result.push_back({region.begin, region.end, address, permissions, 0});
    }
    return result;
}

void GuestBufferMemory::WriteBack() {
    Require(uploaded && !committed, "guest memory cannot be committed twice or before upload");
    std::sort(writes.begin(), writes.end());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
    for (const auto& range : writes) {
        if (!merged.empty() && range.first < merged.back().second) merged.back().second = std::max(merged.back().second, range.second);
        else merged.push_back(range);
    }
    std::vector<std::span<const std::byte>> sources;
    for (const auto& [begin, end] : merged) {
        GuestMemory::CheckRange(reinterpret_cast<const void*>(begin), static_cast<std::size_t>(end - begin), 1, true);
        const auto found = std::upper_bound(regions.begin(), regions.end(), begin, [](auto address, const auto& region) { return address < region.begin; });
        Require(found != regions.begin(), "write-back range has no GPU owner");
        const auto& region = *std::prev(found);
        Require((region.buffer != nullptr || region.view) && region.writable && end <= region.end, "write-back range exceeds its GPU owner");
        // Mirrored memory already holds what the GPU wrote.
        if (region.view) {
            sources.emplace_back();
            continue;
        }
        sources.push_back(region.buffer->Bytes().subspan(static_cast<std::size_t>(begin - region.begin), static_cast<std::size_t>(end - begin)));
    }
    for (std::size_t i = 0; i < merged.size(); ++i) {
        if (!sources[i].empty()) GuestMemory::Write(merged[i].first, sources[i]);
    }
    // Detached views write back the byte runs the GPU changed.
    for (auto& region : detached) {
        region.buffer->Invalidate();
        const auto current = region.buffer->Bytes();
        std::size_t index = 0;
        while (index < current.size()) {
            if (current[index] == region.snapshot[index]) {
                ++index;
                continue;
            }
            auto end = index + 1;
            while (end < current.size() && current[end] != region.snapshot[end]) ++end;
            GuestMemory::Write(region.begin + index, current.subspan(index, end - index));
            index = end;
        }
    }
    committed = true;
    lease.clear();
}

}
