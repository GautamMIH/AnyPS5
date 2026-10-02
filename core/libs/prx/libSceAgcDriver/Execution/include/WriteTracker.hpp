#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_WRITETRACKER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_WRITETRACKER_HPP

#include <cstdint>

// Which guest memory has been written since a point in time, so cached copies of guest memory
// (textures) are compared only when their pages may have changed.
//
// CPU writes, from guest code or from the kernel (read(2) into a buffer), come from libc's write
// watch of the guest mappings. Each collection turns the written pages into generations of 64 KiB
// blocks, shared by every user, so one user's collection hides no write from another. Writes that
// bypass the guest mappings are reported explicitly: GPU writes through imported memory, which may
// land any time after they are recorded (sticky), and the driver's own writes through the host
// alias of guest memory.
namespace AgcDriver::WriteTracker {

// Whether CPU writes are watched; when they are not, CpuWrittenSince always answers "written".
bool Available();
// Collects the CPU writes to the range; CpuWrittenSince(range, result) reports every write that
// lands after this call (and, conservatively, some that landed shortly before).
std::uint64_t CpuMark(std::uint64_t address, std::uint64_t bytes);
bool CpuWrittenSince(std::uint64_t address, std::uint64_t bytes, std::uint64_t generation);
// Begins a new epoch. Within one epoch a range's CPU writes are collected once (a texture sampled by
// many draws of a submission is scanned once): CPU writes racing with the epoch's GPU work are
// unordered on the console too. Ordering points begin epochs: a submission starting, a satisfied
// memory wait, a flip, and every CPU write the driver itself makes to guest memory.
void NextEpoch();
// The current epoch: within it, CpuWrittenSince answers the same for the same arguments.
std::uint64_t Epoch();

// The GPU may write the range (imported guest memory bound writable); such ranges stay "written".
void NoteGpuWrite(std::uint64_t address, std::uint64_t bytes);
bool GpuWritten(std::uint64_t address, std::uint64_t bytes);
// Changes whenever a GPU-written range is added.
std::uint64_t GpuWriteGeneration();

// A listener told of every write the tracker learns of, as it learns of it: CPU writes when a
// collection finds them, GPU and alias writes when they are noted. Copies of guest memory that must
// follow every change (the device-local mirror) invalidate themselves from it. Called with the
// tracker's locks held: the listener must not call back into the tracker.
using WriteListener = void (*)(void* context, std::uint64_t address, std::uint64_t bytes);
// Reports the range's CPU writes to the listener (at most once per epoch). False when the range
// cannot be watched: copies of it cannot learn of CPU writes.
bool CpuCollect(std::uint64_t address, std::uint64_t bytes);
void SetWriteListener(WriteListener listener, void* context);

// The driver wrote the range through the host alias of guest memory (not seen by the write watch).
void NoteAliasWrite(std::uint64_t address, std::uint64_t bytes);
// Changes whenever an alias write is noted; AliasWrittenSince reports whether one overlaps the range.
std::uint64_t AliasWriteGeneration();
bool AliasWrittenSince(std::uint64_t address, std::uint64_t bytes, std::uint64_t generation);

}

#endif
