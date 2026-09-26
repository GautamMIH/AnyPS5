#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cstring>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "DirectMemory.hpp"
#include "prx/libkernel/Pthread/include/PthreadStacks.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <map>
#include <mutex>
#include <string>

namespace VirtualRangeNames {

struct Range {
    std::uint64_t Length;
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

void Set(std::uintptr_t start, std::uint64_t length, const char* name) {
    const std::lock_guard lock(Mutex());
    Ranges()[start] = {length, name};
}

void Get(std::uintptr_t address, char* output, std::size_t outputSize) {
    const std::lock_guard lock(Mutex());
    auto& ranges = Ranges();
    auto it = ranges.upper_bound(address);
    if (it == ranges.begin())
        return;
    --it;
    if (address - it->first >= it->second.Length || outputSize == 0)
        return;
    const std::size_t count = std::min(outputSize - 1, it->second.Name.size());
    std::memcpy(output, it->second.Name.data(), count);
    output[count] = '\0';
}

}

extern "C" {

int APS5_VABI sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len, size_t alignment, int memory_type, int64_t* phys_addr_out) {
 (void)memory_type;
 if (search_start < 0 || search_end <= search_start || len == 0
  || (len & (PS5_PAGE_SIZE - 1)) || !phys_addr_out
  || (alignment != 0 && (alignment & (PS5_PAGE_SIZE - 1))))
  return SCE_KERNEL_ERROR_EINVAL;
 return DirectMemoryAlloc(search_start, search_end, len, alignment, phys_addr_out);
}

int APS5_VABI sceKernelAllocateMainDirectMemory(size_t len, size_t alignment, int memory_type, int64_t* phys_addr_out) {
 return sceKernelAllocateDirectMemory(0, static_cast<int64_t>(DIRECT_MEMORY_SIZE), len, alignment, memory_type, phys_addr_out);
}

int APS5_VABI sceKernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end, size_t alignment, int64_t* phys_addr_out, size_t* size_out) {
 if (!phys_addr_out || !size_out) return SCE_KERNEL_ERROR_EINVAL;
 int64_t tmpPhys = 0;
 int ret = DirectMemoryAlloc(search_start, search_end, PS5_PAGE_SIZE, alignment, &tmpPhys);
 if (ret != 0) { *phys_addr_out = 0; *size_out = 0; return ret; }
 DirectMemoryFree(tmpPhys, PS5_PAGE_SIZE);
 *phys_addr_out = tmpPhys;
 *size_out = static_cast<size_t>(search_end) - static_cast<size_t>(tmpPhys);
 return 0;
}

int APS5_VABI sceKernelDirectMemoryQuery(int64_t offset, int flags, void* info, size_t info_size) {
 (void)flags;
 if (!info || offset < 0) return SCE_KERNEL_ERROR_EINVAL;
 struct DirectMemoryQueryInfo { int64_t start; int64_t end; int memory_type; };
 if (info_size < sizeof(DirectMemoryQueryInfo)) return SCE_KERNEL_ERROR_EINVAL;
 auto* q = static_cast<DirectMemoryQueryInfo*>(info);
 q->start = offset & ~static_cast<int64_t>(PS5_PAGE_SIZE - 1);
 q->end = q->start + PS5_PAGE_SIZE;
 q->memory_type = 3;
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
 return DoMapAnon(addr_in_out, len, prot, flags);
}

int APS5_VABI sceKernelMapNamedDirectMemory(void** addr, size_t len, int prot, int flags, int64_t direct_memory_start, size_t alignment, const char* name) {
 (void)name;
 return DoMapDirect(addr, len, prot, flags, direct_memory_start, alignment);
}

int32_t APS5_VABI sceKernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags, const char* name) {
 (void)name;
 return DoMapAnon(addr_in_out, len, prot, flags);
}

int APS5_VABI sceKernelMprotect(const void* addr, size_t len, int prot) {
 return DoMprotect(addr, len, prot);
}

int APS5_VABI sceKernelMunmap(uint64_t vaddr, size_t len) {
 return DoMunmap(reinterpret_cast<void*>(vaddr), len);
}

int APS5_VABI sceKernelReleaseDirectMemory(int64_t start, size_t len) {
 if (start < 0 || len == 0) return SCE_KERNEL_ERROR_EINVAL;
 DirectMemoryFree(start, len);
 return 0;
}

int APS5_VABI sceKernelReserveVirtualRange(void** addr, size_t len, int flags, size_t alignment) {
 (void)flags;
 return DoReserveVirtual(addr, len, alignment);
}

int APS5_VABI sceKernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info, uint64_t info_size) {
 constexpr int kFindNext = 1;
 constexpr int kProtectionCpuRead = 0x1;
 constexpr int kProtectionCpuWrite = 0x2;
 constexpr int kProtectionGpuRead = 0x10;
 constexpr int kProtectionGpuWrite = 0x20;
 if (!info || info_size < sizeof(VirtualQueryInfo)) return SCE_KERNEL_ERROR_EINVAL;
 GuestAllocations::Range range{};
 {
  GuestAllocations::Mutation mutation;
  if (!mutation.Query(addr, (flags & kFindNext) != 0, &range)) return SCE_KERNEL_ERROR_EACCES;
 }
 memset(info, 0, sizeof(VirtualQueryInfo));
 info->start = range.address;
 info->end = range.address + range.bytes;
 info->protection = (range.readable ? kProtectionCpuRead | kProtectionGpuRead : 0) | (range.writable ? kProtectionCpuWrite | kProtectionGpuWrite : 0);
 info->is_committed = range.readable || range.writable;
 info->is_direct = info->is_committed;
 VirtualRangeNames::Get(range.address, info->name, sizeof(info->name));
 return 0;
}

// ---------------------------------------------------------------------------
// Moved as-is (not yet implemented) from the monolithic libkernel/Export.cpp.
// ---------------------------------------------------------------------------

int APS5_VABI sceKernelCheckedReleaseDirectMemory(int64_t start, size_t len) {
 if (start < 0 || len == 0 || (static_cast<uint64_t>(start) & (PS5_PAGE_SIZE - 1)) != 0 || (len & (PS5_PAGE_SIZE - 1)) != 0) return SCE_KERNEL_ERROR_EINVAL;
 DirectMemoryFree(start, len);
 return 0;
}

int APS5_VABI sceKernelMtypeprotect(const void* addr, size_t len, int type, int prot) {
 (void)addr;
 (void)len;
 (void)type;
 (void)prot;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceKernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot) {
 (void)addr;
 (void)start;
 (void)end;
 (void)prot;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceKernelIsStack(void* addr, void** start, void** end) {
 if (!PthreadStacks::Find(reinterpret_cast<std::uintptr_t>(addr), start, end)) {
  if (start) *start = nullptr;
  if (end) *end = nullptr;
 }
 return 0;
}

int APS5_VABI sceKernelAvailableFlexibleMemorySize(size_t* size) {
 (void)size;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceKernelConfiguredFlexibleMemorySize(size_t* size) {
 (void)size;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceKernelSetVirtualRangeName(const void* addr, uint64_t len, const char* name) {
 if (!addr || len == 0 || !name) return SCE_KERNEL_ERROR_EINVAL;
 VirtualRangeNames::Set(reinterpret_cast<std::uintptr_t>(addr), len, name);
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
 (void)index;
 (void)addr;
 (void)len;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceKernelSetPrtAperture(int index, void* addr, size_t len) {
 (void)index;
 (void)addr;
 (void)len;
 NotImplemented_nid_no_patch(__func__);
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
  const int protection = static_cast<unsigned char>(entry.protection);
  void* address = entry.start;
  switch (entry.operation) {
  case kOperationMapDirect:
   result = DoMapDirect(&address, entry.length, protection, flags, static_cast<int64_t>(entry.offset), 0);
   break;
  case kOperationUnmap:
   result = DoMunmap(address, entry.length);
   break;
  case kOperationProtect:
  case kOperationTypeProtect:
   result = DoMprotect(address, entry.length, protection);
   break;
  case kOperationMapFlexible:
   result = DoMapAnon(&address, entry.length, protection, flags);
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
