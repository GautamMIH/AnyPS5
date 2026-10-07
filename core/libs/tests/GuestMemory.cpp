#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestHeap.hpp"
#include "SceTypes.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include "prx/libc/include/GuestMemoryTracking.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include "prx/libkernel/KernelErrors.hpp"
#if defined(__linux__)
#include <fstream>
#include <string>
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" {
void* APS5_VABI mmap_nid_postfix(void*, std::size_t, int, int, int, std::int64_t) noexcept;
int APS5_VABI munmap_nid_postfix(void*, std::size_t) noexcept;
int* APS5_VABI __error_nid_postfix();
int APS5_VABI sceKernelAllocateMainDirectMemory(std::size_t, std::size_t, int, std::int64_t*);
int APS5_VABI sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
int APS5_VABI sceKernelMapFlexibleMemory(void**, std::size_t, int, int);
int32_t APS5_VABI sceKernelMapNamedFlexibleMemory(void**, std::size_t, int, int, const char*);
int32_t APS5_VABI sceKernelMapNamedFlexibleMemoryInternal(void**, std::size_t, int, int, const char*);
int APS5_VABI sceKernelAvailableFlexibleMemorySize(std::size_t*);
int APS5_VABI sceKernelSetVirtualRangeName(const void*, std::uint64_t, const char*);
int APS5_VABI sceKernelClearVirtualRangeName(const void*, std::uint64_t);
int APS5_VABI sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t*);
int APS5_VABI sceKernelCheckedReleaseDirectMemory(std::int64_t, std::size_t);
int APS5_VABI sceKernelReserveVirtualRange(void**, std::size_t, int, std::size_t);
int APS5_VABI sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
int APS5_VABI sceKernelMunmap(std::uint64_t, std::size_t);
int APS5_VABI sceKernelMprotect(const void*, std::size_t, int);
int APS5_VABI sceKernelVirtualQuery(const void*, int, VirtualQueryInfo*, std::uint64_t);
int APS5_VABI sceKernelMemoryPoolReserve(void*, std::size_t, std::size_t, int, void**);
int APS5_VABI sceKernelMemoryPoolCommit(void*, std::size_t, int, int, int);
int APS5_VABI sceKernelMemoryPoolDecommit(void*, std::size_t, int);
int APS5_VABI sceKernelMlock_nid_postfix(void*, std::uint64_t);
int APS5_VABI sceKernelGetDirectMemoryType(std::int64_t, int*, std::int64_t*, std::int64_t*);
int APS5_VABI sceKernelReleaseFlexibleMemory(void*, std::size_t);
int APS5_VABI sceKernelMtypeprotect(const void*, std::size_t, int, int);
int APS5_VABI sceKernelBatchMap2(KernelBatchMapEntry*, int, int*, int);
}

static void RequireAt(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "Guest memory check failed at line %d\n", line);
        std::abort();
    }
}
#define Require(condition) RequireAt((condition), __LINE__)

static const char* NameAt(const void* address) {
    static VirtualQueryInfo info;
    Require(sceKernelVirtualQuery(address, 0, &info, sizeof(info)) == 0);
    return info.name;
}

static void CheckNamedAndHintedMappings() {
    constexpr std::size_t length = 0x10000;
    void* first = nullptr;
    Require(sceKernelMapNamedFlexibleMemory(&first, length, 3, 0, "first mapping") == 0);
    Require(std::strcmp(NameAt(first), "first mapping") == 0);
    auto* middle = static_cast<unsigned char*>(first) + 0x4000;
    Require(sceKernelSetVirtualRangeName(middle, 0x4000, "middle") == 0);
    Require(std::strcmp(NameAt(first), "first mapping") == 0);
    Require(std::strcmp(NameAt(middle), "middle") == 0);
    Require(sceKernelClearVirtualRangeName(first, length) == 0);
    Require(NameAt(middle)[0] == '\0');
    Require(sceKernelSetVirtualRangeName(nullptr, length, "x") != 0);
#if defined(__linux__)
    void* hinted = first;
    Require(sceKernelMapFlexibleMemory(&hinted, length, 3, 0) == 0);
    Require(hinted > first && (reinterpret_cast<std::uintptr_t>(hinted) & 0x3fff) == 0);
    // MAP_FIXED | MAP_NO_OVERWRITE over a mapping fails with an error code and leaves the address.
    void* overwrite = first;
    Require(sceKernelMapFlexibleMemory(&overwrite, 0x4000, 3, 0x90) != 0 && overwrite == first);
    Require(sceKernelMunmap(reinterpret_cast<std::uint64_t>(hinted), length) == 0);
#endif
    Require(sceKernelMunmap(reinterpret_cast<std::uint64_t>(first), length) == 0);
}

static void CheckInternalNamedFlexibleMapping() {
    constexpr std::size_t length = 0x10000;
    std::size_t before = 0;
    std::size_t available = 0;
    Require(sceKernelAvailableFlexibleMemorySize(&before) == 0);
    void* mapped = nullptr;
    Require(sceKernelMapNamedFlexibleMemoryInternal(&mapped, length, 3, 0, "internal mapping") == 0 && mapped != nullptr);
    Require(std::strcmp(NameAt(mapped), "internal mapping") == 0);
    Require(sceKernelAvailableFlexibleMemorySize(&available) == 0 && available == before - length);
    Require(sceKernelMunmap(reinterpret_cast<std::uint64_t>(mapped), length) == 0);
    Require(sceKernelAvailableFlexibleMemorySize(&available) == 0 && available == before);
}

// The upstream tests pass pointers to sceKernelMunmap; ours declares the exported uint64_t form.
static int Unmap(const void* address, std::size_t bytes) {
    return sceKernelMunmap(reinterpret_cast<std::uint64_t>(address), bytes);
}

static void CheckCheckedReleaseDirectMemory() {
    constexpr std::size_t page = 0x4000;
    std::int64_t phys = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, page * 2, 0, 0, &phys) == 0);
    Require(sceKernelCheckedReleaseDirectMemory(phys + 1, page) == SCE_KERNEL_ERROR_EINVAL);
    Require(sceKernelCheckedReleaseDirectMemory(phys, page + 1) == SCE_KERNEL_ERROR_EINVAL);
    Require(sceKernelCheckedReleaseDirectMemory(phys, 0) == 0);
    Require(sceKernelCheckedReleaseDirectMemory(phys, page * 3) == SCE_KERNEL_ERROR_ENOENT);
    void* mapped = nullptr;
    Require(sceKernelMapDirectMemory(&mapped, page * 2, 3, 0, phys, 0) == 0);
    Require(Unmap(mapped, page * 2) == 0);
    Require(sceKernelCheckedReleaseDirectMemory(phys + page, page) == 0);
    Require(sceKernelCheckedReleaseDirectMemory(phys, page * 2) == SCE_KERNEL_ERROR_ENOENT);
    Require(sceKernelCheckedReleaseDirectMemory(phys, page) == 0);
    Require(sceKernelCheckedReleaseDirectMemory(phys, page) == SCE_KERNEL_ERROR_ENOENT);
}

static void CheckDirectMemoryFollowsPhysicalPages() {
    constexpr std::size_t page = 0x4000;
    std::int64_t phys = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, page * 2, 0, 0, &phys) == 0);
    void* first = nullptr;
    Require(sceKernelMapDirectMemory(&first, page * 2, 3, 0, phys, 0) == 0);
    static_cast<unsigned char*>(first)[0] = 11;
    static_cast<unsigned char*>(first)[page + 5] = 22;
    void* alias = nullptr;
    Require(sceKernelMapDirectMemory(&alias, page, 3, 0, phys + page, 0) == 0);
    Require(alias != first && static_cast<unsigned char*>(alias)[5] == 22);
    static_cast<unsigned char*>(alias)[5] = 37;
    Require(static_cast<unsigned char*>(first)[page + 5] == 37);
    static_cast<unsigned char*>(first)[page + 6] = 48;
    Require(static_cast<unsigned char*>(alias)[6] == 48);
    Require(sceKernelMprotect(alias, page, 1) == 0);
    static_cast<unsigned char*>(first)[page + 5] = 59;
    Require(static_cast<unsigned char*>(alias)[5] == 59);
    Require(sceKernelMprotect(alias, page, 3) == 0);
    static_cast<unsigned char*>(alias)[5] = 22;
    Require(static_cast<unsigned char*>(first)[page + 5] == 22);
    Require(Unmap(alias, page) == 0);
    static_cast<unsigned char*>(first)[page + 5] = 22;
    Require(Unmap(first, page * 2) == 0);
    void* filler = nullptr;
    Require(sceKernelMapFlexibleMemory(&filler, page * 2, 3, 0) == 0);
    void* second = nullptr;
    Require(sceKernelMapDirectMemory(&second, page, 3, 0, phys + page, 0) == 0);
    Require(second != first && static_cast<unsigned char*>(second)[5] == 22);
    void* reserved = nullptr;
    Require(sceKernelReserveVirtualRange(&reserved, page, 0, 0) == 0);
    void* fixed = reserved;
    Require(sceKernelMapDirectMemory(&fixed, page, 1, 0x10, phys, 0) == 0);
    Require(fixed == reserved && static_cast<unsigned char*>(fixed)[0] == 11);
    Require(Unmap(fixed, page) == 0);
    Require(Unmap(second, page) == 0);
    Require(sceKernelReleaseDirectMemory(phys, page * 2) == 0);
    std::int64_t again = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, page * 2, 0, 0, &again) == 0 && again == phys);
    void* fresh = nullptr;
    Require(sceKernelMapDirectMemory(&fresh, page * 2, 3, 0, again, 0) == 0);
    Require(Unmap(fresh, page * 2) == 0);
    Require(Unmap(filler, page * 2) == 0);
    Require(sceKernelReleaseDirectMemory(again, page * 2) == 0);
}

static void CheckReleaseDirectMemoryClearsMappings() {
    constexpr std::size_t page = 0x4000;
    std::int64_t phys = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, page * 2, 0, 0, &phys) == 0);
    void* mapped = nullptr;
    Require(sceKernelMapDirectMemory(&mapped, page * 2, 3, 0, phys, 0) == 0);
    VirtualQueryInfo before{};
    Require(sceKernelVirtualQuery(mapped, 0, &before, sizeof(before)) == 0);
    Require(before.is_direct && before.offset == static_cast<std::uint64_t>(phys));
    Require(sceKernelReleaseDirectMemory(phys + page, page) == 0);
    VirtualQueryInfo split{};
    Require(sceKernelVirtualQuery(mapped, 0, &split, sizeof(split)) == 0);
    Require(split.is_direct && split.offset == static_cast<std::uint64_t>(phys));
    VirtualQueryInfo dropped{};
    Require(sceKernelVirtualQuery(static_cast<unsigned char*>(mapped) + page, 0, &dropped, sizeof(dropped)) != 0 || !dropped.is_direct);
    Require(sceKernelReleaseDirectMemory(phys, page) == 0);
    VirtualQueryInfo cleared{};
    Require(sceKernelVirtualQuery(mapped, 0, &cleared, sizeof(cleared)) != 0 || !cleared.is_direct);
    Require(Unmap(mapped, page * 2) == 0);
}

static void CheckGetDirectMemoryType() {
    constexpr std::size_t page = 0x4000;
    std::int64_t first = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, page * 2, 0, 3, &first) == 0);
    std::int64_t second = 0;
    Require(sceKernelAllocateDirectMemory(first + page * 2, first + page * 3, page, 0, 1, &second) == 0 && second == first + page * 2);
    int type = -1;
    std::int64_t start = -1;
    std::int64_t end = -1;
    Require(sceKernelGetDirectMemoryType(first, &type, &start, &end) == 0);
    Require(type == 3 && start == first && end == first + page * 2);
    type = -1;
    Require(sceKernelGetDirectMemoryType(first + page * 2 - 1, &type, &start, &end) == 0);
    Require(type == 3 && start == first && end == first + page * 2);
    Require(sceKernelGetDirectMemoryType(first + page * 2, &type, &start, &end) == 0);
    Require(type == 1 && start == second && end == second + page);
    Require(sceKernelGetDirectMemoryType(first, nullptr, &start, &end) == SCE_KERNEL_ERROR_EINVAL);
    Require(sceKernelGetDirectMemoryType(first, &type, nullptr, &end) == SCE_KERNEL_ERROR_EINVAL);
    Require(sceKernelGetDirectMemoryType(first, &type, &start, nullptr) == SCE_KERNEL_ERROR_EINVAL);
    Require(sceKernelReleaseDirectMemory(first, page) == 0);
    type = -1;
    start = -1;
    end = -1;
    Require(sceKernelGetDirectMemoryType(first, &type, &start, &end) == SCE_KERNEL_ERROR_ENOENT);
    Require(type == -1 && start == -1 && end == -1);
    Require(sceKernelGetDirectMemoryType(-1, &type, &start, &end) == SCE_KERNEL_ERROR_ENOENT);
    Require(sceKernelGetDirectMemoryType(first + page, &type, &start, &end) == 0);
    Require(type == 3 && start == first + page && end == first + page * 2);
    Require(sceKernelReleaseDirectMemory(first + page, page * 2) == 0);
    Require(sceKernelGetDirectMemoryType(second, &type, &start, &end) == SCE_KERNEL_ERROR_ENOENT);
}

static void CheckReleaseFlexibleMemory() {
    constexpr std::size_t length = 0x10000;
    std::size_t before = 0;
    std::size_t available = 0;
    Require(sceKernelAvailableFlexibleMemorySize(&before) == 0);
    void* mapped = nullptr;
    Require(sceKernelMapFlexibleMemory(&mapped, length, 3, 0) == 0 && mapped != nullptr);
    static_cast<volatile unsigned char*>(mapped)[length - 1] = 1;
    Require(sceKernelAvailableFlexibleMemorySize(&available) == 0 && available == before - length);
    Require(sceKernelReleaseFlexibleMemory(mapped, length) == 0);
    Require(sceKernelAvailableFlexibleMemorySize(&available) == 0 && available == before);
    void* again = mapped;
    Require(sceKernelMapFlexibleMemory(&again, length, 3, 0x90) == 0 && again == mapped);
    Require(static_cast<volatile unsigned char*>(again)[length - 1] == 0);
    Require(Unmap(again, length) == 0);
}

// Batch entries are input only (the mapped address is not written back), so the entries map at
// fixed addresses inside a reservation.
static void CheckBatchMapStopsAtInvalidEntry() {
    constexpr std::size_t page = 0x4000;
    constexpr int mapFixed = 0x10;
    constexpr int mapFlexible = 3;
    constexpr int unmap = 1;
    constexpr int protect = 2;
    void* reservation = nullptr;
    Require(sceKernelReserveVirtualRange(&reservation, page * 3, 0, 0) == 0);
    auto* base = static_cast<unsigned char*>(reservation);
    const auto flexible = [&](std::size_t index) { return KernelBatchMapEntry{base + page * index, 0, page, 3, 0, 0, mapFlexible}; };
    const auto mapped = [&](std::size_t index) {
        VirtualQueryInfo info{};
        return sceKernelVirtualQuery(base + page * index, 0, &info, sizeof(info)) == 0 && info.is_flexible && info.start <= reinterpret_cast<std::uintptr_t>(base + page * index);
    };
    for (const std::int32_t operation : {5, 6, -1, std::numeric_limits<std::int32_t>::max()}) {
        KernelBatchMapEntry entries[3] = {flexible(0), flexible(1), flexible(2)};
        entries[1].operation = operation;
        int processed = -1;
        Require(sceKernelBatchMap2(entries, 3, &processed, mapFixed) == SCE_KERNEL_ERROR_EINVAL);
        Require(processed == 1);
        Require(mapped(0) && !mapped(1) && !mapped(2));
        Require(Unmap(base, page) == 0);
    }
    KernelBatchMapEntry entries[3] = {flexible(0), flexible(1), flexible(2)};
    entries[1].operation = protect;
    entries[1].length = 0;
    int processed = -1;
    Require(sceKernelBatchMap2(entries, 3, &processed, mapFixed) == SCE_KERNEL_ERROR_EINVAL);
    Require(processed == 1 && mapped(0) && !mapped(1) && !mapped(2));
    KernelBatchMapEntry unmaps[2] = {entries[0], entries[0]};
    unmaps[0].operation = unmap;
    unmaps[1].operation = unmap;
    unmaps[1].length = 0;
    processed = -1;
    Require(sceKernelBatchMap2(unmaps, 2, &processed, mapFixed) == SCE_KERNEL_ERROR_EINVAL && processed == 1);
    Require(!mapped(0));
    Require(sceKernelBatchMap2(entries, 1, &processed, mapFixed) == 0 && processed == 1 && mapped(0));
    Require(Unmap(base, page * 3) == 0);
}

static void CheckMtypeprotect() {
    constexpr std::size_t page = 0x4000;
    std::int64_t phys = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, page * 3, 0, 0, &phys) == 0);
    void* mapped = nullptr;
    Require(sceKernelMapDirectMemory(&mapped, page * 3, 3, 0, phys, 0) == 0);
    auto* bytes = static_cast<unsigned char*>(mapped);
    Require(sceKernelMtypeprotect(bytes + page + 1, 1, 3, 1) == 0);
    const auto check = [&](std::size_t index, int expectedType, int expectedProtection) {
        VirtualQueryInfo info{};
        Require(sceKernelVirtualQuery(bytes + page * index, 0, &info, sizeof(info)) == 0);
        Require(info.is_direct && info.memory_type == expectedType && info.protection == expectedProtection);
        Require(info.start == reinterpret_cast<std::uintptr_t>(bytes + page * index) && info.end == info.start + page);
        Require(info.offset == static_cast<std::uint64_t>(phys) + page * index);
        int type = -1;
        std::int64_t start = -1;
        std::int64_t end = -1;
        Require(sceKernelGetDirectMemoryType(phys + static_cast<std::int64_t>(page * index), &type, &start, &end) == 0);
        Require(type == expectedType && start == phys + static_cast<std::int64_t>(page * index) && end == start + static_cast<std::int64_t>(page));
    };
    check(0, 0, 3);
    check(1, 3, 1);
    check(2, 0, 3);
    KernelBatchMapEntry entry{};
    entry.start = bytes + page * 2;
    entry.length = page;
    entry.protection = 3;
    entry.type = 5;
    entry.operation = 4;
    int processed = -1;
    Require(sceKernelBatchMap2(&entry, 1, &processed, 0) == 0 && processed == 1);
    check(0, 0, 3);
    check(1, 3, 1);
    check(2, 5, 3);
    Require(sceKernelMtypeprotect(bytes, page * 3, 2, 3) == 0);
    VirtualQueryInfo whole{};
    Require(sceKernelVirtualQuery(bytes, 0, &whole, sizeof(whole)) == 0);
    Require(whole.memory_type == 2 && whole.protection == 3);
    int type = -1;
    std::int64_t start = -1;
    std::int64_t end = -1;
    Require(sceKernelGetDirectMemoryType(phys + static_cast<std::int64_t>(page * 2), &type, &start, &end) == 0 && type == 2);
    Require(Unmap(mapped, page * 3) == 0);
    Require(sceKernelReleaseDirectMemory(phys, page * 3) == 0);
}

static void CheckDirectMemoryGpuProtBits() {
    constexpr std::size_t page = 0x4000;
    std::int64_t phys = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, page, 0, 0, &phys) == 0);
    void* mapping = nullptr;
    Require(sceKernelMapDirectMemory(&mapping, page, 0x3f2, 0, phys, 0) == 0);
    static_cast<unsigned char*>(mapping)[0] = 11;
    Require(static_cast<unsigned char*>(mapping)[0] == 11);
    Require(Unmap(mapping, page) == 0);
    Require(sceKernelReleaseDirectMemory(phys, page) == 0);
}

static void CheckNoOverwriteRefusesLiveMapping() {
    constexpr std::size_t page = 0x4000;
    constexpr int outOfMemory = static_cast<int>(0x8002000cu);
    std::int64_t phys = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, page * 2, 0, 0, &phys) == 0);
    void* mapped = nullptr;
    Require(sceKernelMapDirectMemory(&mapped, page, 3, 0, phys, 0) == 0);
    static_cast<unsigned char*>(mapped)[0] = 13;
    void* again = mapped;
    Require(sceKernelMapDirectMemory(&again, page, 3, 0x90, phys + page, 0) == outOfMemory);
    Require(again == mapped);
    void* flexible = mapped;
    Require(sceKernelMapFlexibleMemory(&flexible, page, 3, 0x90) == outOfMemory);
    Require(static_cast<unsigned char*>(mapped)[0] == 13);
    VirtualQueryInfo info{};
    Require(sceKernelVirtualQuery(mapped, 0, &info, sizeof(info)) == 0);
    Require(info.is_direct && info.offset == static_cast<std::uint64_t>(phys));
    Require(Unmap(mapped, page) == 0);
    Require(sceKernelReleaseDirectMemory(phys, page * 2) == 0);
}

static void CheckFixedReservationReplacesMappings() {
    constexpr std::size_t page = 0x4000;
    void* probe = nullptr;
    Require(sceKernelReserveVirtualRange(&probe, page * 4, 0, 0) == 0);
    Require(Unmap(probe, page * 4) == 0);
    void* const requested = static_cast<unsigned char*>(probe) + page;
    void* fixed = requested;
    Require(sceKernelReserveVirtualRange(&fixed, page * 2, 0x400010, 0) == 0);
    Require(fixed == requested);
    void* again = requested;
    Require(sceKernelReserveVirtualRange(&again, page * 2, 0x400010, 0) == 0);
    Require(again == requested);
    std::int64_t phys = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, page * 2, 0, 0, &phys) == 0);
    void* mapped = requested;
    Require(sceKernelMapDirectMemory(&mapped, page * 2, 3, 0x10, phys, 0) == 0);
    Require(mapped == requested);
    static_cast<unsigned char*>(mapped)[0] = 11;
    VirtualQueryInfo before{};
    Require(sceKernelVirtualQuery(mapped, 0, &before, sizeof(before)) == 0);
    Require(before.is_direct);
    void* reserved = requested;
    Require(sceKernelReserveVirtualRange(&reserved, page * 2, 0x10, 0) == 0);
    Require(reserved == requested);
    VirtualQueryInfo after{};
    Require(sceKernelVirtualQuery(reserved, 0, &after, sizeof(after)) == 0);
    Require(!after.is_committed && !after.is_direct && after.protection == 0);
    void* remapped = requested;
    Require(sceKernelMapDirectMemory(&remapped, page * 2, 3, 0x10, phys, 0) == 0);
    Require(remapped == requested);
    VirtualQueryInfo revived{};
    Require(sceKernelVirtualQuery(remapped, 0, &revived, sizeof(revived)) == 0);
    Require(revived.is_direct);
    bool refused = false;
    try {
        refused = sceKernelReserveVirtualRange(&reserved, page * 2, 0x90, 0) != 0;
    } catch (const std::exception&) {
        refused = true;
    }
    Require(refused);
    Require(Unmap(reserved, page * 2) == 0);
    Require(sceKernelReleaseDirectMemory(phys, page * 2) == 0);
    void* pooled = nullptr;
    Require(sceKernelMemoryPoolReserve(requested, page * 2, 0, 0x10, &pooled) == 0);
    Require(pooled == requested);
    Require(Unmap(pooled, page * 2) == 0);
}

static void CheckReservedRangeIsNotCommitted() {
    constexpr std::size_t page = 0x4000;
    void* reserved = nullptr;
    Require(sceKernelReserveVirtualRange(&reserved, page * 2, 0, 0) == 0);
    const auto start = reinterpret_cast<std::uintptr_t>(reserved);
    VirtualQueryInfo info{};
    Require(sceKernelVirtualQuery(reserved, 0, &info, sizeof(info)) == 0);
    Require(!info.is_committed && !info.is_direct && !info.is_flexible && info.protection == 0);
    Require(info.start == start && info.end == start + page * 2);
    std::int64_t phys = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, page, 0, 0, &phys) == 0);
    void* fixed = reserved;
    Require(sceKernelMapDirectMemory(&fixed, page, 3, 0x10, phys, 0) == 0);
    Require(fixed == reserved);
    static_cast<unsigned char*>(fixed)[0] = 7;
    Require(sceKernelVirtualQuery(reserved, 0, &info, sizeof(info)) == 0);
    Require(info.is_committed && info.is_direct && info.protection == 3);
    Require(info.start == start && info.end == start + page);
    Require(sceKernelVirtualQuery(static_cast<unsigned char*>(reserved) + page, 0, &info, sizeof(info)) == 0);
    Require(!info.is_committed && info.protection == 0);
    Require(info.start == start + page && info.end == start + page * 2);
    Require(Unmap(reserved, page * 2) == 0);
    Require(sceKernelReleaseDirectMemory(phys, page) == 0);
}

#if defined(__linux__)
static std::size_t LockedKilobytes() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmLck:", 0) == 0) return std::strtoull(line.c_str() + 6, nullptr, 10);
    }
    return 0;
}
#endif

static void CheckMlock() {
    constexpr std::size_t page = 0x4000;
#ifdef _WIN32
    constexpr std::size_t length = 0x400000;
#else
    constexpr std::size_t length = 0x10000;
#endif
    constexpr int outOfMemory = static_cast<int>(0x8002000cu);
    constexpr int invalid = static_cast<int>(0x80020016u);
    void* mapped = nullptr;
    Require(sceKernelMapFlexibleMemory(&mapped, length, 3, 0) == 0);
    auto* bytes = static_cast<unsigned char*>(mapped);
    Require(sceKernelMlock_nid_postfix(mapped, 0) == 0);
#if defined(__linux__)
    const auto lockedBefore = LockedKilobytes();
#endif
    Require(sceKernelMlock_nid_postfix(bytes + 1, length - page) == 0);
#ifdef _WIN32
    SIZE_T minimum = 0;
    SIZE_T maximum = 0;
    DWORD limits = 0;
    Require(GetProcessWorkingSetSizeEx(GetCurrentProcess(), &minimum, &maximum, &limits) && minimum >= length && maximum > minimum);
    Require(VirtualUnlock(mapped, length));
    Require(!VirtualUnlock(mapped, length) && GetLastError() == ERROR_NOT_LOCKED);
#elif defined(__linux__)
    Require(LockedKilobytes() - lockedBefore == length / 1024);
#endif
    Require(sceKernelMlock_nid_postfix(mapped, length) == 0);
    Require(sceKernelMlock_nid_postfix(mapped, length) == 0);
    bytes[length - 1] = 7;
    Require(sceKernelMlock_nid_postfix(reinterpret_cast<void*>(std::numeric_limits<std::uintptr_t>::max() - page + 1), page * 2) == invalid);
    void* reserved = nullptr;
    Require(sceKernelReserveVirtualRange(&reserved, page, 0, 0) == 0);
    Require(sceKernelMlock_nid_postfix(reserved, page) == outOfMemory);
    Require(Unmap(reserved, page) == 0);
    Require(Unmap(mapped, length) == 0);
    Require(sceKernelMlock_nid_postfix(mapped, page) == outOfMemory);
}

static void CheckSharedDirectMemoryLifecycle() {
    constexpr std::size_t page = 0x4000;
    std::int64_t phys = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, page * 3, 0, 0, &phys) == 0);
    void* first = nullptr;
    void* second = nullptr;
    Require(sceKernelMapDirectMemory(&first, page * 3, 3, 0, phys, 0) == 0);
    Require(sceKernelMapDirectMemory(&second, page * 3, 3, 0, phys, 0) == 0);
    VirtualQueryInfo info{};
    Require(sceKernelVirtualQuery(second, 0, &info, sizeof(info)) == 0);
    Require(info.is_direct && !info.is_flexible && info.offset == static_cast<std::uint64_t>(phys));
    auto* left = static_cast<unsigned char*>(first);
    auto* right = static_cast<unsigned char*>(second);
    left[0] = 31;
    right[page] = 47;
    left[page * 2] = 63;
    Require(right[0] == 31 && left[page] == 47 && right[page * 2] == 63);
    void* inaccessible = nullptr;
    Require(sceKernelMapDirectMemory(&inaccessible, page, 0, 0, phys + page, 0) == 0);
    left[page] = 48;
    Require(sceKernelMprotect(inaccessible, page, 1) == 0);
    Require(static_cast<const unsigned char*>(inaccessible)[0] == 48);
    Require(Unmap(inaccessible, page) == 0);
    Require(Unmap(left + page, page) == 0);
    right[page] = 79;
    Require(left[0] == 31 && left[page * 2] == 63);
    void* middle = left + page;
    Require(sceKernelMapDirectMemory(&middle, page, 3, 0x10, phys + page, 0) == 0);
    Require(left[page] == 79);
    Require(sceKernelVirtualQuery(middle, 0, &info, sizeof(info)) == 0);
    // The remap is virtually and physically contiguous with both neighbours, so, as the console's
    // vm_map coalesces such entries (no NO_COALESCE flag), the query may report the merged entry;
    // the physical offset at `middle` must still be phys + page.
    const auto middleAddress = reinterpret_cast<std::uintptr_t>(middle);
    Require(info.start <= middleAddress && middleAddress < info.end && info.offset + (middleAddress - info.start) == static_cast<std::uint64_t>(phys) + page);
    Require(sceKernelMprotect(second, page * 3, 0) == 0);
    left[page] = 95;
    Require(sceKernelMprotect(second, page * 3, 1) == 0);
    Require(right[page] == 95);
    Require(Unmap(first, page) == 0);
    Require(Unmap(left + page * 2, page) == 0);
    Require(Unmap(middle, page) == 0);
    Require(sceKernelMprotect(second, page * 3, 3) == 0);
    right[page * 2] = 111;
    void* reserved = nullptr;
    Require(sceKernelReserveVirtualRange(&reserved, page * 3, 0, 0) == 0);
    void* fixed = static_cast<unsigned char*>(reserved) + page;
    Require(sceKernelMapDirectMemory(&fixed, page, 3, 0x10, phys + page * 2, 0) == 0);
    Require(static_cast<unsigned char*>(fixed)[0] == 111);
    static_cast<unsigned char*>(fixed)[0] = 127;
    Require(right[page * 2] == 127);
    Require(sceKernelMapFlexibleMemory(&fixed, page, 3, 0x10) == 0);
    Require(static_cast<unsigned char*>(fixed)[0] == 0);
    Require(sceKernelVirtualQuery(fixed, 0, &info, sizeof(info)) == 0);
    Require(!info.is_direct && info.is_flexible && info.offset == 0);
    static_cast<unsigned char*>(fixed)[0] = 143;
    Require(right[page * 2] == 127);
    Require(Unmap(reserved, page * 3) == 0);
    Require(Unmap(second, page * 3) == 0);
    Require(sceKernelReleaseDirectMemory(phys + page, page) == 0);
    std::int64_t replacement = 0;
    Require(sceKernelAllocateDirectMemory(phys + page, phys + page * 2, page, 0, 0, &replacement) == 0);
    Require(replacement == phys + page);
    void* mixed = nullptr;
    Require(sceKernelMapDirectMemory(&mixed, page * 3, 3, 0, phys, 0) == 0);
    const auto* data = static_cast<const unsigned char*>(mixed);
    Require(data[0] == 31 && data[page * 2] == 127);
    Require(Unmap(mixed, page * 3) == 0);
    Require(sceKernelReleaseDirectMemory(phys, page * 3) == 0);
}

static void CheckHeapAfterMappingReuse() {
    constexpr std::size_t bytes = 0x30000;
    auto* pointer = static_cast<unsigned char*>(GuestHeap::GuestHeapAllocate_nid_postfix(bytes));
    std::memset(pointer, 0x5a, bytes);
    Require(pointer[0] == 0x5a && pointer[bytes - 1] == 0x5a);
    GuestHeap::GuestHeapFree_nid_postfix(pointer);
    pointer = static_cast<unsigned char*>(GuestHeap::GuestHeapAllocate_nid_postfix(bytes));
    std::memset(pointer, 0xa5, bytes);
    Require(pointer[0] == 0xa5 && pointer[bytes - 1] == 0xa5);
    GuestHeap::GuestHeapFree_nid_postfix(pointer);
}

static void CheckSingleViews() {
    constexpr std::size_t length = 0x10000;
    std::int64_t physical = 0;
    Require(sceKernelAllocateMainDirectMemory(length, 0x10000, 0, &physical) == 0);
    void* first = nullptr;
    Require(sceKernelMapDirectMemory(&first, length, 3, 0, physical, 0x10000) == 0);
    const auto address = reinterpret_cast<std::uint64_t>(first);
    Require(GuestMemoryBacking::GuestVirtualSingleView_nid_postfix(address, length));
    void* second = nullptr;
    Require(sceKernelMapDirectMemory(&second, 0x4000, 3, 0, physical + 0x8000, 0x4000) == 0);
    Require(!GuestMemoryBacking::GuestVirtualSingleView_nid_postfix(address, length));
    Require(GuestMemoryBacking::GuestVirtualSingleView_nid_postfix(address, 0x8000));
    Require(sceKernelMunmap(reinterpret_cast<std::uint64_t>(second), 0x4000) == 0);
    Require(GuestMemoryBacking::GuestVirtualSingleView_nid_postfix(address, length));
    Require(!GuestMemoryBacking::GuestVirtualSingleView_nid_postfix(0x1000, 0x1000));
    Require(sceKernelMunmap(address, length) == 0);
    Require(sceKernelReleaseDirectMemory(physical, length) == 0);
}

static void CheckFixedVirtualReservation() {
    constexpr std::size_t page = 0x4000;
    void* probe = nullptr;
    Require(sceKernelReserveVirtualRange(&probe, page * 4, 0, 0) == 0);
    Require(sceKernelMunmap(reinterpret_cast<std::uint64_t>(probe), page * 4) == 0);
    void* const requested = static_cast<unsigned char*>(probe) + page;
    void* fixed = requested;
    Require(sceKernelReserveVirtualRange(&fixed, page * 2, 0x400010, 0) == 0);
    Require(fixed == requested);
    Require(sceKernelMunmap(reinterpret_cast<std::uint64_t>(fixed), page * 2) == 0);
    void* pooled = nullptr;
    Require(sceKernelMemoryPoolReserve(requested, page * 2, 0, 0x10, &pooled) == 0);
    Require(pooled == requested);
    // Committed pool memory is backed and zeroed; decommitting discards it.
    auto* bytes = static_cast<volatile unsigned char*>(pooled);
    Require(sceKernelMemoryPoolCommit(pooled, page, 0, 3, 0) == 0);
    Require(bytes[0] == 0 && bytes[page - 1] == 0);
    bytes[0] = 0x5a;
    Require(sceKernelMemoryPoolDecommit(pooled, page, 0) == 0);
    Require(sceKernelMemoryPoolCommit(pooled, page, 0, 3, 0) == 0);
    Require(bytes[0] == 0);
    Require(sceKernelMunmap(reinterpret_cast<std::uint64_t>(pooled), page * 2) == 0);
}

static void CheckAccessibleRanges() {
    constexpr std::size_t length = 0x10000;
    void* mapping = nullptr;
    Require(sceKernelMapFlexibleMemory(&mapping, length, 3, 0) == 0);
    const auto address = reinterpret_cast<std::uint64_t>(mapping);
    using GuestMemoryBacking::GuestVirtualAccessible_nid_postfix;
    Require(GuestVirtualAccessible_nid_postfix(address, length, false) && GuestVirtualAccessible_nid_postfix(address, length, true));
    Require(!GuestVirtualAccessible_nid_postfix(address, length + 0x4000, false));
    Require(GuestMemoryBacking::GuestVirtualProtect_nid_postfix(mapping, 0x4000, GuestMemoryBacking::kProtCpuRead) == GuestMemoryBacking::Status::Ok);
    Require(GuestVirtualAccessible_nid_postfix(address, length, false) && !GuestVirtualAccessible_nid_postfix(address, length, true));
    Require(GuestVirtualAccessible_nid_postfix(address + 0x4000, length - 0x4000, true));
    Require(sceKernelMunmap(address, length) == 0);
    Require(!GuestVirtualAccessible_nid_postfix(address, 4, false));
}

#if defined(__linux__)
// Watches sharing a page: the page takes the strictest protection, a fault resolves only the
// watches that block it, and native access returns once no watch protects the page.
struct SharedWatch {
    GuestMemoryTracking::Watch* watch = nullptr;
    int reads = 0;
    int writes = 0;
};

static void ResolveSharedWatch(void* context, GuestMemoryTracking::Access access) {
    auto& shared = *static_cast<SharedWatch*>(context);
    if (access == GuestMemoryTracking::Access::Read) {
        ++shared.reads;
        shared.watch->Protect(GuestMemoryTracking::Protection::Read);
    } else {
        ++shared.writes;
        shared.watch->Protect(GuestMemoryTracking::Protection::ReadWrite);
    }
}

static void CheckSharedWatchPages() {
    const auto page = GuestMemoryTracking::GuestMemoryTrackingPageSize_nid_postfix();
    void* mapping = nullptr;
    Require(sceKernelMapFlexibleMemory(&mapping, 0x10000, 3, 0) == 0);
    auto* bytes = static_cast<volatile unsigned char*>(mapping);
    const auto base = reinterpret_cast<std::uint64_t>(mapping);
    SharedWatch first;
    SharedWatch second;
    // first covers pages 0-1, second pages 1-2: page 1 is shared.
    auto* firstWatch = new GuestMemoryTracking::Watch(base, page + page / 2, &first, ResolveSharedWatch);
    auto* secondWatch = new GuestMemoryTracking::Watch(base + page + page / 2, page + page / 2, &second, ResolveSharedWatch);
    first.watch = firstWatch;
    second.watch = secondWatch;
    firstWatch->Protect(GuestMemoryTracking::Protection::None);
    secondWatch->Protect(GuestMemoryTracking::Protection::Read);
    // Reading second's bytes in the shared page faults on first's protection: only first resolves.
    Require(bytes[page + page / 2 + 8] == 0);
    Require(first.reads == 1 && first.writes == 0 && second.reads == 0 && second.writes == 0);
    // Writing page 2 (second's alone) resolves second.
    bytes[page * 2 + 8] = 5;
    Require(second.writes == 1 && first.writes == 0);
    // Writing the shared page now resolves first only (second already allows writes).
    bytes[page + 8] = 6;
    Require(first.writes == 1 && second.writes == 1);
    // Protected again, then destroyed: page 0 and the shared page return to native access while
    // second still watches page 2.
    firstWatch->Protect(GuestMemoryTracking::Protection::None);
    secondWatch->Protect(GuestMemoryTracking::Protection::Read);
    delete firstWatch;
    bytes[0] = 7;
    Require(bytes[page + 8] == 6);
    Require(first.reads == 1 && first.writes == 1 && second.reads == 0 && second.writes == 1);
    bytes[page + page / 2 + 8] = 9;
    Require(second.writes == 2);
    secondWatch->Protect(GuestMemoryTracking::Protection::None);
    delete secondWatch;
    bytes[page * 2 + 8] = 1;
    Require(bytes[page * 2 + 8] == 1 && second.writes == 2);
    Require(sceKernelMunmap(base, 0x10000) == 0);
}
#endif

int main() {
#if defined(__linux__)
    CheckSharedWatchPages();
#endif
    CheckNamedAndHintedMappings();
    CheckInternalNamedFlexibleMapping();
    CheckHeapAfterMappingReuse();
    CheckSingleViews();
    CheckAccessibleRanges();
    CheckFixedVirtualReservation();
    CheckCheckedReleaseDirectMemory();
    CheckDirectMemoryFollowsPhysicalPages();
    CheckReleaseDirectMemoryClearsMappings();
    CheckFixedReservationReplacesMappings();
    CheckReservedRangeIsNotCommitted();
    CheckMlock();
    CheckSharedDirectMemoryLifecycle();
    CheckGetDirectMemoryType();
    CheckReleaseFlexibleMemory();
    CheckBatchMapStopsAtInvalidEntry();
    CheckMtypeprotect();
    CheckDirectMemoryGpuProtBits();
    CheckNoOverwriteRefusesLiveMapping();
    constexpr std::size_t page = 0x4000;
    const auto failed = reinterpret_cast<void*>(static_cast<std::uintptr_t>(-1));
    const auto reject = [&](std::size_t length, int protection, int flags, int fd,
                            std::int64_t offset, int error) {
        *__error_nid_postfix() = 0;
        Require(mmap_nid_postfix(nullptr, length, protection, flags, fd, offset) == failed);
        Require(*__error_nid_postfix() == error);
    };
    reject(0, 3, 0x1002, -1, 0, 22);
    reject(std::numeric_limits<std::size_t>::max(), 3, 0x1002, -1, 0, 22);
    reject(page, 8, 0x1002, -1, 0, 22);
    reject(page, 3, 0x1002, 0, 0, 22);
    reject(page, 3, 0x1002, -1, 1, 22);
    reject(page, 3, 0x1001, -1, 0, 45); // shared
    reject(page, 3, 0x1012, -1, 0, 45); // fixed
    reject(page, 3, 0x2, 0, 0, 45);    // file-backed
    reject(page, 3, 0x22, -1, 0, 45);  // Linux MAP_ANON is not guest MAP_ANON

    auto* memory = static_cast<unsigned char*>(mmap_nid_postfix(nullptr, page * 3 - 1, 3, 0x1002, -1, 0));
    Require(memory != failed && (reinterpret_cast<std::uintptr_t>(memory) & (page - 1)) == 0);
    for (std::size_t i = 0; i < page * 3; ++i) Require(memory[i] == 0);
    memory[0] = 42;
    memory[page * 2] = 73;
    {
        GuestAllocations::Mutation mutation;
        const auto range = mutation.Find(memory);
        Require(range.bytes == page * 3 && range.readable && range.writable);
    }
    Require(munmap_nid_postfix(memory + 1, page) == -1 && *__error_nid_postfix() == 22);
    Require(munmap_nid_postfix(memory, 0) == -1 && *__error_nid_postfix() == 22);
    Require(memory[0] == 42);
    Require(munmap_nid_postfix(memory + page, 1) == 0); // round to one guest page
    Require(memory[0] == 42 && memory[page * 2] == 73);
    Require(munmap_nid_postfix(memory, page) == 0);
    Require(memory[page * 2] == 73);
    Require(munmap_nid_postfix(memory + page * 2, page) == 0);
    // Unmapping free memory succeeds, as on the PS5 kernel.
    Require(munmap_nid_postfix(memory, page) == 0);
    // Flexible memory cannot be executable.
    reject(page, 5, 0x1002, -1, 0, 13);
    for (int protection : {0, 1, 3}) {
        void* mapped = mmap_nid_postfix(memory, 1, protection, 0x1002, -1, 0);
        Require(mapped != failed);
        {
            GuestAllocations::Mutation mutation;
            const auto range = mutation.Find(mapped);
            Require(range.readable == ((protection & 3) != 0));
            Require(range.writable == ((protection & 2) != 0));
        }
        Require(munmap_nid_postfix(mapped, 1) == 0);
    }

    // PS5 memory model (docs/research-notes.md).
    constexpr int fixed = 0x10;
    constexpr int noOverwrite = 0x80;
    constexpr int enomem = static_cast<int>(0x8002000C);
    constexpr int eacces = static_cast<int>(0x8002000D);
    constexpr int cpuReadWrite = 0x3;
    std::int64_t physical = -1;
    Require(sceKernelAllocateMainDirectMemory(page * 2, page, 0, &physical) == 0);
    // Two views of the same physical memory share contents.
    void* first = nullptr;
    void* second = nullptr;
    Require(sceKernelMapDirectMemory(&first, page * 2, cpuReadWrite, 0, physical, page) == 0);
    Require(sceKernelMapDirectMemory(&second, page, cpuReadWrite, 0, physical + page, page) == 0);
    static_cast<unsigned char*>(first)[page + 5] = 99;
    Require(static_cast<unsigned char*>(second)[5] == 99);
    Require(sceKernelMapDirectMemory(&first, page, 0x7, 0, physical, page) == eacces);
    // Contents survive unmapping one view.
    Require(sceKernelMunmap(reinterpret_cast<std::uint64_t>(second), page) == 0);
    Require(sceKernelMapDirectMemory(&second, page, cpuReadWrite, 0, physical + page, page) == 0);
    Require(static_cast<unsigned char*>(second)[5] == 99);
    VirtualQueryInfo info{};
    Require(sceKernelVirtualQuery(first, 0, &info, sizeof(info)) == 0);
    Require(info.start == reinterpret_cast<std::uintptr_t>(first) && info.end == info.start + page * 2 && info.is_direct && info.is_committed && info.offset == static_cast<std::uint64_t>(physical) && info.protection == cpuReadWrite);
    // Releasing physical memory unmaps every view.
    Require(sceKernelReleaseDirectMemory(physical, page * 2) == 0);
    Require(sceKernelVirtualQuery(second, 0, &info, sizeof(info)) != 0);

    // A reservation is unbacked; fixed maps replace it, NO_OVERWRITE refuses.
    void* reserved = nullptr;
    Require(sceKernelReserveVirtualRange(&reserved, page * 4, 0, page) == 0);
    Require(sceKernelVirtualQuery(reserved, 0, &info, sizeof(info)) == 0 && !info.is_committed && info.end - info.start == page * 4);
    Require(sceKernelMprotect(reserved, page, cpuReadWrite) == 0);
    void* inside = static_cast<unsigned char*>(reserved) + page;
    Require(sceKernelMapFlexibleMemory(&inside, page, cpuReadWrite, fixed | noOverwrite) == enomem);
    Require(sceKernelMapFlexibleMemory(&inside, page, cpuReadWrite, fixed) == 0 && inside == static_cast<unsigned char*>(reserved) + page);
    static_cast<unsigned char*>(inside)[0] = 7;
    Require(sceKernelVirtualQuery(inside, 0, &info, sizeof(info)) == 0 && info.is_flexible && info.is_committed && info.start == reinterpret_cast<std::uintptr_t>(inside) && info.end == info.start + page);
    Require(sceKernelMprotect(inside, page, 0x1) == 0);
    Require(sceKernelVirtualQuery(inside, 0, &info, sizeof(info)) == 0 && info.protection == 0x1);
    Require(static_cast<unsigned char*>(inside)[0] == 7);
    // Unmap skips holes and frees reserved parts.
    Require(sceKernelMunmap(reinterpret_cast<std::uint64_t>(reserved), page * 8) == 0);
    Require(sceKernelVirtualQuery(reserved, 0, &info, sizeof(info)) != 0);
    // The freed range can be claimed again at a fixed address.
    void* again = reserved;
    Require(sceKernelMapFlexibleMemory(&again, page, cpuReadWrite, fixed | noOverwrite) == 0 && again == reserved);
    Require(static_cast<unsigned char*>(again)[0] == 0);
    Require(sceKernelMunmap(reinterpret_cast<std::uint64_t>(again), page) == 0);

    // Translation to segments: adjacent views of contiguous physical memory form one run.
    std::int64_t run = -1;
    Require(sceKernelAllocateMainDirectMemory(page * 4, page, 0, &run) == 0);
    void* low = nullptr;
    Require(sceKernelReserveVirtualRange(&low, page * 4, 0, page) == 0);
    void* high = static_cast<unsigned char*>(low) + page * 2;
    Require(sceKernelMapDirectMemory(&low, page * 2, cpuReadWrite, fixed, run, page) == 0);
    Require(sceKernelMapDirectMemory(&high, page * 2, cpuReadWrite, fixed, run + page * 2, page) == 0);
    GuestMemoryBacking::Translation translation{};
    Require(GuestMemoryBacking::GuestVirtualTranslate_nid_postfix(reinterpret_cast<std::uint64_t>(low) + 16, page * 4 - 16, &translation));
    Require(translation.bytes == page * 4 - 16 && translation.offset == static_cast<std::uint64_t>(run) + 16);
    static_cast<unsigned char*>(high)[3] = 0x42;
    Require(static_cast<unsigned char*>(translation.segmentAlias)[run + page * 2 + 3] == 0x42);
    Require(GuestMemoryBacking::GuestSegmentAlive_nid_postfix(translation.segment));
    // A view of non-adjacent physical memory ends the run.
    Require(sceKernelMapDirectMemory(&high, page * 2, cpuReadWrite, fixed, run, page) == 0);
    Require(GuestMemoryBacking::GuestVirtualTranslate_nid_postfix(reinterpret_cast<std::uint64_t>(low), page * 4, &translation) && translation.bytes == page * 2);
    Require(sceKernelMunmap(reinterpret_cast<std::uint64_t>(low), page * 4) == 0);
    Require(!GuestMemoryBacking::GuestVirtualTranslate_nid_postfix(reinterpret_cast<std::uint64_t>(low), page, &translation));
    Require(sceKernelReleaseDirectMemory(run, page * 4) == 0);
    // Flexible memory has a segment of its own, which dies with its last view.
    void* flexible = nullptr;
    Require(sceKernelMapFlexibleMemory(&flexible, page, cpuReadWrite, 0) == 0);
    Require(GuestMemoryBacking::GuestVirtualTranslate_nid_postfix(reinterpret_cast<std::uint64_t>(flexible), page, &translation) && translation.offset == 0 && translation.segmentBytes == page);
    Require(sceKernelMunmap(reinterpret_cast<std::uint64_t>(flexible), page) == 0);
    Require(!GuestMemoryBacking::GuestSegmentAlive_nid_postfix(translation.segment));
}
