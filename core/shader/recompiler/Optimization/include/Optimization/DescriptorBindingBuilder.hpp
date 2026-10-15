#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_DESCRIPTORBINDINGBUILDER_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_DESCRIPTORBINDINGBUILDER_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/BindingAllocator.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "Recompiler.hpp"
#include <cstdint>

namespace ShaderRecompiler {

std::uint32_t PointFilteredSamplerWord(std::uint32_t word0, std::uint32_t filter);
struct PreparedDescriptorBinding {
    DescriptorBinding descriptor;
    std::vector<std::uint32_t> resources;
    std::vector<bool> identityImageSwizzle;
    std::vector<std::uint32_t> samplerFilterElements;
    std::vector<bool> inactive;
    std::vector<std::uint32_t> tableSamplerMasks;
    std::size_t writtenBuffers = 0;
    std::size_t readOnlyBuffers = 0;
};

struct PreparedImageMetadata {
    std::uint32_t resource;
    std::uint32_t offset;
    RuntimeAbi::ResourceMetadata metadata;
};

struct DescriptorBindingPlan {
    std::vector<PreparedDescriptorBinding> bindings;
    std::vector<PreparedImageMetadata> imageMetadata;
    std::vector<PipelineSpecializationConstant> specialization;
    std::vector<std::uint32_t> runtimeState;
    bool storageData = false;
};

class DescriptorBindingBuilder {
public:
    DescriptorBindingPlan Prepare(const IrBindingLayout& layout, const ShaderInfo& info, IrShaderStage stage, const ResourceSnapshot& snapshot, std::span<const std::uint8_t> exportMappings = {}, bool registrationModules = false, bool fragmentDescriptorSet = false) const;
    DescriptorBindingPlan Select(const DescriptorBindingPlan& plan, std::span<const std::uint32_t> bindings) const;
    // tables: the image/sampler table elements of this invocation (ResolveImageTables), required
    // when the plan has an ImageTable, SamplerTable or ImageTableMap binding.
    // bufferOffsetAlignment: the device's storage buffer offset alignment; each guest buffer's
    // BufferViewMisalignment is written to its byte of the shader data's buffer offsets (0: none).
    void Populate(BindingAllocationResult& allocation, const CompiledBindingLayout& compiled, const DescriptorBindingPlan& plan, std::uint32_t userDataBase, const ResourceSnapshot& snapshot, const std::array<std::uint32_t, 3>& partialThreads, const ResolvedImageTables* tables = nullptr, std::uint32_t bufferOffsetAlignment = 0u) const;
    // Populate's guest words without a DescriptorBinding per binding: binding i of the plan gets
    // words[offsets[i], offsets[i + 1]) (offsets has one entry more than the plan has bindings), and
    // push the push data. The vectors are cleared first. Table bindings get their words too, but
    // their per-element flags and counts vary per invocation and are only set by Populate
    // (PlanHasTables: such a plan cannot share one program's bindings across draws).
    void PopulateWords(std::vector<std::uint32_t>& words, std::vector<std::uint32_t>& offsets, std::vector<std::byte>& push, const CompiledBindingLayout& compiled, const DescriptorBindingPlan& plan, std::uint32_t userDataBase, const ResourceSnapshot& snapshot, const std::array<std::uint32_t, 3>& partialThreads, const ResolvedImageTables* tables = nullptr, std::uint32_t bufferOffsetAlignment = 0u) const;
    // The plan has a binding whose elements a draw resolves at run time (image or sampler tables).
    [[nodiscard]] static bool PlanHasTables(const DescriptorBindingPlan& plan);
    void Populate(BindingAllocationResult& allocation, const IrProgram& program, const ResourceSnapshot& snapshot, const std::array<std::uint32_t, 3>& partialThreads, std::span<const std::uint8_t> exportMappings = {}) const;
    void Populate(BindingAllocationResult& allocation, const ShaderInfo& info, IrShaderStage stage, std::uint32_t userDataBase, const ResourceSnapshot& snapshot, const std::array<std::uint32_t, 3>& partialThreads, std::span<const std::uint8_t> exportMappings = {}) const;
};

}

#endif
