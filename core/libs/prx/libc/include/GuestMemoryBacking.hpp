#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTMEMORYBACKING_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTMEMORYBACKING_HPP

#include <cstddef>
#include <cstdint>

// Guest virtual memory, following the PS5 model (see docs/research-notes.md): areas are reserved,
// direct (views of one physical memory object, so aliases share contents), flexible or libc heap.
// Every change updates areas, host mappings and the GPU allocation registry together.
namespace GuestMemoryBacking {

enum class Kind : int { Reserved = 0, Direct = 1, Flexible = 2, Heap = 3 };

enum class Status : int { Ok = 0, Invalid = 1, NoMemory = 2, Access = 3 };

// PS5 mapping flags.
constexpr int kMapFixed = 0x10;
constexpr int kMapNoOverwrite = 0x80;
constexpr int kMapNoCoalesce = 0x400000;

// SCE protection bits: CPU read/write/execute, GPU read/write; 0x40-0x200 are other devices.
constexpr int kProtCpuRead = 0x1;
constexpr int kProtCpuWrite = 0x2;
constexpr int kProtCpuExec = 0x4;
constexpr int kProtGpuRead = 0x10;
constexpr int kProtGpuWrite = 0x20;

// Bytes of direct (physical) memory.
constexpr std::uint64_t kPhysicalBytes = 13824ull * 1024 * 1024;
constexpr std::uint64_t kPageBytes = 0x4000;

struct Area {
    std::uint64_t address;
    std::uint64_t bytes;
    Kind kind;
    int protection;
    std::int64_t physical;
    bool noCoalesce;
};

extern "C" {
// Maps (or reserves) guest memory. *address is the fixed address or a search hint (0 = anywhere).
Status GuestVirtualMap_nid_postfix(void** address, std::size_t bytes, std::size_t alignment, Kind kind, int protection, int flags, std::int64_t physical);
// Unmaps every area part in the range; free parts are skipped, reserved parts become free.
Status GuestVirtualUnmap_nid_postfix(void* address, std::size_t bytes);
// Changes protection of the areas in the range (reserved areas only record it). Returns Invalid
// when no area overlaps, so callers can handle memory the guest map does not own.
Status GuestVirtualProtect_nid_postfix(const void* address, std::size_t bytes, int protection);
// Unmaps every direct view of the physical range.
Status GuestVirtualReleasePhysical_nid_postfix(std::int64_t physical, std::size_t bytes);
// The area holding address (or, with findNext, the next one), merged with like neighbours.
bool GuestVirtualQuery_nid_postfix(const void* address, bool findNext, Area* area);

// libc heap chunks.
void* GuestMemoryBackingMap_nid_postfix(void* address, std::size_t bytes, std::size_t alignment, int protection);
void GuestMemoryBackingUnmap_nid_postfix(void* address, std::size_t bytes);
// Checks that a range is committed guest memory, and writes to it through its unprotected alias.
void GuestMemoryBackingRequire_nid_postfix(std::uint64_t address, std::size_t bytes);
void GuestMemoryBackingWrite_nid_postfix(std::uint64_t address, const void* source, std::size_t bytes);
// Changes whenever guest mappings are added, removed or re-protected, so callers can cache
// host mapping queries.
std::uint64_t GuestMemoryBackingGeneration_nid_postfix();
void GuestMemoryBackingNoteChange_nid_postfix();
}

}

#endif
