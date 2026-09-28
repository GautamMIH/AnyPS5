#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_WRITETRACKER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_WRITETRACKER_HPP

#include <cstdint>
#include <functional>

// Which guest memory has been written since a point in time, so cached copies of guest memory
// (textures) are compared only when their pages may have changed.
//
// CPU writes, from guest code or from the kernel (read(2) into a buffer), are seen through the
// kernel's soft-dirty page bits of the guest mappings (/proc/self/pagemap, cleared through
// /proc/self/clear_refs). Writes that bypass the guest mappings are reported explicitly: GPU writes
// through imported memory, which may land any time after they are recorded (sticky), and the
// driver's own writes through the host alias of guest memory.
namespace AgcDriver::WriteTracker {

// Whether soft-dirty tracking works on this system; when it does not, every query answers "written".
bool Available();
// Number of clears so far; bits reported by Written refer to writes since the latest clear.
std::uint64_t Clears();
// Whether any page of the range was written by the CPU since the latest clear (or is unknown).
bool CpuWritten(std::uint64_t address, std::uint64_t bytes);
// Clears the soft-dirty bits of the whole process. Listeners run first, so users of the bits can
// record which of their ranges were written before the record is lost.
void Clear();
void AddClearListener(const void* owner, std::function<void()> listener);
void RemoveClearListener(const void* owner);

// The GPU may write the range (imported guest memory bound writable); such ranges stay "written".
void NoteGpuWrite(std::uint64_t address, std::uint64_t bytes);
bool GpuWritten(std::uint64_t address, std::uint64_t bytes);
// Changes whenever a GPU-written range is added.
std::uint64_t GpuWriteGeneration();

// The driver wrote the range through the host alias of guest memory (not seen by soft-dirty bits).
void NoteAliasWrite(std::uint64_t address, std::uint64_t bytes);
// Changes whenever an alias write is noted; AliasWrittenSince reports whether one overlaps the range.
std::uint64_t AliasWriteGeneration();
bool AliasWrittenSince(std::uint64_t address, std::uint64_t bytes, std::uint64_t generation);

}

#endif
