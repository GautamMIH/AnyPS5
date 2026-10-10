#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADERINPUTSTATE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADERINPUTSTATE_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "Recompiler.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

ShaderRecompiler::ShaderPixelStageInfo DecodePixelStageInfo(const Registers& context, const std::array<std::uint8_t, 8>& colorComponentMappings);
// header: the program's AGC shader header (the scratch size of a SCRATCH_EN program).
ShaderRecompiler::ShaderComputeStageInfo DecodeComputeStageInfo(const Registers& shader, std::span<const std::byte> header = {});
ShaderRecompiler::ShaderVertexStageInfo DecodeVertexStageInfo(std::span<const std::byte> header, std::uint64_t headerAddress, std::span<const std::uint32_t> userData, std::uint32_t userSgprBase = 0);

// The static part of DecodeVertexStageInfo: what a registered vertex-family header says about its
// fetch (the user SGPRs holding the attribute and buffer table pointers, the input semantics).
// Prepared once per registered header and user SGPR base; the tables are read per draw.
struct PreparedVertexHeader {
    struct Semantic {
        std::uint32_t semantic;
        std::int32_t hardwareMapping;
        std::int32_t sizeInElements;
    };
    bool fetch = false;
    std::int32_t attribReg = -1;
    std::int32_t bufferReg = -1;
    std::uint32_t shaderType = 0;
    std::vector<Semantic> semantics;
};
PreparedVertexHeader PrepareVertexHeader(std::span<const std::byte> header, std::uint64_t headerAddress, std::uint32_t userSgprBase = 0);
// The draw's part: the tables the user data points at, each read as one guest range.
ShaderRecompiler::ShaderVertexStageInfo DecodeVertexStageInfo(const PreparedVertexHeader& prepared, std::span<const std::uint32_t> userData);

}

#endif
