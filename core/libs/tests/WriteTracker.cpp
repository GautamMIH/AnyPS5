#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
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
    Require(Available());
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    const int memory = memfd_create("write-tracker-test", 0);
    Require(memory >= 0 && ftruncate(memory, static_cast<off_t>(page * 16)) == 0);
    auto* bytes = static_cast<unsigned char*>(mmap(nullptr, page * 16, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0));
    Require(bytes != MAP_FAILED);
    for (std::size_t index = 0; index < page * 16; ++index) bytes[index] = 1;
    const auto base = reinterpret_cast<std::uint64_t>(bytes);

    int listened = 0;
    AddClearListener(&listened, [&] { ++listened; });
    const auto clears = Clears();
    Clear();
    Require(listened == 1 && Clears() == clears + 1);
    Require(!CpuWritten(base, page * 16));

    bytes[page * 3 + 7] = 2;
    Require(CpuWritten(base + page * 3, page));
    Require(CpuWritten(base, page * 16));
    Require(!CpuWritten(base + page * 5, page * 2));

    // Kernel writes into the mapping (read(2) into a buffer) are tracked as well.
    int pipe_[2];
    Require(pipe(pipe_) == 0);
    const char message[32] = "kernel write";
    Require(write(pipe_[1], message, sizeof(message)) == sizeof(message));
    Require(read(pipe_[0], bytes + page * 7, sizeof(message)) == sizeof(message));
    Require(CpuWritten(base + page * 7, page));

    RemoveClearListener(&listened);
    Clear();
    Require(listened == 1);
    Require(!CpuWritten(base, page * 16));
    munmap(bytes, page * 16);
    close(memory);
    close(pipe_[0]);
    close(pipe_[1]);
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
    CheckReportedWrites();
    std::puts("Write tracker tests passed");
}
