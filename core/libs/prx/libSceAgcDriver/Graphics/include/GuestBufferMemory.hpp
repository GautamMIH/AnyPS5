#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTBUFFERMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTBUFFERMEMORY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestGpuMemory.hpp"
#include "BdaAbi.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <memory>
#include <vector>

namespace AgcDriver::Graphics {

struct GuestMemorySnapshot {
    std::uint64_t address;
    std::span<const std::byte> bytes;
};

// The guest memory one draw or dispatch reaches. Regions the device's GuestGpuMemory covers are
// used in place (GPU writes land in guest memory directly); others are copied into buffers before
// the work and their writes copied back after it.
class GuestBufferMemory {
public:
    explicit GuestBufferMemory(const Context& context);
    // Leases the registered allocations for BDA access; writes false exposes them read-only (no
    // shader of the work stores through the table). With fault-driven registered memory
    // (GuestGpuMemory::FaultDriven) they are leased in import chunks, those not imported Absent.
    void AcquireRegistered(bool writes = true);
    // leading: bytes before address the region also covers for a view's alignment; they are
    // uploaded but never written back.
    // checked: the caller already range-checked [address, address + bytes) as the region needs
    // (writable for AddWritable) under a GPU access scope; only the leading bytes are checked.
    void AddWritable(std::uint64_t address, std::size_t bytes, std::size_t leading = 0, bool checked = false);
    // A range the shader only reads (DescriptorBinding::bufferWritten proves it): read live at
    // execution like a writable one, but never written back, noted as written or resolved as a write.
    void AddReadOnly(std::uint64_t address, std::size_t bytes, std::size_t leading = 0, bool checked = false);
    // A writable view whose base cannot meet the storage buffer offset alignment, even with the
    // misalignment carried to the shader (BufferViewMisalignment covers dword multiples only): it
    // gets its own buffer starting at the base. Views with the same base share it.
    void AddDetached(std::uint64_t address, std::size_t bytes);
    void AddSnapshot(const GuestMemorySnapshot& snapshot);
    void Upload(bool addressable);
    VkDescriptorBufferInfo Descriptor(std::uint64_t address, std::size_t bytes) const;
    std::vector<ShaderRecompiler::BdaAbi::Range> AddressRanges() const;
    void WriteBack();
    bool WritesOverlap(std::uint64_t address, std::size_t bytes) const;
    // Appends every range the work may write, as [begin, end).
    void AppendWrites(std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) const;

private:
    struct Region {
        std::uint64_t begin;
        std::uint64_t end;
        bool writable;
        std::vector<std::byte> snapshot;
        std::unique_ptr<Buffer> buffer;
        // Contents are read at upload rather than captured (registered memory, live mirrors).
        bool live = false;
        std::optional<GuestGpuMemory::View> view;
        // The registered range outside guest segments it is (the loaded executable), whose copy
        // GuestGpuMemory keeps between uses.
        std::shared_ptr<const GuestAllocations::Range> image;
        // Its whole range was checked (with resolves) when it was added, as `writable` says.
        bool checked = false;
        // A piece of fault-driven registered memory: BdaAbi::Absent (not imported; no view) or
        // BdaAbi::Tracked.
        std::uint32_t flags = 0;
    };

    void validate(std::uint64_t address, std::size_t bytes) const;
    std::uint64_t gpuLease();
    // The GuestGpuMemory lease of the work (fault-driven registered memory), ended with it.
    struct LeaseToken {
        GuestGpuMemory* memory = nullptr;
        std::uint64_t serial = 0;
        LeaseToken() = default;
        LeaseToken(const LeaseToken&) = delete;
        LeaseToken& operator=(const LeaseToken&) = delete;
        LeaseToken(LeaseToken&& other) noexcept : memory(other.memory), serial(other.serial) { other.serial = 0; }
        LeaseToken& operator=(LeaseToken&& other) noexcept {
            if (this != &other) {
                if (serial != 0) memory->EndLease(serial);
                memory = other.memory;
                serial = other.serial;
                other.serial = 0;
            }
            return *this;
        }
        ~LeaseToken() {
            if (serial != 0) memory->EndLease(serial);
        }
    };
    Context context;
    LeaseToken gpuLeaseToken;
    bool faultDriven = false;
    GuestAllocations::Lease lease;
    std::vector<Region> regions;
    // Detached views (see AddDetached); snapshot keeps the uploaded bytes, so only the bytes the
    // GPU changed are written back (another binding may have written the same memory).
    std::vector<Region> detached;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> writes;
    bool uploaded = false;
    bool committed = false;
};

}

#endif
