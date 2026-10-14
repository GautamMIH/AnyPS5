#include "SceTypes.hpp"
#ifdef __linux__
#include <atomic>
#include <cstdio>
#include <chrono>
#include <thread>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" {
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
int APS5_VABI scePthreadAttrInit(PthreadAttr* attr);
int APS5_VABI scePthreadAttrDestroy(PthreadAttr* attr);
int APS5_VABI scePthreadAttrGet(Pthread thread, PthreadAttr* attr);
int APS5_VABI scePthreadAttrSetstacksize(PthreadAttr* attr, std::size_t stackSize);
int APS5_VABI scePthreadAttrGetstack(const PthreadAttr* attr, void** stackAddress, std::size_t* stackSize);
Pthread APS5_VABI scePthreadSelf();
#ifdef __linux__
int APS5_VABI scePthreadDetach(Pthread thread);
#endif
}

static constexpr int SCE_OK = 0;
static constexpr std::size_t FRAME_SIZE = 4096;
static constexpr std::size_t FRAME_MARGIN = 2 * FRAME_SIZE;
static constexpr std::size_t HOST_CALL_STACK = 256 * 1024;
static constexpr std::size_t STACK_SIZES[] = {16384, 65536, 1u << 20, 16u << 20};

thread_local unsigned char threadLocalBlock[512 * 1024];

static void Require(bool value) { if (!value) std::abort(); }

static std::uintptr_t Descend(std::uintptr_t floor);
static std::uintptr_t (*volatile descend)(std::uintptr_t) = Descend;

static std::uintptr_t Descend(std::uintptr_t floor) {
    volatile unsigned char frame[FRAME_SIZE];
    frame[0] = 0x5a;
    frame[FRAME_SIZE - 1] = 0xa5;
    const auto here = reinterpret_cast<std::uintptr_t>(&frame[0]);
    const auto deepest = here < floor + FRAME_MARGIN ? here : descend(floor);
    Require(frame[0] == 0x5a && frame[FRAME_SIZE - 1] == 0xa5);
    return deepest;
}

static void* APS5_VABI Worker(void* arg) {
    const auto requested = *static_cast<const std::size_t*>(arg);
    threadLocalBlock[0] = 1;
    threadLocalBlock[sizeof(threadLocalBlock) - 1] = 2;

    PthreadAttr attr = nullptr;
    Require(scePthreadAttrInit(&attr) == SCE_OK);
    Require(scePthreadAttrGet(scePthreadSelf(), &attr) == SCE_OK);
    void* address = nullptr;
    std::size_t size = 0;
    Require(scePthreadAttrGetstack(&attr, &address, &size) == SCE_OK);
    Require(scePthreadAttrDestroy(&attr) == SCE_OK);
    Require(size == requested);

    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto local = reinterpret_cast<std::uintptr_t>(&size);
    Require(local > begin && local < begin + size);
    Require(local - begin + FRAME_SIZE >= requested);
    const auto deepest = Descend(begin);
    Require(deepest >= begin && deepest < begin + FRAME_MARGIN);
    const auto hostDeepest = descend(begin - HOST_CALL_STACK);
    Require(hostDeepest < begin - HOST_CALL_STACK + FRAME_MARGIN);

    Require(threadLocalBlock[0] == 1 && threadLocalBlock[sizeof(threadLocalBlock) - 1] == 2);
    return arg;
}

#ifdef __linux__
bool GuestThreadStackHostRangeOverlaps(std::uintptr_t start, std::uintptr_t end);
static std::atomic<std::uintptr_t> retiredStackWord{0};
static std::atomic<bool> detachedDone{false};
static int detachedTid = 0;

static void* APS5_VABI DetachedWorker(void*) {
    int local = 0;
    detachedTid = static_cast<int>(syscall(SYS_gettid));
    retiredStackWord.store(reinterpret_cast<std::uintptr_t>(&local), std::memory_order_relaxed);
    detachedDone.store(true, std::memory_order_release);
    return nullptr;
}

static void* APS5_VABI ReaperWorker(void*) {
    return nullptr;
}

// Upstream watched the release through its GuestArena unmap observer; this branch's guest memory
// (GuestMemoryBacking) releases a stack by carving its area, which leaves the pages inaccessible
// (a reserved placeholder) or unmapped, so the test reads the host mapping instead.
static bool HostPageWritable(std::uintptr_t address) {
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    Require(maps != nullptr);
    bool writable = false;
    char line[512];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long low = 0;
        unsigned long long high = 0;
        char permissions[5] = {};
        if (std::sscanf(line, "%llx-%llx %4s", &low, &high, permissions) != 3 || address < low || address >= high) continue;
        writable = permissions[1] == 'w';
        break;
    }
    std::fclose(maps);
    return writable;
}
#endif

int main() {
    for (auto stackSize : STACK_SIZES) {
        PthreadAttr attr = nullptr;
        Require(scePthreadAttrInit(&attr) == SCE_OK);
        Require(scePthreadAttrSetstacksize(&attr, stackSize) == SCE_OK);
        Pthread thread = nullptr;
        Require(scePthreadCreate(&thread, &attr, Worker, &stackSize, nullptr) == SCE_OK);
        Require(scePthreadAttrDestroy(&attr) == SCE_OK);
        void* result = nullptr;
        Require(scePthreadJoin(thread, &result) == SCE_OK);
        Require(result == &stackSize);
    }
#ifdef __linux__
    Pthread detached = nullptr;
    Require(scePthreadCreate(&detached, nullptr, DetachedWorker, nullptr, nullptr) == SCE_OK);
    Require(scePthreadDetach(detached) == SCE_OK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!detachedDone.load(std::memory_order_acquire)) {
        Require(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto task = "/proc/self/task/" + std::to_string(detachedTid);
    while (access(task.c_str(), F_OK) == 0) {
        Require(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Pthread reaper = nullptr;
    Require(scePthreadCreate(&reaper, nullptr, ReaperWorker, nullptr, nullptr) == SCE_OK);
    Require(scePthreadJoin(reaper, nullptr) == SCE_OK);
    // Creating the reaper released the exited detached thread's stack, host-only pages included.
    const auto retired = retiredStackWord.load(std::memory_order_relaxed);
    const auto retiredPage = retired & ~std::uintptr_t{0x3fff};
    Require(!HostPageWritable(retired));
    Require(!GuestThreadStackHostRangeOverlaps(retiredPage - 0x4000, retiredPage + 0x4000));
#endif
}
