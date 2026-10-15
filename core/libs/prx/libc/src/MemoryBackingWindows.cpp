#include "prx/libc/include/MemoryBackingPlatform.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <system_error>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Placeholder reservations (Windows 10 1803+) let views be mapped into reserved ranges. Untested:
// this backend is written to the same contract as the Linux one but has not been run.
namespace GuestMemoryBacking::Platform {
namespace {

DWORD nativeProtection(int protection) {
    if ((protection & 4) != 0) return (protection & 2) != 0 ? PAGE_EXECUTE_READWRITE : (protection & 1) != 0 ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if ((protection & 2) != 0) return PAGE_READWRITE;
    return (protection & 1) != 0 ? PAGE_READONLY : PAGE_NOACCESS;
}

void check(bool success, const char* operation) {
    if (!success) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

void* reservePlaceholder(void* address, std::size_t bytes) {
    return VirtualAlloc2(nullptr, address, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
}

// Splits the placeholder so [address, address + bytes) is one placeholder of its own.
void isolatePlaceholder(std::uint64_t address, std::size_t bytes) {
    VirtualFree(reinterpret_cast<void*>(address), bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
}

}

Segment CreateSegment(std::size_t bytes) {
    const auto size = static_cast<std::uint64_t>(bytes);
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, static_cast<DWORD>(size >> 32u), static_cast<DWORD>(size), nullptr);
    check(section != nullptr, "CreateFileMapping guest segment");
    void* alias = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, bytes);
    if (alias == nullptr) {
        CloseHandle(section);
        check(false, "MapViewOfFile guest segment alias");
    }
    return {reinterpret_cast<std::uintptr_t>(section), alias, bytes};
}

void DestroySegment(const Segment& segment) {
    check(UnmapViewOfFile(segment.alias) != FALSE, "UnmapViewOfFile guest segment alias");
    check(CloseHandle(reinterpret_cast<HANDLE>(segment.handle)) != FALSE, "CloseHandle guest segment");
}

std::uint64_t ClaimRange(std::uint64_t address, std::size_t bytes, std::size_t alignment, bool fixed) {
    if (fixed) return reservePlaceholder(reinterpret_cast<void*>(address), bytes) != nullptr ? address : 0;
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.LowestStartingAddress = reinterpret_cast<void*>(address);
    requirements.Alignment = alignment;
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    void* result = VirtualAlloc2(nullptr, nullptr, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &parameter, 1);
    // The address is only a hint (FreeBSD's vm_map_find_min, upstream a016b4f2b): without room at or
    // above it, the range comes from anywhere.
    if (result == nullptr && address != 0) {
        requirements.LowestStartingAddress = nullptr;
        result = VirtualAlloc2(nullptr, nullptr, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &parameter, 1);
    }
    return reinterpret_cast<std::uint64_t>(result);
}

bool RangeFree(std::uint64_t address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory)) return false;
    return memory.State == MEM_FREE && reinterpret_cast<std::uint64_t>(memory.BaseAddress) + memory.RegionSize >= address + bytes;
}

void ReleaseRange(std::uint64_t address, std::size_t bytes) {
    isolatePlaceholder(address, bytes);
    check(VirtualFree(reinterpret_cast<void*>(address), 0, MEM_RELEASE) != FALSE, "VirtualFree guest range");
}

void MapView(std::uint64_t address, std::size_t bytes, const Segment& segment, std::uint64_t offset, int protection) {
    isolatePlaceholder(address, bytes);
    void* view = MapViewOfFile3(reinterpret_cast<HANDLE>(segment.handle), nullptr, reinterpret_cast<void*>(address), offset, bytes, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0);
    check(view != nullptr, "MapViewOfFile3 guest view");
    VirtualAlloc(view, bytes, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    Protect(address, bytes, protection);
}

void UnmapView(std::uint64_t address, std::size_t bytes) {
    static_cast<void>(bytes);
    check(UnmapViewOfFile2(GetCurrentProcess(), reinterpret_cast<void*>(address), MEM_PRESERVE_PLACEHOLDER) != FALSE, "UnmapViewOfFile2 guest view");
}

Segment AdoptSegment(std::uintptr_t object, std::size_t bytes) {
    HANDLE duplicate = nullptr;
    check(DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(object), GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE, "DuplicateHandle guest object");
    void* alias = MapViewOfFile(duplicate, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, bytes);
    if (alias == nullptr) alias = MapViewOfFile(duplicate, FILE_MAP_READ, 0, 0, bytes);
    if (alias == nullptr) {
        const DWORD error = GetLastError();
        CloseHandle(duplicate);
        SetLastError(error);
        check(false, "MapViewOfFile guest object alias");
    }
    return {reinterpret_cast<std::uintptr_t>(duplicate), alias, bytes};
}

void MapObjectView(std::uint64_t address, std::size_t bytes, const Segment& segment, std::uint64_t offset, int protection, bool privateCopy) {
    if (privateCopy) throw std::system_error(ERROR_NOT_SUPPORTED, std::system_category(), "private guest object views are not supported on Windows");
    MapView(address, bytes, segment, offset, protection);
}

// A SEC_RESERVE section cannot give pages back while it exists: the committed pages are zeroed
// through the alias (uncommitted ones already read as zero once committed).
void DiscardSegment(const Segment& segment, std::uint64_t offset, std::size_t bytes) {
    if (bytes == 0) return;
    if (offset > segment.bytes || bytes > segment.bytes - offset) throw std::out_of_range("guest segment discard exceeds its segment");
    auto* cursor = static_cast<unsigned char*>(segment.alias) + offset;
    auto* const end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        check(VirtualQuery(cursor, &memory, sizeof(memory)) == sizeof(memory), "VirtualQuery guest segment alias");
        auto* const regionEnd = std::min(end, static_cast<unsigned char*>(memory.BaseAddress) + memory.RegionSize);
        if (memory.State == MEM_COMMIT) std::fill(cursor, regionEnd, static_cast<unsigned char>(0));
        cursor = regionEnd;
    }
}

bool WriteWatchAvailable() {
    return false;
}

bool WatchHost(std::uint64_t, std::size_t) {
    return false;
}

bool CollectWrites(std::uint64_t, std::size_t, WrittenRangeVisitor, void*) {
    return false;
}

void Protect(std::uint64_t address, std::size_t bytes, int protection) {
    DWORD previous = 0;
    check(VirtualProtect(reinterpret_cast<void*>(address), bytes, nativeProtection(protection), &previous) != FALSE, "VirtualProtect guest range");
}

}
