#include "DirectMemory.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#if defined(__linux__)
#include <dlfcn.h>
#include <sys/mman.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#else
#include <windows.h>
#endif

extern "C" int APS5_VABI sceKernelClearVirtualRangeName(const void* addr, uint64_t len);

// Guest virtual memory lives in GuestMemoryBacking, which follows the PS5 model (see
// docs/research-notes.md); these wrappers translate its status into SCE error codes.
namespace {

using GuestMemoryBacking::Kind;
using GuestMemoryBacking::Status;

// Debug aid: ANYPS5_TRACE_MEMORY=1 logs every guest mapping operation.
// A direct mapping also names its physical offset, so overlapping uses of the same physical memory
// can be found in the log.
void TraceMemory(const char* operation, const void* addr, size_t len, int prot, int flags, int result, int64_t physical) {
    static const bool enabled = std::getenv("ANYPS5_TRACE_MEMORY") != nullptr;
    if (!enabled) return;
    if (physical >= 0) {
        std::fprintf(stderr, "[memory] %s 0x%llx+0x%zx phys=0x%llx prot=0x%x flags=0x%x -> 0x%x\n", operation, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(addr)), len,
            static_cast<unsigned long long>(physical), prot, flags, static_cast<unsigned>(result));
    } else {
        std::fprintf(stderr, "[memory] %s 0x%llx+0x%zx prot=0x%x flags=0x%x -> 0x%x\n", operation, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(addr)), len, prot, flags, static_cast<unsigned>(result));
    }
}

void TraceMemory(const char* operation, const void* addr, size_t len, int prot, int flags, int result) {
    TraceMemory(operation, addr, len, prot, flags, result, -1);
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
    TraceMemory(operation, result == 0 ? *addr : requested, len, prot, flags, result, physical);
    // A new mapping starts unnamed: a name set on an earlier mapping of the same addresses (since
    // unmapped or released) does not carry over and split sceKernelVirtualQuery's extents.
    if (result == 0) sceKernelClearVirtualRangeName(*addr, len);
    return result;
}

// Memory outside the guest map (the main image and other loaded modules) is protected directly;
// the main image's pages are GPU-visible registry ranges.
int ProtectImage(const void* pointer, size_t bytes, int prot) {
    TraceMemory("protect-image", pointer, bytes, prot, 0, 0);
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
        // Only a loaded image (the main executable, a module) is protected here, as on Windows
        // (MEM_IMAGE): host heaps, host stacks and other host mappings are not the guest's.
        Dl_info first{};
        Dl_info last{};
        if (dladdr(pointer, &first) == 0 || dladdr(reinterpret_cast<const void*>(end - 1), &last) == 0 || first.dli_fbase != last.dli_fbase) return SCE_KERNEL_ERROR_EINVAL;
        apply();
        return 0;
    }
#endif
    // The main image is registered without GPU access; the title's own mprotect with GPU bits adds it
    // (upstream c11a9cb59).
    const bool gpu = (prot & (GuestMemoryBacking::kProtGpuRead | GuestMemoryBacking::kProtGpuWrite)) != 0;
    mutation.Protect(pointer, bytes, readable, writable, gpu, apply);
    return 0;
}

}

int DoMapDirect(void** addr, size_t len, int prot, int flags, int64_t physStart, size_t alignment) {
    return Map("map-direct", addr, len, alignment, Kind::Direct, prot, flags, physStart);
}

int DoMapAnon(void** addr, size_t len, int prot, int flags, size_t alignment) {
    return Map("map-flexible", addr, len, alignment, Kind::Flexible, prot, flags, -1);
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
    // The guard page below a guest thread's stack belongs to the host (as upstream DoMprotect):
    // the guest cannot change it.
    if (GuestThreadStackHostRangeOverlaps(address, address + len)) {
        TraceMemory("protect-stack-guard", addr, len, prot, 0, SCE_KERNEL_ERROR_EACCES);
        return SCE_KERNEL_ERROR_EACCES;
    }
    auto first = address & ~pageMask;
    auto end = (address + len + pageMask) & ~pageMask;
    // A range inside a thread stack only changes the stack's own pages: widening it to whole pages
    // never reaches past the stack (upstream DoMprotect).
    std::uintptr_t stackStart = 0;
    std::uintptr_t stackEnd = 0;
    bool onStack = false;
#if defined(__linux__)
    onStack = (prot & (GuestMemoryBacking::kProtGpuRead | GuestMemoryBacking::kProtGpuWrite)) == 0 && GuestThreadStack(address, &stackStart, &stackEnd) && address >= stackStart && address + len <= stackEnd;
#endif
    if (onStack) {
        first = (std::max)(first, (stackStart + pageMask) & ~pageMask);
        end = (std::min)(end, stackEnd & ~pageMask);
    }
    if (onStack && first >= end) return 0;
    const auto bytes = static_cast<std::size_t>(end - first);
    const auto* pointer = reinterpret_cast<const void*>(first);
    const auto status = GuestMemoryBacking::GuestVirtualProtect_nid_postfix(pointer, bytes, prot);
    if (status == Status::Invalid && onStack) {
        // A host thread stack (the main thread, adopted threads) is outside the guest map: its host
        // pages inside the request are protected directly and made writable again when the thread
        // finishes (NoteGuestStackProtection).
        constexpr std::uintptr_t hostPageMask = 0xfff;
        const auto inside = (std::max)(address & ~hostPageMask, (stackStart + hostPageMask) & ~hostPageMask);
        const auto insideEnd = (std::min)((address + len + hostPageMask) & ~hostPageMask, stackEnd & ~hostPageMask);
        if (inside >= insideEnd) return 0;
        const bool writable = (prot & GuestMemoryBacking::kProtCpuWrite) != 0;
        const bool readable = writable || (prot & GuestMemoryBacking::kProtCpuRead) != 0;
#ifdef _WIN32
        const DWORD native = (prot & 4) != 0 ? (writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : writable ? PAGE_READWRITE : readable ? PAGE_READONLY : PAGE_NOACCESS;
        DWORD previous = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(inside), static_cast<std::size_t>(insideEnd - inside), native, &previous)) return SCE_KERNEL_ERROR_EINVAL;
#else
        const int native = (readable ? PROT_READ : 0) | (writable ? PROT_WRITE : 0) | ((prot & 4) != 0 ? PROT_READ | PROT_EXEC : 0);
        if (mprotect(reinterpret_cast<void*>(inside), static_cast<std::size_t>(insideEnd - inside), native) != 0) return SCE_KERNEL_ERROR_EINVAL;
#endif
        NoteGuestStackProtection(inside, static_cast<std::size_t>(insideEnd - inside), readable && writable);
        TraceMemory("protect-host-stack", reinterpret_cast<const void*>(inside), static_cast<std::size_t>(insideEnd - inside), prot, 0, 0);
        return 0;
    }
    // A range in the guest map that GuestMemoryBacking refused (its first page is unmapped) is EINVAL
    // and changes nothing (upstream 05ffb4516); only a range outside the guest map can be an image.
    GuestMemoryBacking::Area guest{};
    const bool inGuestMap = status == Status::Invalid && GuestMemoryBacking::GuestVirtualQuery_nid_postfix(pointer, true, &guest) && guest.address < end;
    const auto result = inGuestMap ? SCE_KERNEL_ERROR_EINVAL : status == Status::Invalid ? ProtectImage(pointer, bytes, prot) : SceResult(status);
    TraceMemory("protect", pointer, bytes, prot, 0, result);
    return result;
}

// The memory type belongs to the physical memory: it changes for the direct memory behind the
// (page-widened) range, after its protection changes. Flexible memory has no type to change.
int DoMtypeprotect(const void* addr, size_t len, int type, int prot) {
    const int result = DoMprotect(addr, len, prot);
    if (result != 0) return result;
    const auto address = reinterpret_cast<std::uintptr_t>(addr);
    constexpr auto pageMask = static_cast<std::uintptr_t>(PS5_PAGE_SIZE - 1);
    const auto end = (address + len + pageMask) & ~pageMask;
    auto cursor = address & ~pageMask;
    GuestMemoryBacking::Area area{};
    while (cursor < end && GuestMemoryBacking::GuestVirtualQuery_nid_postfix(reinterpret_cast<const void*>(cursor), true, &area) && area.address < end) {
        const auto areaEnd = area.address + area.bytes;
        if (area.kind == Kind::Direct && area.physical >= 0) {
            const auto low = std::max<std::uint64_t>(area.address, cursor);
            const auto high = std::min<std::uint64_t>(areaEnd, end);
            DirectMemoryRetype(area.physical + static_cast<int64_t>(low - area.address), static_cast<size_t>(high - low), type);
        }
        cursor = std::max<std::uint64_t>(areaEnd, cursor + PS5_PAGE_SIZE);
    }
    TraceMemory("mtypeprotect", reinterpret_cast<const void*>(address & ~pageMask), static_cast<size_t>(end - (address & ~pageMask)), prot, type, 0);
    return 0;
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

// The physical pool's backing hooks (upstream): direct memory is one GuestMemoryBacking segment
// that exists up front, and its views are released with it (DoReleaseDirect), so neither hook has
// anything to create or forget.
void CreateDirectMemoryBacking(int64_t start, size_t len, int memoryType) {
    (void)start;
    (void)len;
    (void)memoryType;
}

void ForgetDirectMemory(int64_t start, size_t len) {
    (void)start;
    (void)len;
}

// JIT shared memory (upstream Jit.cpp): every mapping of the object shows the same bytes.
int DoMapShared(void** addr, size_t len, int prot, HostSharedMemory object) {
#ifdef _WIN32
    const auto handle = reinterpret_cast<std::uintptr_t>(object);
#else
    const auto handle = static_cast<std::uintptr_t>(object);
#endif
    return DoMapObject(addr, len, prot, 0, handle, 0, false, -1);
}

int DoMapObject(void** addr, size_t len, int prot, int flags, std::uintptr_t object, std::uint64_t offset, bool privateCopy, int maxProt) {
    if (addr == nullptr) return SCE_KERNEL_ERROR_EINVAL;
    const void* requested = *addr;
    const auto result = SceResult(GuestMemoryBacking::GuestVirtualMapObject_nid_postfix(addr, len, PS5_PAGE_SIZE, prot, flags, object, offset, privateCopy, maxProt));
    TraceMemory(privateCopy ? "map-object-private" : "map-object", result == 0 ? *addr : requested, len, prot, flags, result);
    if (result == 0) sceKernelClearVirtualRangeName(*addr, len);
    return result;
}

// Page-table entries the committed guest areas need at the given granularity
// (sceKernelGetPageTableStats): CPU entries for CPU-accessible areas, GPU entries for GPU-accessible ones.
void GuestPageTableUsage(std::uint64_t granularity, std::uint64_t* cpuEntries, std::uint64_t* gpuEntries) {
    constexpr int cpuAccess = GuestMemoryBacking::kProtCpuRead | GuestMemoryBacking::kProtCpuWrite | GuestMemoryBacking::kProtCpuExec;
    constexpr int gpuAccess = GuestMemoryBacking::kProtGpuRead | GuestMemoryBacking::kProtGpuWrite;
    std::uint64_t cpu = 0;
    std::uint64_t gpu = 0;
    std::uint64_t cursor = 0;
    GuestMemoryBacking::Area area{};
    while (granularity != 0 && GuestMemoryBacking::GuestVirtualQuery_nid_postfix(reinterpret_cast<const void*>(cursor), true, &area)) {
        const auto end = area.address + area.bytes;
        if (area.bytes == 0 || end <= cursor) break;
        if (area.kind != GuestMemoryBacking::Kind::Reserved) {
            const auto entries = (end - 1) / granularity - area.address / granularity + 1;
            if ((area.protection & cpuAccess) != 0) cpu += entries;
            if ((area.protection & gpuAccess) != 0) gpu += entries;
        }
        cursor = end;
    }
    *cpuEntries = cpu;
    *gpuEntries = gpu;
}
