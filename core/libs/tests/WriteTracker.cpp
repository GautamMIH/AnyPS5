#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

void RequireAt(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "Write tracker check failed at line %d\n", line);
        std::abort();
    }
}
#define Require(condition) RequireAt((condition), __LINE__)

void CheckCpuWrites() {
    using namespace AgcDriver::WriteTracker;
    if (!Available()) {
        std::puts("Write watch unavailable (userfaultfd write-protect); CPU write checks skipped");
        Require(CpuWrittenSince(0x1000, 0x1000, CpuMark(0x1000, 0x1000)));
        return;
    }
    constexpr std::uint64_t block = 64 * 1024;
    constexpr std::uint64_t size = block * 8;
    auto* bytes = static_cast<unsigned char*>(GuestMemoryBacking::GuestMemoryBackingMap_nid_postfix(nullptr, size, block, 3));
    std::memset(bytes, 1, size);
    const auto base = reinterpret_cast<std::uint64_t>(bytes);

    const auto mark = CpuMark(base, size);
    Require(!CpuWrittenSince(base, size, mark));
    bytes[block * 3 + 7] = 2;
    // Another user's collection does not hide the write from this mark.
    const auto other = CpuMark(base + block * 3, block);
    Require(CpuWrittenSince(base + block * 3, block, mark));
    Require(CpuWrittenSince(base, size, mark));
    Require(!CpuWrittenSince(base + block * 3, block, other));
    Require(!CpuWrittenSince(base + block * 5, block * 2, mark));

    // Kernel writes into the mapping (read(2) into a buffer) are seen as well.
    int pipe_[2];
    Require(pipe(pipe_) == 0);
    const char message[32] = "kernel write";
    Require(write(pipe_[1], message, sizeof(message)) == sizeof(message));
    Require(read(pipe_[0], bytes + block * 6, sizeof(message)) == sizeof(message));
    Require(CpuWrittenSince(base + block * 6, block, mark));

    // Writes through the host alias are not; the driver reports those with NoteAliasWrite.
    const auto beforeAlias = CpuMark(base, size);
    const unsigned char value = 9;
    GuestMemoryBacking::GuestMemoryBackingWrite_nid_postfix(base + block, &value, 1);
    Require(bytes[block] == 9);
    Require(!CpuWrittenSince(base, size, beforeAlias));

    // A range is collected once per epoch: a write racing with the epoch shows at the next one.
    NextEpoch();
    const auto epochMark = CpuMark(base + block * 4, block);
    bytes[block * 4 + 8] = 3;
    Require(!CpuWrittenSince(base + block * 4, block, epochMark));
    NextEpoch();
    Require(CpuWrittenSince(base + block * 4, block, epochMark));

    // Memory outside the watched guest views is always "written".
    unsigned char host[64] = {};
    const auto hostAddress = reinterpret_cast<std::uint64_t>(host);
    Require(CpuWrittenSince(hostAddress, sizeof(host), CpuMark(hostAddress, sizeof(host))));

    GuestMemoryBacking::GuestMemoryBackingUnmap_nid_postfix(bytes, size);
    close(pipe_[0]);
    close(pipe_[1]);
}

// Direct memory mapped at two guest addresses: a write through either view shows in both.
void CheckAliasedViews() {
    using namespace AgcDriver::WriteTracker;
    if (!Available()) return;
    using GuestMemoryBacking::Kind;
    using GuestMemoryBacking::Status;
    constexpr std::uint64_t block = 64 * 1024;
    constexpr std::uint64_t size = block * 4;
    constexpr std::int64_t physical = 0x300000000ll;
    void* first = nullptr;
    void* second = nullptr;
    Require(GuestMemoryBacking::GuestVirtualMap_nid_postfix(&first, size, block, Kind::Direct, 3, 0, physical) == Status::Ok);
    // The second view shows the last three blocks of the first one's.
    Require(GuestMemoryBacking::GuestVirtualMap_nid_postfix(&second, size - block, block, Kind::Direct, 3, 0, physical + block) == Status::Ok);
    auto* a = static_cast<unsigned char*>(first);
    auto* b = static_cast<unsigned char*>(second);
    const auto aBase = reinterpret_cast<std::uint64_t>(a);
    const auto bBase = reinterpret_cast<std::uint64_t>(b);

    NextEpoch();
    const auto aMark = CpuMark(aBase, size);
    const auto bMark = CpuMark(bBase, size - block);
    Require(!CpuWrittenSince(bBase, size - block, bMark));
    // Written through the first view, seen through the second (and still through the first).
    a[block * 2 + 5] = 7;
    Require(b[block + 5] == 7);
    NextEpoch();
    Require(CpuWrittenSince(bBase + block, block, bMark));
    Require(!CpuWrittenSince(bBase, block, bMark));
    Require(CpuWrittenSince(aBase + block * 2, block, aMark));
    // Written through the second view, seen through the first after the second was collected.
    NextEpoch();
    const auto aMark2 = CpuMark(aBase, size);
    b[block * 2 + 9] = 4;
    NextEpoch();
    Require(CpuWrittenSince(bBase + block * 2, block, bMark));
    NextEpoch();
    Require(CpuWrittenSince(aBase + block * 3, block, aMark2));
    Require(!CpuWrittenSince(aBase, block * 3, aMark2));
    // Memory no other view shows is unaffected.
    Require(!CpuWrittenSince(aBase, block, aMark));

    Require(GuestMemoryBacking::GuestVirtualUnmap_nid_postfix(second, size - block) == Status::Ok);
    Require(GuestMemoryBacking::GuestVirtualUnmap_nid_postfix(first, size) == Status::Ok);
}

void CheckReportedWrites() {
    using namespace AgcDriver::WriteTracker;
    const auto gpu = GpuWriteGeneration();
    Require(!GpuWritten(0x100000000ull, 0x1000));
    NoteGpuWrite(0x100000000ull, 0x2000);
    NoteGpuWrite(0x100003000ull, 0x1000);
    NoteGpuWrite(0x100001000ull, 0x2800);
    Require(GpuWriteGeneration() != gpu);
    Require(GpuWritten(0x100002800ull, 1) && GpuWritten(0x100003fffull, 1));
    Require(!GpuWritten(0x100004000ull, 0x1000) && !GpuWritten(0xfffff000ull, 0x1000));

    const auto alias = AliasWriteGeneration();
    Require(!AliasWrittenSince(0x200000000ull, 0x1000, alias));
    NoteAliasWrite(0x200000800ull, 0x100);
    Require(AliasWrittenSince(0x200000000ull, 0x1000, alias));
    Require(!AliasWrittenSince(0x200001000ull, 0x1000, alias));
    Require(!AliasWrittenSince(0x200000000ull, 0x1000, AliasWriteGeneration()));
    // Queries older than the remembered history answer "written".
    for (int index = 0; index < 5000; ++index) NoteAliasWrite(0x300000000ull, 16);
    Require(AliasWrittenSince(0x200001000ull, 0x1000, alias));
}

}

int main() {
    CheckCpuWrites();
    CheckAliasedViews();
    CheckReportedWrites();
    std::puts("Write tracker tests passed");
}
