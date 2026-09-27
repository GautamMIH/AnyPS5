#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_MEMORYBACKINGPLATFORM_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_MEMORYBACKINGPLATFORM_HPP

#include <cstddef>
#include <cstdint>

// Host primitives for guest virtual memory: address ranges are claimed (reserved, no access) and
// released; segments are shared memory objects with a host alias; views map part of a segment into
// a claimed range.
namespace GuestMemoryBacking::Platform {

struct Segment {
    std::uintptr_t handle;
    void* alias;
    std::size_t bytes;
};

// Zero-filled shared memory, readable and writable through alias.
Segment CreateSegment(std::size_t bytes);
void DestroySegment(const Segment& segment);

// Claims [address, address + bytes) when fixed, otherwise the first free aligned range at or after
// address (anywhere when address is 0). Returns 0 when the range (or no range) is available.
std::uint64_t ClaimRange(std::uint64_t address, std::size_t bytes, std::size_t alignment, bool fixed);
// Whether [address, address + bytes) is unclaimed.
bool RangeFree(std::uint64_t address, std::size_t bytes);
void ReleaseRange(std::uint64_t address, std::size_t bytes);

// Maps segment bytes [offset, offset + bytes) at a claimed address; UnmapView returns the range to
// the claimed, inaccessible state.
void MapView(std::uint64_t address, std::size_t bytes, const Segment& segment, std::uint64_t offset, int protection);
void UnmapView(std::uint64_t address, std::size_t bytes);
void Protect(std::uint64_t address, std::size_t bytes, int protection);

}

#endif
