#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_DESCRIPTORBINDINGBUILDER_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_DESCRIPTORBINDINGBUILDER_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/BindingAllocator.hpp"
#include "Recompiler.hpp"

namespace ShaderRecompiler {

class DescriptorBindingBuilder {
public:
    // `partialThreads`: ShaderComputeStageInfo::partialThreads (zero outside compute).
    // bufferOffsetAlignment: SpirvTarget::storageBufferOffsetAlignment; each guest buffer's
    // BufferViewMisalignment is written into its shader-data byte offset.
    void Populate(BindingAllocationResult& allocation, const IrProgram& program, const ResourceSnapshot& snapshot, const std::array<std::uint32_t, 3>& partialThreads, std::uint32_t bufferOffsetAlignment = 0) const;
    void Populate(BindingAllocationResult& allocation, const ShaderInfo& info, IrShaderStage stage, std::uint32_t userDataBase, const ResourceSnapshot& snapshot, const std::array<std::uint32_t, 3>& partialThreads, std::uint32_t bufferOffsetAlignment = 0) const;
};

}

#endif
