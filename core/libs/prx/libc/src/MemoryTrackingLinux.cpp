#include "prx/libc/include/MemoryTrackingPlatform.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <dlfcn.h>
#include <exception>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

namespace GuestMemoryTracking::Platform {
namespace {

FaultHandler faultHandler = nullptr;
struct sigaction previousAction{};

// Reads memory without faulting (the address may be unmapped): false if it is not readable.
bool readSafely(std::uintptr_t address, void* destination, std::size_t bytes) {
    iovec local{destination, bytes};
    iovec remote{reinterpret_cast<void*>(address), bytes};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(bytes);
}

// Return addresses on the faulting thread's stack: words pointing into a loaded module just after
// a call instruction (frame pointers are unreliable in guest code, so the stack is scanned).
void reportCallers(std::uintptr_t stack) {
    constexpr std::size_t words = 2048;
    constexpr int maximum = 24;
    int reported = 0;
    for (std::size_t index = 0; index < words && reported < maximum; ++index) {
        std::uintptr_t value = 0;
        if (!readSafely(stack + index * sizeof(value), &value, sizeof(value))) break;
        if (value < 0x10000) continue;
        std::uint8_t before[6]{};
        if (!readSafely(value - sizeof(before), before, sizeof(before))) continue;
        // call rel32 (E8), call r/m (FF /2: 2, 3 or 6 bytes before the return address).
        const bool call = before[1] == 0xe8 || before[0] == 0xff || before[3] == 0xff || before[4] == 0xff;
        if (!call) continue;
        Dl_info module{};
        if (dladdr(reinterpret_cast<void*>(value), &module) == 0 || module.dli_fname == nullptr) continue;
        const char* name = module.dli_fname;
        for (const char* cursor = name; *cursor != '\0'; ++cursor) if (*cursor == '/') name = cursor + 1;
        char line[384];
        const int length = std::snprintf(line, sizeof(line), "    caller stack+0x%zx: %s+0x%lx (%s)\n", index * sizeof(value), name,
            static_cast<unsigned long>(value - reinterpret_cast<std::uintptr_t>(module.dli_fbase)), module.dli_sname != nullptr ? module.dli_sname : "no symbol");
        if (length > 0) static_cast<void>(write(STDERR_FILENO, line, static_cast<std::size_t>(std::min<int>(length, sizeof(line) - 1))));
        ++reported;
    }
}

void reportUnhandledFault(const siginfo_t* info, const ucontext_t* native) {
    const auto instruction = static_cast<std::uintptr_t>(native->uc_mcontext.gregs[REG_RIP]);
    Dl_info module{};
    const bool located = dladdr(reinterpret_cast<void*>(instruction), &module) != 0 && module.dli_fname != nullptr;
    // The host thread carries the guest thread's name (scePthreadCreate).
    char thread[17]{};
    if (prctl(PR_GET_NAME, thread, 0, 0, 0) != 0) thread[0] = '\0';
    char message[512];
    const int length = std::snprintf(message, sizeof(message), "[AnyPS5] unhandled SIGSEGV in thread \"%s\": address %p, rip %p in %s+0x%lx (%s)\n",
        thread, info->si_addr, reinterpret_cast<void*>(instruction),
        located ? module.dli_fname : "unknown",
        located ? static_cast<unsigned long>(instruction - reinterpret_cast<std::uintptr_t>(module.dli_fbase)) : 0ul,
        located && module.dli_sname != nullptr ? module.dli_sname : "no symbol");
    if (length > 0) static_cast<void>(write(STDERR_FILENO, message, static_cast<std::size_t>(std::min<int>(length, sizeof(message) - 1))));
    const auto* g = native->uc_mcontext.gregs;
    char registers[640];
    const int registerLength = std::snprintf(registers, sizeof(registers),
        "    rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rbp %llx rsp %llx\n    r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx\n",
        static_cast<unsigned long long>(g[REG_RAX]), static_cast<unsigned long long>(g[REG_RBX]), static_cast<unsigned long long>(g[REG_RCX]), static_cast<unsigned long long>(g[REG_RDX]),
        static_cast<unsigned long long>(g[REG_RSI]), static_cast<unsigned long long>(g[REG_RDI]), static_cast<unsigned long long>(g[REG_RBP]), static_cast<unsigned long long>(g[REG_RSP]),
        static_cast<unsigned long long>(g[REG_R8]), static_cast<unsigned long long>(g[REG_R9]), static_cast<unsigned long long>(g[REG_R10]), static_cast<unsigned long long>(g[REG_R11]),
        static_cast<unsigned long long>(g[REG_R12]), static_cast<unsigned long long>(g[REG_R13]), static_cast<unsigned long long>(g[REG_R14]), static_cast<unsigned long long>(g[REG_R15]));
    if (registerLength > 0) static_cast<void>(write(STDERR_FILENO, registers, static_cast<std::size_t>(std::min<int>(registerLength, sizeof(registers) - 1))));
    reportCallers(static_cast<std::uintptr_t>(native->uc_mcontext.gregs[REG_RSP]));
}

void handleFault(int signal, siginfo_t* info, void* context) {
    const auto* native = static_cast<const ucontext_t*>(context);
    const auto error = native->uc_mcontext.gregs[REG_ERR];
    if (info->si_code == SEGV_ACCERR && (error & 16) == 0) {
        try {
            if (faultHandler(reinterpret_cast<std::uintptr_t>(info->si_addr), (error & 2) != 0)) return;
        } catch (...) {
            std::terminate();
        }
    }
    if (previousAction.sa_handler == SIG_DFL || previousAction.sa_handler == SIG_IGN) {
        reportUnhandledFault(info, native);
        if (sigaction(signal, &previousAction, nullptr) != 0) std::terminate();
        if (raise(signal) != 0) std::terminate();
        return;
    }
    if ((previousAction.sa_flags & SA_SIGINFO) != 0) previousAction.sa_sigaction(signal, info, context);
    else previousAction.sa_handler(signal);
}

void protect(std::uint64_t address, std::size_t bytes, int protection) {
    if (mprotect(reinterpret_cast<void*>(address), bytes, protection) != 0) throw std::system_error(errno, std::generic_category(), "guest memory tracking mprotect failed");
}

}

std::size_t PageSize() {
    static const auto size = [] {
        const auto result = sysconf(_SC_PAGESIZE);
        if (result <= 0) throw std::runtime_error("invalid native page size");
        return static_cast<std::size_t>(result);
    }();
    return size;
}

void Install(FaultHandler handler) {
    if (handler == nullptr || faultHandler != nullptr) throw std::runtime_error("invalid guest memory fault handler installation");
    faultHandler = handler;
    struct sigaction action{};
    action.sa_flags = SA_SIGINFO;
    action.sa_sigaction = handleFault;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGSEGV, &action, &previousAction) != 0) {
        const auto error = errno;
        faultHandler = nullptr;
        throw std::system_error(error, std::generic_category(), "guest memory fault handler installation failed");
    }
}

std::vector<Region> Query(std::uint64_t address, std::size_t bytes) {
    std::ifstream maps("/proc/self/maps");
    if (!maps) throw std::runtime_error("cannot query tracked guest memory mappings");
    std::vector<Region> regions;
    const auto end = address + bytes;
    std::string line;
    while (address < end && std::getline(maps, line)) {
        std::istringstream input(line);
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        char separator = 0;
        std::string permissions;
        if (!(input >> std::hex >> first >> separator >> last >> permissions) || separator != '-' || permissions.size() < 3 || first >= last) throw std::runtime_error("invalid tracked guest memory mapping");
        if (last <= address) continue;
        if (first > address || permissions[0] != 'r' || permissions[1] != 'w' || permissions[2] != '-') throw std::runtime_error("tracked render memory must be mapped, writable and non-executable");
        const auto next = std::min(end, last);
        regions.push_back({address, static_cast<std::size_t>(next - address), static_cast<std::uint64_t>(PROT_READ | PROT_WRITE | (permissions[2] == 'x' ? PROT_EXEC : 0))});
        address = next;
    }
    if (address != end) throw std::runtime_error("tracked guest memory range is not mapped");
    return regions;
}

void Protect(std::uint64_t address, std::size_t bytes, Protection protection) {
    if (protection != Protection::None && protection != Protection::Read) throw std::invalid_argument("invalid tracked page protection");
    protect(address, bytes, protection == Protection::None ? PROT_NONE : PROT_READ);
}

void Restore(const std::vector<Region>& regions) {
    for (const auto& region : regions) protect(region.address, region.bytes, static_cast<int>(region.protection));
}

}
