#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "SceTypes.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>

extern "C" {
void* APS5_VABI mmap_nid_postfix(void*, std::size_t, int, int, int, std::int64_t) noexcept;
int APS5_VABI munmap_nid_postfix(void*, std::size_t) noexcept;
int* APS5_VABI __error_nid_postfix();
int APS5_VABI sceKernelAllocateMainDirectMemory(std::size_t, std::size_t, int, std::int64_t*);
int APS5_VABI sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
int APS5_VABI sceKernelMapFlexibleMemory(void**, std::size_t, int, int);
int32_t APS5_VABI sceKernelMapNamedFlexibleMemory(void**, std::size_t, int, int, const char*);
int APS5_VABI sceKernelSetVirtualRangeName(const void*, std::uint64_t, const char*);
int APS5_VABI sceKernelClearVirtualRangeName(const void*, std::uint64_t);
int APS5_VABI sceKernelReserveVirtualRange(void**, std::size_t, int, std::size_t);
int APS5_VABI sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
int APS5_VABI sceKernelMunmap(std::uint64_t, std::size_t);
int APS5_VABI sceKernelMprotect(const void*, std::size_t, int);
int APS5_VABI sceKernelVirtualQuery(const void*, int, VirtualQueryInfo*, std::uint64_t);
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

int main() {
    CheckNamedAndHintedMappings();
    CheckSingleViews();
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
