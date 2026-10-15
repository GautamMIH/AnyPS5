#include "Optimization/DescriptorBindingBuilder.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "SpirvBackend/SpirvEmitterHelpers.hpp"
#include "SpirvBackend/SpirvBufferFormat.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace ShaderRecompiler {

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

// Debug aid: APS5_TRACE_BUFFER_WRITTEN=1 prints, after every Populate, how many guest buffer
// elements the program may store to and how many it only loads (this call and cumulative), so the
// replay tool shows what a driver gains from DescriptorBinding::bufferWritten.
bool bufferWrittenTraceEnabled() {
    static const bool enabled = std::getenv("APS5_TRACE_BUFFER_WRITTEN") != nullptr;
    return enabled;
}
struct BufferWrittenCounts {
    std::atomic<unsigned long long> written{0};
    std::atomic<unsigned long long> readOnly{0};
};
BufferWrittenCounts bufferWrittenCounts;

DescriptorKind PhysicalKindFor(DescriptorBindingKind kind) {
    if (kind == DescriptorBindingKind::Samplers || kind == DescriptorBindingKind::SamplerTable) {
        return DescriptorKind::Sampler;
    }
    if (kind == DescriptorBindingKind::ImageTable) {
        return DescriptorKind::SampledImage;
    }
    const ImageResourceClass imageClass = ImageBindingResourceClass(kind);
    if (imageClass == ImageResourceClass::Sampled) {
        return DescriptorKind::SampledImage;
    }
    if (imageClass == ImageResourceClass::Storage) {
        return DescriptorKind::StorageImage;
    }
    return DescriptorKind::StorageBuffer;
}

DescriptorRole RoleFor(DescriptorBindingKind kind) {
    if (kind == DescriptorBindingKind::Buffers) {
        return DescriptorRole::GuestBuffers;
    }
    if (kind == DescriptorBindingKind::Samplers) {
        return DescriptorRole::GuestSamplers;
    }
    if (kind == DescriptorBindingKind::Gds) {
        return DescriptorRole::Gds;
    }
    if (kind == DescriptorBindingKind::BdaPagetable) {
        return DescriptorRole::BdaPagetable;
    }
    if (kind == DescriptorBindingKind::FaultBuffer) {
        return DescriptorRole::FaultBuffer;
    }
    if (kind == DescriptorBindingKind::FlattenedSrt) {
        return DescriptorRole::FlattenedSrt;
    }
    if (kind == DescriptorBindingKind::ShaderData) {
        return DescriptorRole::ShaderData;
    }
    if (kind == DescriptorBindingKind::ImageTable) {
        return DescriptorRole::ImageTable;
    }
    if (kind == DescriptorBindingKind::SamplerTable) {
        return DescriptorRole::SamplerTable;
    }
    if (kind == DescriptorBindingKind::ImageTableMap) {
        return DescriptorRole::ImageTableMap;
    }
    if (ImageBindingResourceClass(kind) != ImageResourceClass::None) {
        return DescriptorRole::GuestImages;
    }
    fail("DescriptorBindingBuilder::Populate binding kind has no descriptor role");
}

DescriptorImageShape ImageShapeForResource(const ImageResource& image) {
    const RdnaImageDimensionInfo& info = RdnaImageDimensionInfoFor(image.dimension);
    if (info.multisampled != 0u) {
        if (info.arrayed != 0u) {
            fail("DescriptorBindingBuilder::Populate multisampled array image resources have no descriptor image shape");
        }
        return DescriptorImageShape::Image2DMultisample;
    }
    if (info.spirvDimension == spv::Dim1D) {
        return info.arrayed != 0u ? DescriptorImageShape::Image1DArray : DescriptorImageShape::Image1D;
    }
    if (info.spirvDimension == spv::Dim3D) {
        return DescriptorImageShape::Image3D;
    }
    if (info.spirvDimension == spv::Dim2D) {
        // Cube images are declared and addressed as 2D arrays of faces (the backend converts
        // cube coordinates to face layers), so they bind as 2D arrays.
        if (image.cube && info.arrayed == 0u) {
            fail("DescriptorBindingBuilder::Populate cube image resource is not arrayed");
        }
        return info.arrayed != 0u ? DescriptorImageShape::Image2DArray : DescriptorImageShape::Image2D;
    }
    fail("DescriptorBindingBuilder::Populate image resource dimension has no descriptor image shape");
}

std::vector<std::uint32_t> GuestBuffersDescriptor(const std::vector<std::uint32_t>& resources, const ResourceSnapshot& snapshot) {
    std::vector<std::uint32_t> result;
    result.reserve(resources.size() * 4u);
    for (const std::uint32_t r : resources) {
        if (r >= snapshot.buffers.size()) {
            fail("DescriptorBindingBuilder::Populate guest buffer index is out of range");
        }
        const DescriptorValue& value = snapshot.buffers[r];
        if (value.dwordCount != 4u) {
            fail("DescriptorBindingBuilder::Populate guest buffer descriptor has an invalid width");
        }
        for (std::uint32_t dword = 0; dword < 4u; dword++) {
            result.push_back(value.dwords[dword]);
        }
    }
    return result;
}

std::vector<std::uint32_t> GuestSamplersDescriptor(const std::vector<std::uint32_t>& resources, const ResourceSnapshot& snapshot) {
    std::vector<std::uint32_t> result;
    std::uint32_t dwordCount = 0;
    for (std::size_t i = 0; i < resources.size(); i++) {
        const std::uint32_t r = resources[i];
        if (r >= snapshot.samplers.size()) {
            fail("DescriptorBindingBuilder::Populate guest sampler index is out of range");
        }
        const DescriptorValue& value = snapshot.samplers[r];
        if (value.dwordCount == 0u) {
            fail("DescriptorBindingBuilder::Populate guest sampler descriptor is empty");
        }
        if (i == 0u) {
            dwordCount = value.dwordCount;
        } else if (value.dwordCount != dwordCount) {
            fail("DescriptorBindingBuilder::Populate guest sampler descriptors have inconsistent widths");
        }
        for (std::uint32_t dword = 0; dword < value.dwordCount; dword++) {
            result.push_back(value.dwords[dword]);
        }
    }
    return result;
}

constexpr std::uint32_t ForceUnnormalizedBit = 1u << 15u;

[[noreturn]] void failUnnormalized(const char* reason) {
    fail(std::string("DescriptorBindingBuilder::Populate unnormalized guest sampler ") + reason + ", which is not implemented");
}

const char* UnnormalizedUseReason(std::uint32_t uses) {
    switch (uses & (~uses + 1u)) {
        case SamplerUseImplicitLod: return "is used by an implicit-LOD sample";
        case SamplerUseGradient: return "is used by a sample with derivatives";
        case SamplerUseCompare: return "is used with depth comparison";
        case SamplerUseGather: return "is used by a gather";
        case SamplerUseQueryLod: return "is used by image_get_lod";
        default: return "is used by an image_sample_*_a variant";
    }
}

struct UnnormalizedProof {
    std::vector<bool> samplers;
    std::vector<bool> images;
    std::vector<bool> imagesAtBaseLevel;
};

UnnormalizedProof ProveUnnormalized(const ShaderInfo& info, const ResourceSnapshot& snapshot) {
    UnnormalizedProof proof{std::vector<bool>(info.samplers.size()), std::vector<bool>(info.images.size()), std::vector<bool>(info.images.size())};
    for (std::uint32_t r = 0; r < info.samplers.size(); r++) {
        if (snapshot.samplers.at(r).dwordCount != 4u) fail("sampler descriptor must contain four dwords");
        const auto& sampler = info.samplers[r];
        if (sampler.table != NoTable) continue;
        for (const auto& pair : info.sampledPairs) {
            if (pair.sampler != r) continue;
            const auto& image = info.images.at(pair.image);
            if (!image.singleMipPlaneBiasCompatible || image.dimension != RdnaImageDimension::Dim2D) continue;
            const auto mode = ResourceMaterializer::RuntimeImageMode(image, snapshot.images.at(pair.image), info.runtimeImageModes.at(pair.image));
            if (info.runtimeImageModes.at(pair.image).at(mode).dimension != RdnaImageDimension::Dim3D) continue;
            const auto& words = snapshot.samplers.at(r).dwords;
            if ((words[0] & ForceUnnormalizedBit) != 0u || ((words[0] >> 29u) & 3u) > 2u || ((words[2] >> 20u) & 0xfu) != 0u || ((words[2] >> 26u) & 3u) > 1u) fail("2D biased sampling of a one-slice volume requires a normalized point sampler");
        }
        if ((snapshot.samplers[r].dwords[0] & ForceUnnormalizedBit) == 0u) {
            if (sampler.foldTexelOffsetsOnly) {
                fail("DescriptorBindingBuilder::Populate normalized guest sampler is used with a texel offset that is not a constant, which image sampling takes only with VK_KHR_maintenance8 and shaderImageGatherExtended");
            }
            continue;
        }
        const std::uint32_t unsupported = sampler.uses & ~static_cast<std::uint32_t>(SamplerUseExplicitLod | SamplerUseOffset | SamplerUseNonZeroLod);
        if (unsupported != 0u && !SamplesBaseLevelIfUnnormalized(sampler)) {
            failUnnormalized(UnnormalizedUseReason(unsupported));
        }
        if (sampler.depthCompare) {
            failUnnormalized("is used with depth comparison");
        }
        for (const auto& pair : info.sampledPairs) {
            if (pair.sampler != r) {
                continue;
            }
            const auto& base = info.images.at(pair.image);
            if (base.table != NoTable) {
                failUnnormalized("samples an image selected at run time");
            }
            const auto& image = info.runtimeImageModes.at(pair.image).at(ResourceMaterializer::RuntimeImageMode(base, snapshot.images.at(pair.image), info.runtimeImageModes.at(pair.image)));
            if (const auto* reason = ResourceMaterializer::UnnormalizedSampleMismatch(image)) {
                failUnnormalized(reason);
            }
            if (image.constantSwizzle) continue;
            proof.images[pair.image] = true;
        }
        proof.samplers[r] = true;
    }
    for (std::uint32_t resource = 0; resource < info.images.size(); resource++) {
        bool exact = proof.images[resource];
        for (const auto& pair : info.sampledPairs) {
            if (!exact) break;
            if (pair.image != resource) continue;
            const auto& words = snapshot.samplers.at(pair.sampler).dwords;
            exact = proof.samplers[pair.sampler] && info.samplers.at(pair.sampler).uses == SamplerUseExplicitLod && (words[1] & 0xfffu) == 0u && (words[2] & 0x3fffu) == 0u;
        }
        proof.imagesAtBaseLevel[resource] = exact;
    }
    return proof;
}

std::vector<std::uint32_t> ShaderDataDwordsFor(const IrBindingLayout& layout, std::uint32_t userDataBase, const ResourceSnapshot& snapshot, const std::array<std::uint32_t, 3>& partialThreads, const std::vector<PreparedImageMetadata>& imageMetadata, std::span<const std::uint32_t> runtimeState) {
    std::vector<std::uint32_t> result(layout.ShaderDataDwords(), 0u);
    for (std::size_t i = 0; i < layout.userDataRegisters.size(); ++i) {
        const auto reg = layout.userDataRegisters[i];
        if (reg < userDataBase || reg - userDataBase >= snapshot.userData.size()) fail("shader user-data register is out of range");
        result[i] = snapshot.userData[reg - userDataBase];
    }
    if (layout.dispatchThreadLimit) {
        if (partialThreads == std::array<std::uint32_t, 3>{}) fail("partial-group shader has no dispatch size");
        std::copy(partialThreads.begin(), partialThreads.end(), result.begin() + layout.DispatchThreadLimitDword());
    }
    for (const auto& entry : imageMetadata) {
        auto metadata = entry.metadata;
        const auto& descriptor = snapshot.images.at(entry.resource);
        if (descriptor.dwordCount != metadata.descriptor.size()) fail("invalid runtime image descriptor width");
        std::copy_n(descriptor.dwords.begin(), metadata.descriptor.size(), metadata.descriptor.begin());
        std::memcpy(result.data() + entry.offset, &metadata, sizeof(metadata));
    }
    if (runtimeState.size() != layout.runtimeStateDwords) fail("runtime state does not match its shader data layout");
    std::copy(runtimeState.begin(), runtimeState.end(), result.begin() + layout.RuntimeStateDword());
    return result;
}

}

bool ReducesBetweenTexels(std::uint32_t word0, std::uint32_t filter) {
    return ((word0 >> 29u) & 3u) != 0u && (((filter >> 20u) & 0xfu) != 0u || ((filter >> 26u) & 3u) == 2u);
}

std::uint32_t PointFilteredSamplerWord(std::uint32_t word0, std::uint32_t filter) {
    if (ReducesBetweenTexels(word0, filter)) {
        fail("DescriptorBindingBuilder: a min or max reduction sampler that filters between texels or mip levels samples an image that needs point filtering (sint, converted or depth-bits format), which is not implemented");
    }
    const bool mipmapped = ((filter >> 26u) & 3u) != 0u;
    return (filter & ~(0xffu << 20u)) | (1u << 24u) | (mipmapped ? 1u << 26u : 0u);
}

void DescriptorBindingBuilder::Populate(BindingAllocationResult& allocation, const IrProgram& program, const ResourceSnapshot& snapshot, const std::array<std::uint32_t, 3>& partialThreads, std::span<const std::uint8_t> exportMappings) const {
    Populate(allocation, program.Info(), program.Resources().stage, program.Resources().userDataBase, snapshot, partialThreads, exportMappings);
}

void DescriptorBindingBuilder::Populate(BindingAllocationResult& allocation, const ShaderInfo& info, IrShaderStage stage, std::uint32_t userDataBase, const ResourceSnapshot& snapshot, const std::array<std::uint32_t, 3>& partialThreads, std::span<const std::uint8_t> exportMappings) const {
    auto plan = Prepare(allocation.layout, info, stage, snapshot, exportMappings);
    const auto tables = ResourceMaterializer::ResolveImageTables(info, snapshot);
    Populate(allocation, allocation, plan, userDataBase, snapshot, partialThreads, &tables);
    allocation.specialization = std::move(plan.specialization);
}

DescriptorBindingPlan DescriptorBindingBuilder::Prepare(const IrBindingLayout& layout, const ShaderInfo& info, IrShaderStage stage, const ResourceSnapshot& snapshot, std::span<const std::uint8_t> exportMappings, bool registrationModules, bool fragmentDescriptorSet) const {
    if (stage == IrShaderStage::Pixel && exportMappings.size() != 8u) fail("fragment runtime export mappings are missing");
    if (info.images.size() > ShaderInfo::MaxImages) fail("runtime image count exceeds the static capacity");
    DescriptorBindingPlan plan;
    // Every draw builds this list: its size is known up front (buffers, images, compare states,
    // export mappings), so it is reserved once instead of regrowing.
    plan.specialization.reserve(info.buffers.size() * 4u + info.images.size() * (5u + 1u + PipelineSpecialization::CompareWords) + exportMappings.size() * 4u);
    const auto unnormalized = ProveUnnormalized(info, snapshot);
    for (std::uint32_t index = 0; index < info.samplers.size(); ++index) {
        if (FoldsTexelOffsets(info.samplers[index])) plan.specialization.push_back({PipelineSpecialization::FoldOffsetBase + index, unnormalized.samplers[index] ? 1u : 0u});
        if (SamplesBaseLevelIfUnnormalized(info.samplers[index])) plan.specialization.push_back({PipelineSpecialization::UnnormalizedBaseLevelBase + index, unnormalized.samplers[index] ? 1u : 0u});
    }
    std::vector<std::uint32_t> compareStates(info.images.size());
    for (std::uint32_t index = 0; index < info.images.size(); ++index) {
        if (!info.images[index].depthCompare) continue;
        const auto compare = compareStates[index] = ResourceMaterializer::EmulatedCompareState(info, snapshot, index);
        const auto first = PipelineSpecialization::CompareBase + index * PipelineSpecialization::CompareWords;
        const std::array<std::uint32_t, 6> values{EmulatedCompare::Function(compare), (compare & EmulatedCompare::Linear) != 0u, EmulatedCompare::AddressX(compare), EmulatedCompare::AddressY(compare), EmulatedCompare::Reference(compare), (compare & EmulatedCompare::BorderWhite) != 0u};
        for (std::uint32_t word = 0; word < values.size() && !registrationModules; ++word) plan.specialization.push_back({first + word, values[word]});
    }
    std::vector<std::uint32_t> imageModes(info.images.size());
    for (std::size_t index = 0; index < info.images.size(); ++index) {
        if (info.images[index].table == NoTable) imageModes[index] = ResourceMaterializer::RuntimeImageMode(info.images[index], snapshot.images.at(index), info.runtimeImageModes.at(index));
    }
    for (std::uint32_t index = 0; index < info.buffers.size(); ++index) {
        const auto& descriptor = snapshot.buffers.at(index);
        if (descriptor.dwordCount != 4u) fail("buffer specialization requires four descriptor words");
        const auto& buffer = info.buffers[index];
        const auto format = (descriptor.dwords[3] >> 12u) & 0x7fu;
        const bool present = descriptor.dwords[2] != 0u && (descriptor.dwords[0] != 0u || (descriptor.dwords[1] & 0xffffu) != 0u);
        if (buffer.descriptorFormatted && (format > static_cast<std::uint32_t>(IrBufferFormat::Format32_32_32_32Float) || (present && format == 0u))) fail("buffer specialization has an unsupported format");
        const auto misalignment = present ? descriptor.dwords[0] & 3u : 0u;
        if (buffer.atomic && misalignment != 0u) fail("buffer atomic on a V# whose base is not DWORD aligned");
        auto alignment = static_cast<std::uint32_t>(buffer.typedAlignment);
        if (buffer.descriptorFormatted && format != 0u) alignment = std::max(alignment, std::min(GetFormatInfo(static_cast<IrBufferFormat>(format)).byteSize, 4u));
        if (alignment == 0u || misalignment % alignment != 0u) fail("is a formatted buffer access through a V# whose base is not aligned to its element");
        if (registrationModules && buffer.descriptorFormatted) plan.specialization.push_back({PipelineSpecialization::BufferBase + index * PipelineSpecialization::BufferWords + 1u, descriptor.dwords[3]});
        if (registrationModules) plan.specialization.push_back({PipelineSpecialization::BufferBase + index * PipelineSpecialization::BufferWords + 3u, misalignment});
        if (!registrationModules) {
            const auto first = PipelineSpecialization::BufferBase + index * PipelineSpecialization::BufferWords;
            plan.specialization.push_back({first, descriptor.dwords[1] & 0xffff0000u});
            plan.specialization.push_back({first + 1u, descriptor.dwords[3]});
            plan.specialization.push_back({first + 2u, present ? 1u : 0u});
            plan.specialization.push_back({first + 3u, misalignment});
        }
        if (buffer.formattedReadMask != 0u) {
            for (std::uint32_t component = 0; component < 4u; ++component) {
                if ((buffer.formattedReadMask & (1u << component)) == 0u) continue;
                const auto selector = (descriptor.dwords[3] >> (component * 3u)) & 7u;
                if (selector == 2u || selector == 3u) fail("buffer specialization has a reserved component selector");
            }
        }
    }
    for (std::uint32_t index = 0; index < info.images.size() && !registrationModules; ++index) {
        if (info.images[index].table != NoTable) continue;
        const auto first = PipelineSpecialization::ImageBase + index * PipelineSpecialization::ImageWords;
        plan.specialization.push_back({first, imageModes[index]});
        for (std::uint32_t component = 0; component < 4u; ++component) plan.specialization.push_back({first + 1u + component, (snapshot.images.at(index).dwords[3] >> (component * 3u)) & 7u});
    }
    for (std::uint32_t target = 0; target < exportMappings.size() && !registrationModules; ++target) {
        for (std::uint32_t component = 0; component < 4u; ++component) plan.specialization.push_back({PipelineSpecialization::ExportBase + target * 4u + component, (exportMappings[target] >> (component * 2u)) & 3u});
    }
    std::vector<std::uint32_t> samplerModes(info.samplers.size(), 0u);
    std::vector<std::uint32_t> samplerIntegerModes(info.samplers.size(), 0u);
    std::vector<std::uint32_t> samplerFloatModes(info.samplers.size(), 0u);
    std::vector<std::uint32_t> directSamplerModes(info.samplers.size(), 0u);
    const auto samplerMode = [](const ImageResource& image) { return !image.constantSwizzle && (image.numericClass == IrTextureNumericClass::Sint || image.conversionFormat != IrBufferFormat::Invalid || image.depthBits) ? 2u : 1u; };
    const auto pairSampler = [&](std::uint32_t sampler, const ImageResource& image) {
        const auto mode = samplerMode(image);
        samplerModes.at(sampler) |= mode;
        (image.numericClass == IrTextureNumericClass::Uint || image.numericClass == IrTextureNumericClass::Sint ? samplerIntegerModes : samplerFloatModes).at(sampler) |= mode;
    };
    for (const auto& pair : info.sampledPairs) {
        const auto& image = info.images.at(pair.image);
        if (image.table == NoTable) {
            const auto mode = samplerMode(info.runtimeImageModes.at(pair.image).at(imageModes[pair.image]));
            pairSampler(pair.sampler, info.runtimeImageModes.at(pair.image).at(imageModes[pair.image]));
            directSamplerModes.at(pair.sampler) |= mode;
        } else {
            samplerModes.at(pair.sampler) |= 3u;
        }
    }
    for (std::uint32_t index = 0; index < info.images.size(); ++index) {
        if (info.images[index].mipMode != ImageMipMode::DynamicStorage) continue;
        const auto word = snapshot.images.at(index).dwords[3];
        const auto first = (word >> 12u) & 0xfu;
        const auto last = (word >> 16u) & 0xfu;
        if (last < first || last - first >= RuntimeAbi::StorageMipSlots) fail("invalid dynamic storage mip range");
        if (!registrationModules) plan.specialization.push_back({PipelineSpecialization::MipCountBase + index, last - first + 1u});
    }
    plan.bindings.reserve(layout.descriptors.size());
    for (const auto& logical : layout.descriptors) {
        const bool imageHeap = ImageBindingResourceClass(logical.kind) != ImageResourceClass::None;
        const bool samplerHeap = logical.kind == DescriptorBindingKind::Samplers;
        IrDescriptorBinding compact{logical.kind, {}};
        std::vector<std::uint32_t> originals;
        std::vector<bool> inactive;
        std::uint32_t previous = UINT32_MAX;
        std::uint32_t mip = 0;
        for (std::uint32_t element = 0; element < logical.resources.size(); ++element) {
            const auto resource = logical.resources[element];
            bool active = true;
            if (imageHeap) {
                const auto& image = info.images.at(resource);
                if (image.table != NoTable) fail("an image table resource was placed in a typed heap");
                const auto& mode = info.runtimeImageModes.at(resource).at(imageModes[resource]);
                mip = previous == resource ? mip + 1u : 0u;
                previous = resource;
                {
                    const auto word = snapshot.images.at(resource).dwords[3];
                    active = DescriptorBindingForImage(mode) == logical.kind && mode.packedFormat != IrBufferFormat::Fmask8_S2_F1 && !mode.constantSwizzle && (image.mipMode != ImageMipMode::DynamicStorage || mip <= ((word >> 16u) & 0xfu) - ((word >> 12u) & 0xfu));
                }
            } else if (samplerHeap) {
                active = (samplerModes.at(resource) & (1u << (element & 1u))) != 0u;
            }
            if ((imageHeap || samplerHeap) && !registrationModules) plan.specialization.push_back({PipelineSpecialization::DescriptorIndex(static_cast<std::uint32_t>(logical.kind), element), active ? static_cast<std::uint32_t>(compact.resources.size()) : 0u});
            if (active || ((imageHeap || samplerHeap) && registrationModules)) {
                compact.resources.push_back(resource);
                originals.push_back(element);
                inactive.push_back(!active);
            }
        }
        if ((imageHeap || samplerHeap) && !registrationModules) plan.specialization.push_back({PipelineSpecialization::HeapCountBase + static_cast<std::uint32_t>(logical.kind), std::max(1u, static_cast<std::uint32_t>(compact.resources.size()))});
        if ((imageHeap || samplerHeap) && compact.resources.empty()) continue;
        PreparedDescriptorBinding entry{};
        auto& physical = entry.descriptor;
        physical.descriptorSet = fragmentDescriptorSet && stage == IrShaderStage::Pixel ? RuntimeAbi::FragmentDescriptorSet : RuntimeAbi::DescriptorSet;
        physical.binding = NativeBinding(stage, compact.kind);
        physical.count = compact.resources.empty() ? 1u : static_cast<std::uint32_t>(compact.resources.size());
        physical.kind = PhysicalKindFor(compact.kind);
        physical.role = RoleFor(compact.kind);
        if (physical.role == DescriptorRole::GuestBuffers) {
            for (const auto resource : compact.resources) {
                const auto& buffer = info.buffers.at(resource);
                physical.bufferAtomic.push_back(buffer.atomic);
                physical.bufferWritten.push_back(buffer.written || buffer.atomic);
                physical.bufferRead.push_back(buffer.read);
                if (buffer.written || buffer.atomic) ++entry.writtenBuffers;
                else ++entry.readOnlyBuffers;
            }
        } else if (physical.role == DescriptorRole::GuestImages) {
            const auto& modes = info.runtimeImageModes.at(compact.resources.front());
            const auto shape = std::ranges::find_if(modes, [&](const ImageResource& mode) { return DescriptorBindingForImage(mode) == compact.kind; });
            if (shape == modes.end()) fail("runtime image heap has no static image type");
            physical.imageShape = ImageShapeForResource(*shape);
            for (std::size_t element = 0; element < compact.resources.size(); ++element) {
                const auto resource = compact.resources[element];
                const auto& image = info.images.at(resource);
                const auto& mode = info.runtimeImageModes.at(resource).at(imageModes[resource]);
                entry.identityImageSwizzle.push_back(mode.conversionFormat != IrBufferFormat::Invalid || mode.depthBits);
                physical.imageWritten.push_back(image.written || image.atomic);
                physical.imageDepthCompare.push_back(mode.depthCompare);
                physical.imageAtomic.push_back(image.atomic);
                physical.imageUnnormalized.push_back(unnormalized.images.at(resource));
                physical.imageUnnormalizedBaseLevel.push_back(unnormalized.imagesAtBaseLevel.at(resource));
                physical.imageSamplers.push_back(0u);
                physical.imageAtomic64.push_back(image.atomic64);
            }
        } else if (physical.role == DescriptorRole::GuestSamplers) {
            for (std::uint32_t element = 0; element < compact.resources.size(); ++element) {
                const auto resource = compact.resources[element];
                bool compare = false;
                for (const auto& pair : info.sampledPairs) if (pair.sampler == resource) compare |= info.images.at(pair.image).depthCompare && compareStates.at(pair.image) == 0u;
                physical.samplerDepthCompare.push_back(compare);
                physical.samplerUnnormalized.push_back(unnormalized.samplers.at(resource));
                const auto mode = 1u << (originals[element] & 1u);
                physical.samplerIntegerBorder.push_back((samplerIntegerModes.at(resource) & mode) != 0u && (samplerFloatModes.at(resource) & mode) == 0u);
                const auto& words = snapshot.samplers.at(resource).dwords;
                const bool tablePointOnly = (directSamplerModes.at(resource) & 2u) == 0u;
                if ((originals[element] & 1u) != 0u && !inactive[element] && !(tablePointOnly && ReducesBetweenTexels(words[0], words[2]))) entry.samplerFilterElements.push_back(element);
            }
        } else if (physical.role == DescriptorRole::ShaderData && layout.UsesPushData()) {
            fail("DescriptorBindingBuilder::Populate shader-data binding must not exist when push data is used");
        }
        plan.storageData |= physical.role == DescriptorRole::ShaderData;
        entry.resources = std::move(compact.resources);
        if (registrationModules && (imageHeap || samplerHeap)) entry.inactive = std::move(inactive);
        plan.bindings.push_back(std::move(entry));
    }
    for (std::uint32_t resource = 0; resource < info.images.size() && registrationModules && layout.runtimeImageCount != 0u; ++resource) {
        if (info.images[resource].table != NoTable) continue;
        if (resource >= layout.runtimeImageCount) fail("runtime image metadata exceeds compact layout");
        const auto& descriptor = snapshot.images.at(resource);
        RuntimeAbi::ResourceMetadata metadata{};
        if (descriptor.dwordCount != metadata.descriptor.size()) fail("invalid runtime image descriptor width");
        metadata.elementCount = 1u;
        if (imageModes[resource] > RuntimeAbi::ImageModeMask) fail("runtime image mode exceeds its metadata field");
        metadata.flags = imageModes[resource] << 1u | (registrationModules ? (compareStates[resource] & RuntimeAbi::CompareStateMask) << RuntimeAbi::CompareStateShift : 0u);
        if (descriptor.dwords[0] == 0u && (descriptor.dwords[1] & 0xffu) == 0u) metadata.flags |= 1u;
        plan.imageMetadata.push_back({resource, layout.ImageMetadataDword() + resource * static_cast<std::uint32_t>(sizeof(metadata) / sizeof(std::uint32_t)), metadata});
    }
    if (layout.runtimeStateDwords != 0u) {
        plan.runtimeState.assign(layout.runtimeStateDwords, 0u);
        for (std::uint32_t target = 0; target < exportMappings.size(); ++target) plan.runtimeState.at(RuntimeAbi::StateExportMappings + target / 4u) |= static_cast<std::uint32_t>(exportMappings[target]) << (target % 4u * 8u);
    }
    std::vector<std::uint32_t> samplerMasks(info.samplers.size());
    for (const auto& binding : plan.bindings) {
        if (binding.descriptor.role != DescriptorRole::GuestSamplers) continue;
        if (binding.resources.size() > ShaderInfo::MaxSamplers) fail("prepared sampler mask capacity exceeded");
        for (std::uint32_t element = 0; element < binding.resources.size(); ++element) {
            if (binding.inactive.empty() || !binding.inactive[element]) samplerMasks.at(binding.resources[element]) |= 1u << element;
        }
    }
    for (auto& binding : plan.bindings) {
        if (binding.descriptor.role != DescriptorRole::GuestImages) continue;
        for (std::size_t element = 0; element < binding.resources.size(); ++element) {
            const auto resource = binding.resources[element];
            for (const auto& pair : info.sampledPairs) {
                if (pair.image == resource) binding.descriptor.imageSamplers.Mutable()[element] |= samplerMasks.at(pair.sampler);
            }
        }
    }
    for (auto& binding : plan.bindings) {
        if (binding.descriptor.role == DescriptorRole::ImageTable) binding.tableSamplerMasks = samplerMasks;
    }
    return plan;
}

DescriptorBindingPlan DescriptorBindingBuilder::Select(const DescriptorBindingPlan& plan, std::span<const std::uint32_t> bindings) const {
    DescriptorBindingPlan selected;
    std::set<std::uint32_t> remaining(bindings.begin(), bindings.end());
    selected.bindings.reserve(remaining.size());
    for (const auto& binding : plan.bindings) {
        if (remaining.erase(binding.descriptor.binding) == 0u) continue;
        selected.storageData |= binding.descriptor.role == DescriptorRole::ShaderData;
        selected.bindings.push_back(binding);
    }
    if (!remaining.empty()) fail("specialized module bindings disagree with the prepared plan");
    const bool hasSamplers = std::any_of(selected.bindings.begin(), selected.bindings.end(), [](const auto& binding) { return binding.descriptor.role == DescriptorRole::GuestSamplers; });
    if (!hasSamplers) {
        for (auto& binding : selected.bindings) {
            auto& samplers = binding.descriptor.imageSamplers.Mutable();
            std::fill(samplers.begin(), samplers.end(), 0u);
            binding.tableSamplerMasks.clear();
        }
    }
    selected.imageMetadata = plan.imageMetadata;
    selected.runtimeState = plan.runtimeState;
    return selected;
}

bool DescriptorBindingBuilder::PlanHasTables(const DescriptorBindingPlan& plan) {
    return std::any_of(plan.bindings.begin(), plan.bindings.end(), [](const PreparedDescriptorBinding& binding) {
        const auto role = binding.descriptor.role;
        return role == DescriptorRole::ImageTable || role == DescriptorRole::SamplerTable || role == DescriptorRole::ImageTableMap;
    });
}

void DescriptorBindingBuilder::Populate(BindingAllocationResult& allocation, const CompiledBindingLayout& compiled, const DescriptorBindingPlan& plan, std::uint32_t userDataBase, const ResourceSnapshot& snapshot, const std::array<std::uint32_t, 3>& partialThreads, const ResolvedImageTables* tables, std::uint32_t bufferOffsetAlignment) const {
    std::vector<std::uint32_t> words;
    std::vector<std::uint32_t> offsets;
    std::vector<std::byte> push;
    PopulateWords(words, offsets, push, compiled, plan, userDataBase, snapshot, partialThreads, tables, bufferOffsetAlignment);
    std::vector<DescriptorBinding> bindings;
    bindings.reserve(plan.bindings.size());
    for (std::size_t index = 0; index < plan.bindings.size(); ++index) {
        const auto& entry = plan.bindings[index];
        auto physical = entry.descriptor;
        physical.guestDescriptor.assign(words.begin() + offsets[index], words.begin() + offsets[index + 1]);
        // Table bindings: the elements (and so their flags and count) are this invocation's.
        if (physical.role == DescriptorRole::ImageTable) {
            std::vector<DescriptorImageShape> shapes;
            std::vector<bool> depthCompare;
            std::vector<std::uint32_t> imageSamplers;
            shapes.reserve(tables->elements.size());
            depthCompare.reserve(tables->elements.size());
            imageSamplers.reserve(tables->elements.size());
            for (const auto& element : tables->elements) {
                ImageResource shape;
                shape.dimension = element.dimension;
                shapes.push_back(ImageShapeForResource(shape));
                depthCompare.push_back(element.depthCompare);
                std::uint32_t samplers = 0;
                for (std::uint32_t sampler = 0; sampler < entry.tableSamplerMasks.size(); ++sampler) {
                    if ((element.samplers & (1u << sampler)) != 0u) samplers |= entry.tableSamplerMasks[sampler];
                }
                imageSamplers.push_back(samplers);
            }
            physical.imageShapes = std::move(shapes);
            physical.imageDepthCompare = std::move(depthCompare);
            physical.imageWritten = std::vector<bool>(tables->elements.size(), false);
            physical.imageAtomic = std::vector<bool>(tables->elements.size(), false);
            physical.imageSamplers = std::move(imageSamplers);
            physical.count = static_cast<std::uint32_t>(tables->elements.size());
        } else if (physical.role == DescriptorRole::SamplerTable) {
            std::vector<bool> compare;
            compare.reserve(tables->samplers.size());
            for (const auto& element : tables->samplers) compare.push_back(element.compare);
            physical.samplerDepthCompare = std::move(compare);
            physical.count = static_cast<std::uint32_t>(tables->samplers.size());
        }
        bindings.push_back(std::move(physical));
    }
    allocation.bindings = std::move(bindings);
    allocation.pushConstants = std::move(push);
}

void DescriptorBindingBuilder::PopulateWords(std::vector<std::uint32_t>& words, std::vector<std::uint32_t>& offsets, std::vector<std::byte>& push, const CompiledBindingLayout& compiled, const DescriptorBindingPlan& plan, std::uint32_t userDataBase, const ResourceSnapshot& snapshot, const std::array<std::uint32_t, 3>& partialThreads, const ResolvedImageTables* tables, std::uint32_t bufferOffsetAlignment) const {
    const auto& layout = compiled.layout;
    const bool needsShaderData = layout.UsesPushData() || plan.storageData;
    auto shaderData = needsShaderData ? ShaderDataDwordsFor(layout, userDataBase, snapshot, partialThreads, plan.imageMetadata, plan.runtimeState) : std::vector<std::uint32_t>{};
    // Guest buffers bound below their base (a driver meeting the storage buffer offset alignment):
    // the shader reads one byte per buffer element from dword memoryOffsetDword on.
    if (bufferOffsetAlignment > 1u && !shaderData.empty()) {
        for (const auto& entry : plan.bindings) {
            if (entry.descriptor.role != DescriptorRole::GuestBuffers) continue;
            for (std::size_t element = 0; element < entry.resources.size() && element < layout.memoryOffsetCount; ++element) {
                const auto& value = snapshot.buffers.at(entry.resources[element]);
                const std::uint64_t base = value.dwords[0] | (static_cast<std::uint64_t>(value.dwords[1] & 0xffffu) << 32u);
                const auto misalignment = BufferViewMisalignment(base, bufferOffsetAlignment);
                if (misalignment == 0u) continue;
                auto& dword = shaderData.at(layout.memoryOffsetDword + static_cast<std::uint32_t>(element) / 4u);
                const auto shift = static_cast<std::uint32_t>(element % 4u) * 8u;
                dword = (dword & ~(0xffu << shift)) | (misalignment << shift);
            }
        }
    }
    words.clear();
    offsets.clear();
    offsets.reserve(plan.bindings.size() + 1u);
    offsets.push_back(0u);
    // The shader data binding takes the words; push data is then empty (as when it was moved out).
    bool shaderDataTaken = false;
    std::size_t writtenHere = 0;
    std::size_t readOnlyHere = 0;
    for (const auto& entry : plan.bindings) {
        const auto& physical = entry.descriptor;
        const auto first = words.size();
        switch (physical.role) {
        case DescriptorRole::GuestBuffers:
            for (const std::uint32_t r : entry.resources) {
                if (r >= snapshot.buffers.size()) fail("DescriptorBindingBuilder::Populate guest buffer index is out of range");
                const DescriptorValue& value = snapshot.buffers[r];
                if (value.dwordCount != 4u) fail("DescriptorBindingBuilder::Populate guest buffer descriptor has an invalid width");
                words.insert(words.end(), value.dwords.begin(), value.dwords.begin() + 4);
            }
            writtenHere += entry.writtenBuffers;
            readOnlyHere += entry.readOnlyBuffers;
            break;
        case DescriptorRole::GuestImages:
            for (std::size_t element = 0; element < entry.resources.size(); ++element) {
                const auto& descriptor = snapshot.images.at(entry.resources[element]);
                if (descriptor.dwordCount != 8u) fail("invalid runtime image descriptor width");
                if (!entry.inactive.empty() && entry.inactive[element]) {
                    words.insert(words.end(), 8u, 0u);
                    continue;
                }
                const auto offset = words.size();
                words.insert(words.end(), descriptor.dwords.begin(), descriptor.dwords.begin() + 8);
                if (entry.identityImageSwizzle[element]) words[offset + 3u] = (words[offset + 3u] & ~0xfffu) | ShaderImageIdentitySwizzle;
            }
            break;
        case DescriptorRole::GuestSamplers: {
            std::uint32_t dwordCount = 0;
            for (std::size_t i = 0; i < entry.resources.size(); i++) {
                const std::uint32_t r = entry.resources[i];
                if (r >= snapshot.samplers.size()) fail("DescriptorBindingBuilder::Populate guest sampler index is out of range");
                const DescriptorValue& value = snapshot.samplers[r];
                if (value.dwordCount == 0u) fail("DescriptorBindingBuilder::Populate guest sampler descriptor is empty");
                if (i == 0u) dwordCount = value.dwordCount;
                else if (value.dwordCount != dwordCount) fail("DescriptorBindingBuilder::Populate guest sampler descriptors have inconsistent widths");
                words.insert(words.end(), value.dwords.begin(), value.dwords.begin() + value.dwordCount);
            }
            for (const auto element : entry.samplerFilterElements) {
                const auto at = first + static_cast<std::size_t>(element) * 4u;
                if (at + 2u >= words.size()) fail("DescriptorBindingBuilder::Populate sampler filter element is out of range");
                words[at + 2u] = PointFilteredSamplerWord(words[at], words[at + 2u]);
            }
            break;
        }
        // Table bindings: Populate sets their per-element flags and count from the same tables.
        case DescriptorRole::ImageTable:
            if (tables == nullptr) fail("DescriptorBindingBuilder::Populate image table elements were not resolved");
            words.reserve(words.size() + tables->elements.size() * 8u);
            for (const auto& element : tables->elements) words.insert(words.end(), element.words.begin(), element.words.end());
            break;
        case DescriptorRole::SamplerTable:
            if (tables == nullptr) fail("DescriptorBindingBuilder::Populate sampler table elements were not resolved");
            words.reserve(words.size() + tables->samplers.size() * 4u);
            for (const auto& element : tables->samplers) {
                const auto at = words.size();
                words.insert(words.end(), element.words.begin(), element.words.end());
                if (element.point) words[at + 2u] = PointFilteredSamplerWord(words[at], words[at + 2u]);
            }
            break;
        case DescriptorRole::ImageTableMap:
            if (tables == nullptr || tables->map.empty()) fail("DescriptorBindingBuilder::Populate image table map was not resolved");
            words.insert(words.end(), tables->map.begin(), tables->map.end());
            break;
        case DescriptorRole::FlattenedSrt:
            if (snapshot.flattenedSrt.empty()) {
                fail("DescriptorBindingBuilder::Populate flattened SRT snapshot is empty");
            }
            words.insert(words.end(), snapshot.flattenedSrt.begin(), snapshot.flattenedSrt.end());
            break;
        case DescriptorRole::ShaderData:
            if (!shaderDataTaken) words.insert(words.end(), shaderData.begin(), shaderData.end());
            shaderDataTaken = true;
            break;
        case DescriptorRole::Gds:
        case DescriptorRole::BdaPagetable:
        case DescriptorRole::FaultBuffer:
            break;
        }

        if (physical.role == DescriptorRole::GuestBuffers || physical.role == DescriptorRole::GuestImages || physical.role == DescriptorRole::GuestSamplers) {
            const auto size = words.size() - first;
            if (physical.count == 0u || size % physical.count != 0u) {
                fail("DescriptorBindingBuilder::Populate guest descriptor size is not a multiple of the binding count");
            }
        }
        offsets.push_back(static_cast<std::uint32_t>(words.size()));
    }

    if (bufferWrittenTraceEnabled()) {
        // Cumulative counts include every Populate of the process (the replay tool populates
        // each program twice), so the per-call counts are the ones to sum per program.
        const auto written = bufferWrittenCounts.written.fetch_add(writtenHere) + writtenHere;
        const auto readOnly = bufferWrittenCounts.readOnly.fetch_add(readOnlyHere) + readOnlyHere;
        std::fprintf(stderr, "[bindings] guest buffer elements: %zu written, %zu read-only (total so far: %llu / %llu)\n", writtenHere, readOnlyHere, written, readOnly);
    }
    push.clear();
    if (layout.UsesPushData() && !shaderDataTaken) {
        push.resize(static_cast<std::size_t>(shaderData.size()) * sizeof(std::uint32_t));
        std::memcpy(push.data(), shaderData.data(), push.size());
    }
}

}
