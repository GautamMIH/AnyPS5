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

bool WriteWatchAvailable() {
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
