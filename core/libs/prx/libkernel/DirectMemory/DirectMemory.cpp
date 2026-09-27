#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <system_error>

#if defined(__linux__)
#include <sys/mman.h>
#else
#include <windows.h>
#endif

// Guest virtual memory lives in GuestMemoryBacking, which follows the PS5 model (see
// docs/research-notes.md); these wrappers translate its status into SCE error codes.
namespace {

using GuestMemoryBacking::Kind;
using GuestMemoryBacking::Status;

// Debug aid: ANYPS5_TRACE_MEMORY=1 logs every guest mapping operation.
void TraceMemory(const char* operation, const void* addr, size_t len, int prot, int flags, int result) {
    static const bool enabled = std::getenv("ANYPS5_TRACE_MEMORY") != nullptr;
    if (enabled) std::fprintf(stderr, "[memory] %s 0x%llx+0x%zx prot=0x%x flags=0x%x -> 0x%x\n", operation, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(addr)), len, prot, flags, static_cast<unsigned>(result));
}

int SceResult(Status status) {
    switch (status) {
    case Status::Ok: return 0;
    case Status::NoMemory: return SCE_KERNEL_ERROR_ENOMEM;
    case Status::Access: return SCE_KERNEL_ERROR_EACCES;
    case Status::Invalid: break;
    }
    return SCE_KERNEL_ERROR_EINVAL;
}

int Map(const char* operation, void** addr, size_t len, size_t alignment, Kind kind, int prot, int flags, int64_t physical) {
    if (addr == nullptr) return SCE_KERNEL_ERROR_EINVAL;
    const void* requested = *addr;
    const auto result = SceResult(GuestMemoryBacking::GuestVirtualMap_nid_postfix(addr, len, alignment, kind, prot, flags, physical));
    TraceMemory(operation, result == 0 ? *addr : requested, len, prot, flags, result);
    return result;
}

// Memory outside the guest map (the main image and other loaded modules) is protected directly;
// the main image's pages are GPU-visible registry ranges.
int ProtectImage(const void* pointer, size_t bytes, int prot) {
    const bool writable = (prot & (GuestMemoryBacking::kProtCpuWrite | GuestMemoryBacking::kProtGpuWrite)) != 0;
    const bool readable = writable || (prot & (GuestMemoryBacking::kProtCpuRead | GuestMemoryBacking::kProtGpuRead)) != 0;
#ifdef _WIN32
    const DWORD native = (prot & 4) != 0 ? (writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : writable ? PAGE_READWRITE : readable ? PAGE_READONLY : PAGE_NOACCESS;
    const auto apply = [&] {
        GuestMemoryBacking::GuestMemoryBackingNoteChange_nid_postfix();
        DWORD previous = 0;
        if (!VirtualProtect(const_cast<void*>(pointer), bytes, native, &previous)) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "VirtualProtect failed");
    };
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(pointer, &memory, sizeof(memory)) != sizeof(memory)) return SCE_KERNEL_ERROR_EINVAL;
    if (memory.Type != MEM_IMAGE) return SCE_KERNEL_ERROR_EINVAL;
    GuestAllocations::Mutation mutation;
    if (memory.AllocationBase != GetModuleHandleW(nullptr)) {
        apply();
        return 0;
    }
    mutation.RegisterMainImage();
#else
    const int native = (readable ? PROT_READ : 0) | (writable ? PROT_WRITE : 0) | ((prot & 4) != 0 ? PROT_READ | PROT_EXEC : 0);
    const auto apply = [&] {
        GuestMemoryBacking::GuestMemoryBackingNoteChange_nid_postfix();
        if (mprotect(const_cast<void*>(pointer), bytes, native) != 0) throw std::system_error(errno, std::generic_category(), "mprotect failed");
    };
    GuestAllocations::Mutation mutation;
    mutation.RegisterMainImage();
    GuestAllocations::Range overlapping{};
    const auto end = reinterpret_cast<std::uintptr_t>(pointer) + bytes;
    if (!mutation.Query(pointer, true, &overlapping) || overlapping.address >= end) {
        apply();
        return 0;
    }
#endif
    mutation.Protect(pointer, bytes, readable, writable, apply);
    return 0;
}

}

int DoMapDirect(void** addr, size_t len, int prot, int flags, int64_t physStart, size_t alignment) {
    return Map("map-direct", addr, len, alignment, Kind::Direct, prot, flags, physStart);
}

int DoMapAnon(void** addr, size_t len, int prot, int flags) {
    return Map("map-flexible", addr, len, PS5_PAGE_SIZE, Kind::Flexible, prot, flags, -1);
}

int DoReserveVirtual(void** addr, size_t len, int flags, size_t alignment) {
    return Map("reserve", addr, len, alignment, Kind::Reserved, 0, flags, -1);
}

int DoMprotect(const void* addr, size_t len, int prot) {
    // Like the kernel, the range is widened to whole pages.
    const auto address = reinterpret_cast<std::uintptr_t>(addr);
    constexpr auto pageMask = static_cast<std::uintptr_t>(PS5_PAGE_SIZE - 1);
    const auto limit = std::numeric_limits<std::uintptr_t>::max();
    if (address == 0 || len == 0 || len > limit - address || address + len > limit - pageMask || (prot & ~0x3f7) != 0) return SCE_KERNEL_ERROR_EINVAL;
    const auto first = address & ~pageMask;
    const auto bytes = static_cast<std::size_t>(((address + len + pageMask) & ~pageMask) - first);
    const auto* pointer = reinterpret_cast<const void*>(first);
    const auto status = GuestMemoryBacking::GuestVirtualProtect_nid_postfix(pointer, bytes, prot);
    const auto result = status == Status::Invalid ? ProtectImage(pointer, bytes, prot) : SceResult(status);
    TraceMemory("protect", pointer, bytes, prot, 0, result);
    return result;
}

int DoMunmap(void* addr, size_t len) {
    const auto result = SceResult(GuestMemoryBacking::GuestVirtualUnmap_nid_postfix(addr, len));
    TraceMemory("unmap", addr, len, 0, 0, result);
    return result;
}

int DoReleaseDirect(int64_t start, size_t len) {
    if (start < 0 || len == 0 || (static_cast<std::uint64_t>(start) & (PS5_PAGE_SIZE - 1)) != 0 || (len & (PS5_PAGE_SIZE - 1)) != 0) return SCE_KERNEL_ERROR_EINVAL;
    // Releasing physical memory also unmaps every view of it.
    const auto result = SceResult(GuestMemoryBacking::GuestVirtualReleasePhysical_nid_postfix(start, len));
    if (result == 0) DirectMemoryFree(start, len);
    TraceMemory("release-direct", reinterpret_cast<const void*>(start), len, 0, 0, result);
    return result;
}
