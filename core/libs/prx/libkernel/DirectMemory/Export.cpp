#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "DirectMemory.hpp"
#include "prx/libkernel/Pthread/include/PthreadStacks.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include "prx/libc/include/GuestMemoryTracking.hpp"
#include <cstdint>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/mman.h>
#include <sys/resource.h>
#endif
#include <limits>
#include <stdexcept>
#include <system_error>
#include <iterator>
#include <map>
#include <mutex>
#include <string>

namespace VirtualRangeNames {

struct Range {
    std::uintptr_t End;
    std::string Name;
};

std::mutex& Mutex() {
    static std::mutex instance;
    return instance;
}

std::map<std::uintptr_t, Range>& Ranges() {
    static std::map<std::uintptr_t, Range> instance;
    return instance;
}

// Removes names from [start, end); a named range that straddles either end keeps its outside part.
void EraseLocked(std::uintptr_t start, std::uintptr_t end) {
    auto& ranges = Ranges();
    auto it = ranges.lower_bound(start);
    if (it != ranges.begin() && std::prev(it)->second.End > start) --it;
    while (it != ranges.end() && it->first < end) {
        const auto rangeStart = it->first;
        const auto range = it->second;
        it = ranges.erase(it);
        if (rangeStart < start) ranges.emplace(rangeStart, Range{start, range.Name});
        if (range.End > end) it = ranges.emplace(end, Range{range.End, range.Name}).first;
    }
}

void Set(std::uintptr_t start, std::uint64_t length, const char* name) {
    const std::lock_guard lock(Mutex());
    EraseLocked(start, start + length);
    // Names hold at most 31 characters, as in SceKernelVirtualQueryInfo.
    Ranges().emplace(start, Range{start + length, std::string(name, strnlen(name, 31))});
}

void Clear(std::uintptr_t start, std::uint64_t length) {
    const std::lock_guard lock(Mutex());
    EraseLocked(start, start + length);
}

// Names the query result at `address` and narrows [start, end) to the named range around it, or to
// the unnamed gap between named ranges: named sub-ranges are separate VirtualQuery entries.
void Apply(std::uintptr_t address, std::uintptr_t& start, std::uintptr_t& end, char* output, std::size_t outputSize) {
    const std::lock_guard lock(Mutex());
    auto& ranges = Ranges();
    auto next = ranges.upper_bound(address);
    if (next != ranges.begin()) {
        const auto containing = std::prev(next);
        if (address < containing->second.End) {
            start = std::max(start, containing->first);
            end = std::min(end, containing->second.End);
            if (outputSize == 0) return;
            const std::size_t count = std::min(outputSize - 1, containing->second.Name.size());
            std::memcpy(output, containing->second.Name.data(), count);
            output[count] = '\0';
            return;
        }
        start = std::max(start, containing->second.End);
    }
    if (next != ranges.end()) end = std::min(end, next->first);
}

}

namespace {

// Flexible memory the title configured; sceKernelAvailableFlexibleMemorySize reports what remains.
// Mappings are not refused past it: titles configure their own size, which is not read yet.
constexpr size_t FLEXIBLE_MEMORY_SIZE = 448ULL * 1024 * 1024;
constexpr int PRT_APERTURE_COUNT = 3;

struct PrtAperture {
    void* address;
    size_t length;
};

std::mutex g_prtLock;
PrtAperture g_prtApertures[PRT_APERTURE_COUNT] = {};

std::mutex g_flexibleLock;
std::map<uintptr_t, size_t> g_flexibleRanges;

size_t _flexibleUsedLocked() {
    size_t used = 0;
    for (const auto& [start, len] : g_flexibleRanges) used += len;
    return used;
}

int _mapFlexible(void** addr, size_t len, int prot, int flags) {
    const int result = DoMapAnon(addr, len, prot, flags);
    if (result == 0) {
        std::lock_guard lock(g_flexibleLock);
        g_flexibleRanges[reinterpret_cast<uintptr_t>(*addr)] = len;
    }
    return result;
}

void _releaseFlexible(uintptr_t start, size_t len) {
    std::lock_guard lock(g_flexibleLock);
    const uintptr_t end = start + len;
    auto it = g_flexibleRanges.upper_bound(start);
    if (it != g_flexibleRanges.begin()) --it;
    while (it != g_flexibleRanges.end() && it->first < end) {
        const uintptr_t rangeStart = it->first;
        const uintptr_t rangeEnd = rangeStart + it->second;
        if (rangeEnd <= start) { ++it; continue; }
        it = g_flexibleRanges.erase(it);
        if (rangeStart < start) g_flexibleRanges[rangeStart] = start - rangeStart;
        if (rangeEnd > end) g_flexibleRanges[end] = rangeEnd - end;
    }
}

}

extern "C" {

int APS5_VABI sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len, size_t alignment, int memory_type, int64_t* phys_addr_out) {
 if (search_start < 0 || search_end <= search_start || len == 0
  || (len & (PS5_PAGE_SIZE - 1)) || !phys_addr_out
  || (alignment != 0 && (alignment & (PS5_PAGE_SIZE - 1))))
  return SCE_KERNEL_ERROR_EINVAL;
 return DirectMemoryAlloc(search_start, search_end, len, alignment, memory_type, phys_addr_out);
}

int APS5_VABI sceKernelAllocateMainDirectMemory(size_t len, size_t alignment, int memory_type, int64_t* phys_addr_out) {
 return sceKernelAllocateDirectMemory(0, static_cast<int64_t>(DIRECT_MEMORY_SIZE), len, alignment, memory_type, phys_addr_out);
}

int APS5_VABI sceKernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end, size_t alignment, int64_t* phys_addr_out, size_t* size_out) {
 if (!phys_addr_out || !size_out) return SCE_KERNEL_ERROR_EINVAL;
 int64_t tmpPhys = 0;
 int ret = DirectMemoryAlloc(search_start, search_end, PS5_PAGE_SIZE, alignment, -1, &tmpPhys);
 if (ret != 0) { *phys_addr_out = 0; *size_out = 0; return ret; }
 DirectMemoryFree(tmpPhys, PS5_PAGE_SIZE);
 *phys_addr_out = tmpPhys;
 *size_out = DirectMemoryFreeRun(static_cast<uint64_t>(tmpPhys), static_cast<uint64_t>(search_end));
 return 0;
}

int APS5_VABI sceKernelDirectMemoryQuery(int64_t offset, int flags, void* info, size_t info_size) {
 constexpr int SCE_KERNEL_DMQ_FIND_NEXT = 1;
 if (!info || offset < 0) return SCE_KERNEL_ERROR_EINVAL;
 struct DirectMemoryQueryInfo { int64_t start; int64_t end; int memory_type; };
 if (info_size < sizeof(DirectMemoryQueryInfo)) return SCE_KERNEL_ERROR_EINVAL;
 auto* q = static_cast<DirectMemoryQueryInfo*>(info);
 if (!DirectMemoryFind(offset, (flags & SCE_KERNEL_DMQ_FIND_NEXT) != 0, &q->start, &q->end, &q->memory_type)) return SCE_KERNEL_ERROR_EACCES;
 return 0;
}

int APS5_VABI sceKernelGetDirectMemoryType(int64_t offset, int* memory_type, int64_t* start, int64_t* end) {
 if (!memory_type || !start || !end) return SCE_KERNEL_ERROR_EINVAL;
 int64_t blockStart = 0;
 int64_t blockEnd = 0;
 int blockType = 0;
 if (!DirectMemoryFind(offset, false, &blockStart, &blockEnd, &blockType)) return SCE_KERNEL_ERROR_ENOENT;
 *memory_type = blockType;
 *start = blockStart;
 *end = blockEnd;
 return 0;
}

size_t APS5_VABI sceKernelGetDirectMemorySize(void) {
 return DIRECT_MEMORY_SIZE;
}

int APS5_VABI sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags, int64_t direct_memory_start, size_t alignment) {
 (void)alignment;
 return DoMapDirect(addr, len, prot, flags, direct_memory_start, alignment);
}

int APS5_VABI sceKernelMapDirectMemory2(void** addr, size_t len, int type, int prot, int flags, int64_t direct_memory_start, size_t alignment) {
 (void)type;
 return DoMapDirect(addr, len, prot, flags, direct_memory_start, alignment);
}

int APS5_VABI sceKernelMapFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags) {
 return _mapFlexible(addr_in_out, len, prot, flags);
}

int APS5_VABI sceKernelSetVirtualRangeName(const void* addr, uint64_t len, const char* name);

int APS5_VABI sceKernelMapNamedDirectMemory(void** addr, size_t len, int prot, int flags, int64_t direct_memory_start, size_t alignment, const char* name) {
 const int result = DoMapDirect(addr, len, prot, flags, direct_memory_start, alignment);
 if (result == 0 && name) sceKernelSetVirtualRangeName(*addr, len, name);
 return result;
}

int32_t APS5_VABI sceKernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags, const char* name) {
 const int result = _mapFlexible(addr_in_out, len, prot, flags);
 if (result == 0 && name) sceKernelSetVirtualRangeName(*addr_in_out, len, name);
 return result;
}

int32_t APS5_VABI sceKernelMapNamedFlexibleMemoryInternal(void** addr_in_out, size_t len, int prot, int flags, const char* name) {
 return sceKernelMapNamedFlexibleMemory(addr_in_out, len, prot, flags, name);
}

int APS5_VABI sceKernelMprotect(const void* addr, size_t len, int prot) {
 return DoMprotect(addr, len, prot);
}

int APS5_VABI sceKernelMunmap(uint64_t vaddr, size_t len) {
 const int result = DoMunmap(reinterpret_cast<void*>(vaddr), len);
 if (result == 0) _releaseFlexible(static_cast<uintptr_t>(vaddr), len);
 return result;
}

int APS5_VABI sceKernelReleaseFlexibleMemory(void* addr, size_t len) {
 return sceKernelMunmap(reinterpret_cast<uint64_t>(addr), len);
}

int APS5_VABI sceKernelReleaseDirectMemory(int64_t start, size_t len) {
 return DoReleaseDirect(start, len);
}

int APS5_VABI sceKernelReserveVirtualRange(void** addr, size_t len, int flags, size_t alignment) {
 return DoReserveVirtual(addr, len, flags, alignment);
}

int APS5_VABI sceKernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info, uint64_t info_size) {
 constexpr int kFindNext = 1;
 constexpr int kProtectionCpuRead = 0x1;
 constexpr int kProtectionCpuWrite = 0x2;
 constexpr int kProtectionGpuRead = 0x10;
 constexpr int kProtectionGpuWrite = 0x20;
 constexpr int kDefaultMemoryType = 3;
 if (!info || info_size < sizeof(VirtualQueryInfo)) return SCE_KERNEL_ERROR_EINVAL;
 const bool findNext = (flags & kFindNext) != 0;
 const auto address = reinterpret_cast<std::uint64_t>(addr);
 GuestMemoryBacking::Area area{};
 const bool guest = GuestMemoryBacking::GuestVirtualQuery_nid_postfix(addr, findNext, &area);
 // Loaded images are not guest areas; the registry describes them.
 GuestAllocations::Range image{};
 bool imageFound = false;
 {
  GuestAllocations::Mutation mutation;
  imageFound = mutation.Query(addr, findNext, &image) && !image.releasable;
 }
 const auto contains = [&](std::uint64_t first, std::uint64_t bytes) { return first <= address && address - first < bytes; };
 const bool useImage = imageFound && (!guest || (!contains(area.address, area.bytes) && (contains(image.address, image.bytes) || image.address < area.address)));
 if (!guest && !useImage) return SCE_KERNEL_ERROR_EACCES;
 memset(info, 0, sizeof(VirtualQueryInfo));
 if (useImage) {
  info->start = image.address;
  info->end = image.address + image.bytes;
  info->protection = (image.readable ? kProtectionCpuRead | kProtectionGpuRead : 0) | (image.writable ? kProtectionCpuWrite | kProtectionGpuWrite : 0);
  info->is_committed = 1;
 } else {
  info->start = area.address;
  info->end = area.address + area.bytes;
  info->protection = area.protection;
  info->is_committed = area.kind != GuestMemoryBacking::Kind::Reserved;
  info->is_direct = area.kind == GuestMemoryBacking::Kind::Direct;
  info->is_flexible = area.kind == GuestMemoryBacking::Kind::Flexible || area.kind == GuestMemoryBacking::Kind::Heap;
  if (info->is_direct) {
   info->offset = static_cast<std::uint64_t>(area.physical);
   // The memory type belongs to the direct memory (sceKernelMtypeprotect retypes it).
   int64_t blockStart = 0;
   int64_t blockEnd = 0;
   int blockType = kDefaultMemoryType;
   info->memory_type = DirectMemoryFind(area.physical, false, &blockStart, &blockEnd, &blockType) ? blockType : kDefaultMemoryType;
  }
 }
 {
  std::uintptr_t start = info->start;
  std::uintptr_t end = info->end;
  VirtualRangeNames::Apply(std::max(reinterpret_cast<std::uintptr_t>(addr), start), start, end, info->name, sizeof(info->name));
  if (info->is_direct) info->offset += start - info->start;
  info->start = start;
  info->end = end;
 }
 return 0;
}

// ---------------------------------------------------------------------------
// Moved as-is (not yet implemented) from the monolithic libkernel/Export.cpp.
// ---------------------------------------------------------------------------

int APS5_VABI sceKernelCheckedReleaseDirectMemory(int64_t start, size_t len) {
 // Releases only when every page of the range is allocated; otherwise nothing is released.
 if (start < 0 || (static_cast<uint64_t>(start) & (PS5_PAGE_SIZE - 1)) != 0 || (len & (PS5_PAGE_SIZE - 1)) != 0) return SCE_KERNEL_ERROR_EINVAL;
 if (len == 0) return 0;
 if (!DirectMemoryAllocated(start, len)) return SCE_KERNEL_ERROR_ENOENT;
 return DoReleaseDirect(start, len);
}

int APS5_VABI sceKernelMtypeprotect(const void* addr, size_t len, int type, int prot) {
 return DoMtypeprotect(addr, len, type, prot);
}

int APS5_VABI sceKernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot) {
 VirtualQueryInfo info{};
 const int result = sceKernelVirtualQuery(addr, 0, &info, sizeof(info));
 if (result != 0) return result;
 if (start) *start = reinterpret_cast<void*>(info.start);
 if (end) *end = reinterpret_cast<void*>(info.end);
 if (prot) *prot = info.protection;
 return 0;
}

namespace {

// Whether the host has accessible memory at the address: memory neither the guest areas nor the
// image registry describe (the title's own static data, host-allocated blocks) is still mapped
// memory. A PROT_NONE reservation counts as unmapped.
bool HostAccessible(const void* addr) {
#ifdef _WIN32
 MEMORY_BASIC_INFORMATION host{};
 return VirtualQuery(addr, &host, sizeof(host)) != 0 && host.State == MEM_COMMIT && (host.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
#else
 const auto address = reinterpret_cast<std::uintptr_t>(addr);
 FILE* maps = std::fopen("/proc/self/maps", "r");
 if (maps == nullptr) return false;
 char line[512];
 bool accessible = false;
 while (std::fgets(line, sizeof(line), maps) != nullptr) {
  unsigned long long begin = 0;
  unsigned long long finish = 0;
  char perms[5] = {};
  if (std::sscanf(line, "%llx-%llx %4s", &begin, &finish, perms) != 3 || address < begin || address >= finish) continue;
  accessible = perms[0] == 'r' || perms[1] == 'w' || perms[2] == 'x';
  break;
 }
 std::fclose(maps);
 return accessible;
#endif
}

}

int APS5_VABI sceKernelIsStack(void* addr, void** start, void** end) {
 if (!PthreadStacks::Find(reinterpret_cast<std::uintptr_t>(addr), start, end)) {
  // An address outside every thread stack is not a stack; one outside mapped memory is an error.
  VirtualQueryInfo info{};
  if (sceKernelVirtualQuery(addr, 0, &info, sizeof(info)) != 0 && !HostAccessible(addr)) return SCE_KERNEL_ERROR_EACCES;
  if (start) *start = nullptr;
  if (end) *end = nullptr;
 }
 return 0;
}

int APS5_VABI sceKernelAvailableFlexibleMemorySize(size_t* size) {
 if (!size) return SCE_KERNEL_ERROR_EINVAL;
 std::lock_guard lock(g_flexibleLock);
 *size = FLEXIBLE_MEMORY_SIZE - std::min(FLEXIBLE_MEMORY_SIZE, _flexibleUsedLocked());
 return 0;
}

int APS5_VABI sceKernelConfiguredFlexibleMemorySize(size_t* size) {
 if (!size) return SCE_KERNEL_ERROR_EINVAL;
 *size = FLEXIBLE_MEMORY_SIZE;
 return 0;
}

int APS5_VABI sceKernelSetVirtualRangeName(const void* addr, uint64_t len, const char* name) {
 const auto start = reinterpret_cast<std::uintptr_t>(addr);
 if (!addr || len == 0 || !name || len > UINTPTR_MAX - start) return SCE_KERNEL_ERROR_EINVAL;
 VirtualRangeNames::Set(start, len, name);
 return 0;
}

int APS5_VABI sceKernelClearVirtualRangeName(const void* addr, uint64_t len) {
 const auto start = reinterpret_cast<std::uintptr_t>(addr);
 if (!addr || len == 0 || len > UINTPTR_MAX - start) return SCE_KERNEL_ERROR_EINVAL;
 VirtualRangeNames::Clear(start, len);
 return 0;
}

int APS5_VABI sceKernelGetPageTableStats(int* cpu_total, int* cpu_available, int* gpu_total, int* gpu_available) {
 (void)cpu_total;
 (void)cpu_available;
 (void)gpu_total;
 (void)gpu_available;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceKernelGetPrtAperture(int index, void** addr, size_t* len) {
 if (index < 0 || index >= PRT_APERTURE_COUNT || !addr || !len) return SCE_KERNEL_ERROR_EINVAL;
 std::lock_guard lock(g_prtLock);
 *addr = g_prtApertures[index].address;
 *len = g_prtApertures[index].length;
 return 0;
}

int APS5_VABI sceKernelSetPrtAperture(int index, void* addr, size_t len) {
 if (index < 0 || index >= PRT_APERTURE_COUNT) return SCE_KERNEL_ERROR_EINVAL;
 std::lock_guard lock(g_prtLock);
 g_prtApertures[index] = {addr, len};
 return 0;
}

int APS5_VABI sceKernelBatchMap2(KernelBatchMapEntry* entries, int num_entries, int* num_entries_out, int flags) {
 constexpr int kOperationMapDirect = 0;
 constexpr int kOperationUnmap = 1;
 constexpr int kOperationProtect = 2;
 constexpr int kOperationMapFlexible = 3;
 constexpr int kOperationTypeProtect = 4;
 if (num_entries < 0 || (num_entries > 0 && !entries)) return SCE_KERNEL_ERROR_EINVAL;
 int processed = 0;
 int result = 0;
 for (; processed < num_entries; ++processed) {
  const auto& entry = entries[processed];
  // A zero length or unknown operation stops the batch at that entry.
  if (entry.length == 0 || entry.operation < kOperationMapDirect || entry.operation > kOperationTypeProtect) {
   result = SCE_KERNEL_ERROR_EINVAL;
   break;
  }
  const int protection = static_cast<unsigned char>(entry.protection);
  void* address = entry.start;
  switch (entry.operation) {
  case kOperationMapDirect:
   result = DoMapDirect(&address, entry.length, protection, flags, static_cast<int64_t>(entry.offset), 0);
   break;
  case kOperationUnmap:
   result = sceKernelMunmap(reinterpret_cast<uint64_t>(address), entry.length);
   break;
  case kOperationProtect:
   result = DoMprotect(address, entry.length, protection);
   break;
  case kOperationTypeProtect:
   result = DoMtypeprotect(address, entry.length, static_cast<unsigned char>(entry.type), protection);
   break;
  case kOperationMapFlexible:
   result = _mapFlexible(&address, entry.length, protection, flags);
   break;
  default:
   result = SCE_KERNEL_ERROR_EINVAL;
   break;
  }
  if (result != 0) break;
 }
 if (num_entries_out) *num_entries_out = processed;
 return result;
}

int APS5_VABI sceKernelBatchMap(KernelBatchMapEntry* entries, int num_entries, int* num_entries_out) {
 constexpr int kMapFixed = 0x10;
 return sceKernelBatchMap2(entries, num_entries, num_entries_out, kMapFixed);
}

}

namespace {

#ifdef _WIN32
std::mutex g_workingSetLock;

bool HostRangeAccessible(std::uintptr_t start, std::uintptr_t end) {
    for (auto cursor = start; cursor < end;) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) != sizeof(region)) return false;
        if (region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
        cursor = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
    }
    return true;
}

int LockHostPages(std::uintptr_t start, std::uintptr_t end) {
    if (!HostRangeAccessible(start, end)) return SCE_KERNEL_ERROR_ENOMEM;
    auto* const address = reinterpret_cast<void*>(start);
    const SIZE_T bytes = end - start;
    std::lock_guard lock(g_workingSetLock);
    if (VirtualLock(address, bytes)) return 0;
    if (GetLastError() != ERROR_WORKING_SET_QUOTA) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "VirtualLock failed");
    SIZE_T minimum = 0;
    SIZE_T maximum = 0;
    DWORD flags = 0;
    if (!GetProcessWorkingSetSizeEx(GetCurrentProcess(), &minimum, &maximum, &flags)) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetProcessWorkingSetSizeEx failed");
    if (bytes > std::numeric_limits<SIZE_T>::max() - maximum) return SCE_KERNEL_ERROR_EAGAIN;
    if (!SetProcessWorkingSetSizeEx(GetCurrentProcess(), minimum + bytes, maximum + bytes, flags)) return SCE_KERNEL_ERROR_EAGAIN;
    if (VirtualLock(address, bytes)) return 0;
    if (GetLastError() != ERROR_WORKING_SET_QUOTA) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "VirtualLock failed");
    return SCE_KERNEL_ERROR_EAGAIN;
}
#else
int LockHostPages(std::uintptr_t start, std::uintptr_t end) {
    auto* const address = reinterpret_cast<void*>(start);
    const std::size_t bytes = end - start;
    if (::mlock(address, bytes) == 0) return 0;
    int error = errno;
    rlimit limit{};
    if ((error == ENOMEM || error == EPERM || error == EAGAIN) && getrlimit(RLIMIT_MEMLOCK, &limit) == 0 && limit.rlim_cur < limit.rlim_max) {
        limit.rlim_cur = limit.rlim_max;
        if (setrlimit(RLIMIT_MEMLOCK, &limit) == 0) {
            if (::mlock(address, bytes) == 0) return 0;
            error = errno;
        }
    }
    switch (error) {
    case ENOMEM: return SCE_KERNEL_ERROR_ENOMEM;
    case EPERM: return SCE_KERNEL_ERROR_EPERM;
    case EAGAIN: return SCE_KERNEL_ERROR_EAGAIN;
    default: throw std::system_error(error, std::generic_category(), "mlock failed");
    }
}
#endif

}

extern "C" {

int APS5_VABI sceKernelMlock_nid_postfix(void* address, std::uint64_t length) {
    constexpr std::uintptr_t pageMask = PS5_PAGE_SIZE - 1;
    const auto first = reinterpret_cast<std::uintptr_t>(address);
    if (length > UINTPTR_MAX - first || first + length > UINTPTR_MAX - pageMask) return SCE_KERNEL_ERROR_EINVAL;
    const auto start = first & ~pageMask;
    const auto end = (first + length + pageMask) & ~pageMask;
    if (start == end) return 0;
    // As for any system call on guest memory: tracked pages are resolved first, so a page a
    // driver watch keeps inaccessible does not read as unmapped.
    GuestMemoryTracking::GuestMemoryTrackingResolve_nid_postfix(start, end - start, false);
    return LockHostPages(start, end);
}

}
