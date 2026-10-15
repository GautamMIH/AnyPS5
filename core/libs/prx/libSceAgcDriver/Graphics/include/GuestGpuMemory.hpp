#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTGPUMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTGPUMEMORY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include "prx/libc/include/GuestAllocations.hpp"
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

class DrawQueue;

class Buffer;

// Guest memory as the GPU sees it, without copies: the host memory behind guest mappings (the
// segments of GuestMemoryBacking, through their always-writable aliases) is imported into Vulkan
// with VK_EXT_external_memory_host, so shaders read and write the same pages as the CPU. Imports are
// made on demand in fixed chunks (drivers pin imported pages) and kept while their segment lives.
class GuestGpuMemory {
public:
    struct View {
        VkBuffer buffer;
        VkDeviceSize offset;
        VkDeviceAddress address;
        // Bytes of the requested range the view covers; less when the guest range continues in
        // another segment (or not in guest memory at all).
        std::uint64_t bytes;
    };

    // Returns null when the device cannot import host memory.
    static std::unique_ptr<GuestGpuMemory> Create(const Context& context);
    ~GuestGpuMemory();
    GuestGpuMemory(const GuestGpuMemory&) = delete;
    GuestGpuMemory& operator=(const GuestGpuMemory&) = delete;

    // The GPU view of guest memory starting at address, or nothing when it is not segment memory.
    // lease: the BeginLease serial of the work that uses the view until it completes; without one
    // the import is kept for good (callers that do not report when the GPU is done with it).
    std::optional<View> Resolve(std::uint64_t address, std::uint64_t bytes, std::uint64_t lease = 0);

    // Fault-driven registered memory. Work using the address table leases all registered guest
    // memory; importing it whole pins every page (Astro's Playroom: 13 GB on a 15 GB machine). When
    // the registered memory exceeds the pinned budget (ANYPS5_GPU_PINNED_BUDGET_MB, default 40% of
    // host RAM), the table maps it in import chunks and marks the chunks not imported Absent
    // (BdaAbi::Absent): shaders reaching them note the entry and read nothing, and the chunks are
    // imported when the work completed (FaultIn), for later work. Imports past the budget are
    // released least recently used first: a chunk is Tracked for a while (shaders note it when they
    // use it, which keeps it), then left out of new tables, then released once no work that may use
    // it is in flight. ANYPS5_LEASE_ALL_REGISTERED=1 keeps leasing everything; ANYPS5_REGISTERED_FAULT_IN=1
    // forces fault-driven leases whatever the budget.
    struct RegisteredPiece {
        std::uint64_t begin;
        std::uint64_t end;
        // BdaAbi::Absent, BdaAbi::Tracked or 0 (imported).
        std::uint32_t flags;
    };
    // Whether registered memory of this size is leased fault-driven (sticky once chosen).
    bool FaultDriven(std::uint64_t registeredBytes);
    // Appends the pieces of a registered range split at import chunks; false (nothing appended) when
    // the range is not wholly guest segment memory.
    bool RegisteredPieces(std::uint64_t address, std::uint64_t bytes, std::vector<RegisteredPiece>& pieces);
    // Work using leased views registers from before it resolves them until the GPU is done with it.
    std::uint64_t BeginLease();
    void EndLease(std::uint64_t lease) noexcept;
    // After work completed: imports the chunks of a range shaders found Absent; returns whether a
    // chunk was imported or kept (false: nothing to import, or the import failed).
    bool FaultIn(std::uint64_t address, std::uint64_t bytes);
    // A Tracked chunk the work used: kept.
    void Touch(std::uint64_t address);
    void SetPinnedBudget(std::uint64_t bytes);
    std::uint64_t PinnedBytes();
    // A view for GPU reads only, in device-local memory when the mirror applies: pages written since
    // they were last copied (by the CPU, the GPU through Resolve's view, or the driver) are copied
    // from guest memory first, recorded into commands in queue order. Falls back to Resolve's view.
    // The GPU must not write through it.
    // The copies go into the queue's batch outside a render pass (DrawQueue::Begin), which is entered
    // only when a stale page is copied: a read of current pages leaves the open pass alone.
    std::optional<View> ResolveRead(std::uint64_t address, std::uint64_t bytes, DrawQueue* queue);
    // Whether ResolveRead may record copies (ANYPS5_GPU_MIRROR); otherwise it is Resolve.
    bool Mirrors() const { return mirrorEnabled; }
    // A GPU-readable copy of a registered range outside guest segments that stays mapped (the
    // loaded executable, which the GPU reads on the console). Kept between uses: read-only ranges
    // are copied once, writable ones refresh the 64 KiB blocks the write watch saw written (all of
    // them when their pages cannot be watched). The GPU must not write through it.
    std::optional<View> ResolveImage(const std::shared_ptr<const GuestAllocations::Range>& range);
    // Releases the imports of segments that no longer exist; the GPU must not be using them.
    void Collect();

private:
    enum class ChunkState : std::uint8_t { Present, Tracked, Evicting };
    struct Import {
        std::uint64_t end;
        std::uint64_t bytes = 0;
        // Released under the pinned budget (fault-driven chunk imports used only through leases).
        bool evictable = false;
        ChunkState state = ChunkState::Present;
        std::uint32_t lastUseMs = 0;
        std::uint32_t trackedMs = 0;
        // Leases from this serial on do not use it (Evicting).
        std::uint64_t evictLease = 0;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
        // The device-local mirror (made on first read) and which of its pages are current.
        std::shared_ptr<Buffer> mirror;
        std::vector<bool> current;
        // When each page was last written (GuestGpuMemory::nowMs): pages written lately are dynamic.
        std::vector<std::uint32_t> written;
    };
    // Imports by (segment, first segment offset).
    using Key = std::pair<std::uint64_t, std::uint64_t>;

    GuestGpuMemory(const Context& context, std::uint32_t memoryTypeMask);
    static void invalidateWrites(void* self, std::uint64_t address, std::uint64_t bytes);
    void invalidate(std::uint64_t address, std::uint64_t bytes);
    Import& importRange(std::uint64_t segment, void* alias, std::uint64_t segmentBytes, std::uint64_t first, std::uint64_t last);
    void release(Import& import) noexcept;
    // Under the lock: releases, demotes and tracks chunks for the pinned budget (fault-driven only).
    void maintain(std::uint32_t now, bool force);

    Context context;
    std::uint32_t memoryTypeMask;
    std::mutex mutex;
    std::map<Key, Import> imports;
    std::vector<std::pair<std::uint64_t, Import>> retired;
    struct ImageCopy {
        std::shared_ptr<const GuestAllocations::Range> range;
        std::unique_ptr<Buffer> copy;
        bool watched = false;
        std::vector<bool> stale;
    };
    // Image copies by address.
    std::map<std::uint64_t, ImageCopy> imageCopies;
    bool listening = false;
    // Device-local bytes the mirrors may use; past it reads use guest memory in place.
    std::uint64_t mirrorBudget = 0;
    std::uint64_t mirrorBytes = 0;
    bool mirrorEnabled = false;
    // Fault-driven registered memory (see FaultDriven).
    std::uint64_t pinnedBudget = 0;
    std::uint64_t pinnedBytes = 0;
    std::atomic<bool> faultDriven{false};
    std::atomic<bool> faultDrivenReported{false};
    std::atomic<std::uint64_t> pinnedBudgetSeen{0};
    // Changes whenever a chunk is imported, released or changes state (cached pieces are redone).
    std::uint64_t generation = 1;
    std::uint64_t nextLease = 1;
    std::set<std::uint64_t> liveLeases;
    std::uint32_t lastMaintainMs = 0;
    struct CachedPieces {
        std::uint64_t bytes = 0;
        std::uint64_t generation = 0;
        std::uint64_t mappings = 0;
        std::vector<RegisteredPiece> pieces;
    };
    std::map<std::uint64_t, CachedPieces> pieceCache;
};

}

#endif
