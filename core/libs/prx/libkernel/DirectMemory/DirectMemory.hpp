#ifndef CORE_LIBS_PRX_LIBKERNEL_DIRECTMEMORY_DIRECTMEMORY_HPP
#define CORE_LIBS_PRX_LIBKERNEL_DIRECTMEMORY_DIRECTMEMORY_HPP

#include <cstdint>
#include <cstddef>
#include <vector>

#include "prx/libkernel/KernelErrors.hpp"

static constexpr size_t DIRECT_MEMORY_SIZE = 13824ULL * 1024 * 1024;
static constexpr size_t PS5_PAGE_SIZE = 0x4000;

#ifdef _WIN32
using HostSharedMemory = void*;
#else
using HostSharedMemory = int;
#endif

int DirectMemoryAlloc(int64_t searchStart, int64_t searchEnd, size_t len, size_t alignment, int memoryType, int64_t* physOut);
void DirectMemoryFree(int64_t start, size_t len);
bool DirectMemoryCheckedFree(int64_t start, size_t len);
// Whether every page of [start, start + len) is allocated direct memory.
bool DirectMemoryAllocated(int64_t start, size_t len);
// Hooks the physical pool calls when it allocates and releases direct memory. Guest views of direct
// memory live in GuestMemoryBacking here, so they have nothing to do (DoReleaseDirect unmaps views).
void CreateDirectMemoryBacking(int64_t start, size_t len, int memoryType);
void ForgetDirectMemory(int64_t start, size_t len);
bool DirectMemoryFind(int64_t offset, bool findNext, int64_t* start, int64_t* end, int* memoryType);
void DirectMemoryRetype(int64_t start, size_t len, int memoryType);
bool DirectMemoryLargestFree(int64_t searchStart, int64_t searchEnd, size_t alignment, int64_t* startOut, size_t* sizeOut);
size_t DirectMemoryFreeRun(uint64_t offset, uint64_t limit);
int DoMapDirect(void** addr, size_t len, int prot, int flags, int64_t physStart, size_t alignment);
int DoMapAnon(void** addr, size_t len, int prot, int flags, size_t alignment = PS5_PAGE_SIZE);
// Maps a host shared memory object (JIT shared memory).
int DoMapShared(void** addr, size_t len, int prot, HostSharedMemory object);
// Maps bytes [offset, offset + len) of a host memory object (Linux: a descriptor; Windows: a file
// mapping handle), shared or as a private copy-on-write view (mmap of shared memory and files).
// maxProt (-1 = any) bounds later protection changes of a shared view.
int DoMapObject(void** addr, size_t len, int prot, int flags, std::uintptr_t object, std::uint64_t offset, bool privateCopy, int maxProt);
int DoMprotect(const void* addr, size_t len, int prot);
int DoMtypeprotect(const void* addr, size_t len, int type, int prot);
int DoMunmap(void* addr, size_t len);
int DoReserveVirtual(void** addr, size_t len, int flags, size_t alignment);
int DoReleaseDirect(int64_t start, size_t len);
// Whether every page of [start, end) belongs to a guest mapping (sceKernelVirtualQuery).
bool GuestRangeMapped(std::uintptr_t start, std::uintptr_t end);
void ForgetJitSharedMemory(int descriptor);
void DuplicateJitSharedMemory(int from, int to);
void GuestPageTableUsage(std::uint64_t granularity, std::uint64_t* cpu_entries, std::uint64_t* gpu_entries);

#endif
