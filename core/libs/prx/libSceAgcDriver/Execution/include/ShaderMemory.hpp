#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SHADERMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SHADERMEMORY_HPP

#include "Recompiler.hpp"
#include <map>
#include <memory>

namespace AgcDriver {

class ShaderMemory {
public:
    explicit ShaderMemory(std::span<const ShaderRecompiler::MemoryRegion> initial);
    // Walks the request's resources, reading guest memory the snapshot lacks. The result compiles
    // the request (Recompile(request, capture)) without walking or resolving it again.
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> Capture(const ShaderRecompiler::RecompileRequest& request);
    [[nodiscard]] std::vector<ShaderRecompiler::MemoryRegion> Regions() const;

private:
    static bool read(void* context, std::uint64_t address, std::uint32_t* value);
    std::map<std::uint64_t, std::vector<std::byte>> regions;
};

}

#endif
