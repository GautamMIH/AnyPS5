#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SHADERMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SHADERMEMORY_HPP

#include "Recompiler.hpp"
#include "Optimization/SrtWalker.hpp"
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver {

class ShaderMemory {
public:
    explicit ShaderMemory(std::span<const ShaderRecompiler::MemoryRegion> initial);
    // Walks the request's resources, reading guest memory the snapshot lacks. The result compiles
    // the request (Recompile(request, capture)) without walking or resolving it again.
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> Capture(const ShaderRecompiler::RecompileRequest& request);
    // As Capture, for a request of the same program and context as sameProgram's (see
    // ShaderRecompiler::CaptureResources): only the resources are walked again.
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> Capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::ResourceCapture& sameProgram);
    [[nodiscard]] std::vector<ShaderRecompiler::MemoryRegion> Regions() const;
    // The guest dwords the latest Capture read outside the initial regions, by address, with values.
    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint32_t>> CapturedWords() const;
    // Adds a guest dword known to hold value (an input of a reused capture) as if it had been read.
    void Insert(std::uint64_t address, std::uint32_t value);

private:
    static bool read(void* context, std::uint64_t address, std::uint32_t* value);
    bool initial(std::uint64_t address) const;
    ShaderRecompiler::SrtRuntime runtime(const ShaderRecompiler::RecompileRequest& request);
    std::map<std::uint64_t, std::vector<std::byte>> regions;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> initialRanges;
    std::vector<std::uint64_t> accessed;
};

// Reuses shader resource captures across draws. A stage whose program (the registered snapshot,
// held so its code cannot be replaced at the same address), request context and user data match an
// earlier capture, and whose every guest input dword still holds the value captured, reuses that
// capture: no source lookup, SRT walk or per-dword guarded read. The dwords are checked with the
// same range guards the capture's reads had, then compared by value. Cleared with the device.
class CaptureMemo {
public:
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> Capture(const std::shared_ptr<const void>& owner, const ShaderRecompiler::RecompileRequest& request, ShaderMemory& memory);
    void Clear();

private:
    struct Entry {
        std::shared_ptr<const void> owner;
        std::vector<std::uint64_t> key;
        std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> words;
        std::uint64_t mappingGeneration = 0;
    };
    bool reusable(const Entry& entry, ShaderMemory& memory) const;
    std::unordered_map<std::uint64_t, Entry> entries;
    // The latest capture of each program and context, whatever its user data (see
    // ShaderMemory::Capture(request, sameProgram)).
    struct Program {
        // Held so no other code can be registered at the owner's address while the entry lives.
        std::shared_ptr<const void> owner;
        std::vector<std::uint64_t> key;
        std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;
    };
    std::unordered_map<std::uint64_t, Program> programs;
    std::vector<std::uint64_t> key;
};

}

#endif
