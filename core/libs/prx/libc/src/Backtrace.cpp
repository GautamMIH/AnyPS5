#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstddef>
#include <cstdint>

// The libc backtrace API hands out a linked list of SceLibcCallFrame records whose layout is not
// documented, so the walk reports no frames. Callers (for example IL2CPP's managed stack traces)
// treat an empty backtrace as a normal result.
namespace {
constexpr int BACKTRACE_OK = 0;
constexpr int BACKTRACE_ERROR_INVALID = 22;
}

extern "C" {

int APS5_VABI sceLibcBacktraceGetBufferSize_nid_postfix(std::uint32_t maxDepth, std::uint32_t* depth, std::size_t* bufferSize) {
    static_cast<void>(maxDepth);
    if (depth == nullptr || bufferSize == nullptr) return BACKTRACE_ERROR_INVALID;
    *depth = 0;
    *bufferSize = 0;
    return BACKTRACE_OK;
}

int APS5_VABI sceLibcBacktraceSelf_nid_postfix(std::uint32_t depth, void* frames, std::size_t bufferSize, std::uint32_t* count) {
    static_cast<void>(depth);
    static_cast<void>(frames);
    static_cast<void>(bufferSize);
    if (count == nullptr) return BACKTRACE_ERROR_INVALID;
    *count = 0;
    return BACKTRACE_OK;
}

}
