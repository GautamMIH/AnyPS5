#include <algorithm>
#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <optional>
#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libc/include/General.hpp"
#include "Optimization/include/Optimization/ShaderStageInputInfo.hpp"
#include "IntermediateRepresentation/include/IntermediateRepresentation/IrMetadata/ShaderStage.hpp"
#include "ImageTableAbi.hpp"
#include "RdnaDecoder/include/RdnaDecoder/RdnaDescriptorFormat.hpp"
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <string>

namespace AgcDriver::Graphics {
namespace {

// The image allocation of a null storage image element (bound as a null descriptor).
constexpr std::size_t NullStorageImage = std::numeric_limits<std::size_t>::max();

bool overlap(std::uint64_t first, std::size_t firstSize, std::uint64_t second, std::size_t secondSize) {
    return first < second + secondSize && second < first + firstSize;
}

VkComponentSwizzle ComponentSwizzleFor(std::uint8_t dstSel) {
    switch (dstSel) {
        case 0: return VK_COMPONENT_SWIZZLE_ZERO;
        case 1: return VK_COMPONENT_SWIZZLE_ONE;
        case 4: return VK_COMPONENT_SWIZZLE_R;
        case 5: return VK_COMPONENT_SWIZZLE_G;
        case 6: return VK_COMPONENT_SWIZZLE_B;
        case 7: return VK_COMPONENT_SWIZZLE_A;
        default: throw std::runtime_error("AGC graphics: guest texture descriptor has an invalid destination channel selector " + std::to_string(dstSel));
    }
}

const char* roleName(ShaderRecompiler::DescriptorRole role) {
    switch (role) {
        case ShaderRecompiler::DescriptorRole::GuestBuffers: return "GuestBuffers";
        case ShaderRecompiler::DescriptorRole::GuestImages: return "GuestImages";
        case ShaderRecompiler::DescriptorRole::GuestSamplers: return "GuestSamplers";
        case ShaderRecompiler::DescriptorRole::Gds: return "Gds";
        case ShaderRecompiler::DescriptorRole::BdaPagetable: return "BdaPagetable";
        case ShaderRecompiler::DescriptorRole::FaultBuffer: return "FaultBuffer";
        case ShaderRecompiler::DescriptorRole::FlattenedSrt: return "FlattenedSrt";
        case ShaderRecompiler::DescriptorRole::ShaderData: return "ShaderData";
        case ShaderRecompiler::DescriptorRole::ImageTable: return "ImageTable";
        case ShaderRecompiler::DescriptorRole::SamplerTable: return "SamplerTable";
        case ShaderRecompiler::DescriptorRole::ImageTableMap: return "ImageTableMap";
    }
    throw std::runtime_error("AGC graphics: unknown descriptor role");
}

const char* kindName(ShaderRecompiler::DescriptorKind kind) {
    switch (kind) {
        case ShaderRecompiler::DescriptorKind::UniformBuffer: return "UniformBuffer";
        case ShaderRecompiler::DescriptorKind::StorageBuffer: return "StorageBuffer";
        case ShaderRecompiler::DescriptorKind::UniformTexelBuffer: return "UniformTexelBuffer";
        case ShaderRecompiler::DescriptorKind::StorageTexelBuffer: return "StorageTexelBuffer";
        case ShaderRecompiler::DescriptorKind::SampledImage: return "SampledImage";
        case ShaderRecompiler::DescriptorKind::StorageImage: return "StorageImage";
        case ShaderRecompiler::DescriptorKind::Sampler: return "Sampler";
    }
    throw std::runtime_error("AGC graphics: unknown descriptor kind");
}

// The stage a table fault names (the recompiler's IrShaderStage).
std::uint32_t IrStage(ShaderRecompiler::ShaderStage stage) {
    using Ir = ShaderRecompiler::IrShaderStage;
    switch (stage) {
        case ShaderRecompiler::ShaderStage::Compute: return static_cast<std::uint32_t>(Ir::Compute);
        case ShaderRecompiler::ShaderStage::Vertex: return static_cast<std::uint32_t>(Ir::Vertex);
        case ShaderRecompiler::ShaderStage::Fragment: return static_cast<std::uint32_t>(Ir::Pixel);
        case ShaderRecompiler::ShaderStage::Mesh: return static_cast<std::uint32_t>(Ir::Mesh);
        case ShaderRecompiler::ShaderStage::Local: return static_cast<std::uint32_t>(Ir::Local);
        case ShaderRecompiler::ShaderStage::TessellationControl: return static_cast<std::uint32_t>(Ir::TessellationControl);
        case ShaderRecompiler::ShaderStage::TessellationEvaluation: return static_cast<std::uint32_t>(Ir::TessellationEvaluation);
        case ShaderRecompiler::ShaderStage::Geometry: break;
    }
    return static_cast<std::uint32_t>(Ir::Unknown);
}

std::string HexText(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(value));
    return text;
}

// The runtime image metadata of the shader data (bindless images, ShaderRecompiler::RuntimeAbi):
// every entry names a heap the shader binds and elements within it, as upstream's driver checks.
void ValidateRuntimeResources(const ShaderRecompiler::RecompileResult& program, std::span<const std::uint32_t> words) {
    constexpr auto bindingCount = static_cast<std::uint32_t>(ShaderRecompiler::RuntimeAbi::Binding::Count);
    std::array<const ShaderRecompiler::DescriptorBinding*, bindingCount> heaps{};
    if (program.runtimeImageCount != 0u) {
        for (const auto& binding : program.bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages) continue;
            auto& heap = heaps[binding.binding % bindingCount];
            Require(heap == nullptr, "duplicate runtime image binding");
            heap = &binding;
        }
    }
    Require(words.size() == program.shaderDataDwords, "invalid compact shader data size");
    constexpr auto metadataWords = sizeof(ShaderRecompiler::RuntimeAbi::ResourceMetadata) / sizeof(std::uint32_t);
    Require(program.imageMetadataDword <= words.size() && program.runtimeImageCount <= (words.size() - program.imageMetadataDword) / metadataWords, "runtime image metadata exceeds shader data");
    for (const auto index : program.runtimeImageResources) {
        Require(index < program.runtimeImageCount, "runtime image resource exceeds its compact layout");
        const auto offset = program.imageMetadataDword + index * metadataWords;
        const auto kind = words[offset];
        const auto first = words[offset + 1u];
        const auto count = words[offset + 2u];
        Require(count != 0u, "runtime image metadata has no descriptor elements");
        Require(kind < heaps.size() && heaps[kind] != nullptr, "runtime metadata references an unbound image heap");
        Require(first < heaps[kind]->count && count <= heaps[kind]->count - first, "runtime metadata exceeds its bound heap");
    }
}

}

ShaderResources::ShaderResources(const Context& context, const ShaderRecompiler::RecompileResult& vertex, const ShaderRecompiler::RecompileResult& fragment, std::span<const ColorTarget> targets, std::uint64_t indexAddress, std::size_t indexBytes) : ShaderResources(context, std::array<CompiledShader, 2>{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &fragment, static_cast<std::uint32_t>(vertex.pushConstants.size())}}}, targets, indexAddress, indexBytes) {}

ShaderResources::ShaderResources(const Context& context, std::span<const CompiledShader> shaders, std::span<const ColorTarget> targets, std::uint64_t indexAddress, std::size_t indexBytes, std::span<const GuestMemorySnapshot> snapshots) : context(context), guestMemory(context) {
    prepareAddressBindings(shaders, snapshots);
    // Mesh stages fetch the draw's indices by address, so the index buffer must be addressable
    // whatever allocation it lives in.
    if (usesBda && indexBytes != 0) {
        std::vector<std::byte> indices(indexBytes);
        GuestMemory::Read(indexAddress, indices, 1);
        guestMemory.AddSnapshot({indexAddress, indices});
    }
    build(shaders, targets, indexAddress, indexBytes);
}

ShaderResources::ShaderResources(const Context& context, const CompiledShader& compute, std::span<const GuestMemorySnapshot> snapshots) : context(context), guestMemory(context) {
    Require(compute.stage == ShaderRecompiler::ShaderStage::Compute, "compute resources require a compute shader");
    prepareAddressBindings(std::span<const CompiledShader>(&compute, 1), snapshots);
    build(std::span<const CompiledShader>(&compute, 1), {}, 0, 0);
}

void ShaderResources::build(std::span<const CompiledShader> shaders, std::span<const ColorTarget> targets, std::uint64_t indexAddress, std::size_t indexBytes) {
    PerformanceTimer timing("Graphics.ShaderResources");
    try {
        Require(!shaders.empty() && context.limits.maxBoundDescriptorSets >= 1, "shader descriptor set exceeds device limits");
        std::vector<Binding> bindings;
        std::set<std::uint32_t> occupied;
        std::uint64_t storageBuffers = 0;
        for (const auto& shader : shaders) {
            Require(shader.program != nullptr, "missing compiled shader");
            const VkShaderStageFlags flags = VulkanStage(shader.stage);
            std::uint64_t stageDescriptors = 0;
            const auto firstSampler = samplers.size();
            pairedSamplers.clear();
            for (std::size_t index = 0; index < shader.program->bindings.size(); ++index) {
                const auto& binding = shader.program->bindings[index];
                const auto words = shader.Words(index);
                Require(binding.descriptorSet == 0, "unexpected descriptor set: every shader resource must use descriptor set zero");
                ShaderRecompiler::RuntimeAbi::RequireVersion(shader.program->runtimeAbiVersion);
                // Every stage's image table shares one binding (RuntimeAbi::ImageTableBinding), each
                // at its element base: resolved once the stage's samplers are bound.
                if (binding.role == ShaderRecompiler::DescriptorRole::ImageTable) {
                    Require(binding.binding == ShaderRecompiler::RuntimeAbi::ImageTableBinding && binding.kind == ShaderRecompiler::DescriptorKind::SampledImage && words.size() == static_cast<std::size_t>(binding.count) * 8u && binding.imageShapes.size() == binding.count && binding.imageDepthCompare.size() == binding.count && binding.imageSamplers.size() == binding.count, "image table binding is malformed");
                    auto& table = tableStage(shader);
                    Require(table.images == nullptr, "a stage binds two image tables");
                    table.images = &binding;
                    table.imageWords = words;
                    continue;
                }
                Require(occupied.insert(binding.binding).second, "duplicate shader binding");
                if (binding.role == ShaderRecompiler::DescriptorRole::SamplerTable) {
                    addSamplerTable(binding, words, flags, tableStage(shader), bindings);
                    continue;
                }
                if (binding.role == ShaderRecompiler::DescriptorRole::ImageTableMap) {
                    Require(binding.kind == ShaderRecompiler::DescriptorKind::StorageBuffer && binding.count == 1 && words.size() >= ShaderRecompiler::ImageTableAbi::HeaderWords && !binding.readOnly && words[ShaderRecompiler::ImageTableAbi::HeaderVersion] == ShaderRecompiler::ImageTableAbi::Version, "image table map binding is malformed");
                    Require(words.size() * sizeof(std::uint32_t) <= context.limits.maxStorageBufferRange, "image table map exceeds the storage buffer range limit");
                    stageDescriptors += 1;
                    storageBuffers += 1;
                    Require(stageDescriptors <= context.limits.maxPerStageDescriptorStorageBuffers && stageDescriptors <= context.limits.maxPerStageResources, "shader descriptors exceed per-stage limits");
                    auto& table = tableStage(shader);
                    Require(table.mapBinding == NoElement, "a stage binds two image table maps");
                    table.map.assign(words.begin(), words.end());
                    // Its buffer is made once the driver filled in the element base and fault codes.
                    table.mapBinding = bindings.size();
                    bindings.push_back({{binding.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags, nullptr}, {}, {}});
                    continue;
                }
                const bool addressRole = binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable || binding.role == ShaderRecompiler::DescriptorRole::FaultBuffer;
                const bool bufferRole = addressRole || binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers || binding.role == ShaderRecompiler::DescriptorRole::ShaderData || binding.role == ShaderRecompiler::DescriptorRole::FlattenedSrt || binding.role == ShaderRecompiler::DescriptorRole::Gds;
                const bool imageRole = binding.role == ShaderRecompiler::DescriptorRole::GuestImages || binding.role == ShaderRecompiler::DescriptorRole::GuestSamplers;
                if (imageRole) {
                    addImageBinding(binding, words, flags, bindings);
                    continue;
                }
                if (!(bufferRole)) Require(false, std::string("unsupported descriptor role ") + roleName(binding.role));
                if (!(binding.kind == ShaderRecompiler::DescriptorKind::StorageBuffer)) Require(false, std::string("unsupported descriptor kind ") + kindName(binding.kind) + " for role " + roleName(binding.role) + ": only StorageBuffer is supported");
                Require(!binding.readOnly, "read-only descriptors are unsupported because the recompiler emits no NonWritable decoration");
                Require(binding.count != 0, "empty descriptor binding");
                stageDescriptors += binding.count;
                storageBuffers += binding.count;
                Require(stageDescriptors <= context.limits.maxPerStageDescriptorStorageBuffers && stageDescriptors <= context.limits.maxPerStageResources, "shader descriptors exceed per-stage limits");
                Binding item{{binding.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, binding.count, flags, nullptr}, {}};
                if (binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
                    Require(words.size() == static_cast<std::uint64_t>(binding.count) * 4, "guest buffer descriptor must contain four DWORDs per array element");
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        // The recompiler proves elements read-only (bufferWritten false); the rest may be written.
                        const bool written = !binding.readOnly && (element >= binding.bufferWritten.size() || binding.bufferWritten[element]);
                        item.allocations.push_back(addGuestBuffer(words.subspan(static_cast<std::size_t>(element) * 4, 4), targets, indexAddress, indexBytes, written));
                    }
                } else if (addressRole) {
                    item.allocations.push_back(allocations.size());
                    allocations.push_back({0, 0, false, nullptr, binding.role});
                } else if (binding.role == ShaderRecompiler::DescriptorRole::Gds) {
                    // The global data share: the driver-owned guest range DMA_DATA reads and writes
                    // (Pm4::GdsAddress), bound as a writable guest buffer.
                    Require(binding.count == 1 && words.empty(), "invalid GDS descriptor contract");
                    const auto gds = Pm4::GdsAddress();
                    guestMemory.AddWritable(gds, Pm4::GdsBytes, 0, true);
                    item.allocations.push_back(allocations.size());
                    allocations.push_back({gds, Pm4::GdsBytes, true, nullptr});
                } else {
                    Require(binding.count == 1, "shader data and flattened SRT descriptors must not be arrays");
                    Require(!words.empty(), "empty shader data descriptor");
                    if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData) ValidateRuntimeResources(*shader.program, words);
                    item.allocations.push_back(addDataBuffer(words));
                }
                bindings.push_back(std::move(item));
            }
            // Min/max reduction samplers with linear filtering need a view format that filters
            // min/max (upstream 1afcd202); the pairs name elements of this shader's sampler binding.
            // ANISO_OVERRIDE: a sampler paired only with single-level images is bound without its
            // anisotropic filters (upstream d8bb4411, 17d9efc2, 1a778f75); the T#'s own levels decide.
            for (std::size_t index = firstSampler; index < samplers.size(); ++index) {
                const auto element = index - firstSampler;
                if (element >= 32u) break;
                bool single = false;
                bool mipmapped = false;
                for (const auto& paired : pairedSamplers) {
                    if (((paired.mask >> element) & 1u) == 0u) continue;
                    (paired.singleLevel ? single : mipmapped) = true;
                }
                const auto& input = samplerInputs[index];
                if (const auto variant = SingleLevelSamplerWords(std::span<const std::uint32_t, 4>(input.words), single, mipmapped)) {
                    auto resource = DecodeSamplerResource(*variant, input.unnormalized);
                    resource.compareEnable = input.compareEnable;
                    samplers[index] = context.samplerCache->Get(context, *variant, resource);
                }
            }
            const auto shaderSamplers = std::span<const std::shared_ptr<Sampler>>(samplers).subspan(firstSampler);
            for (const auto& paired : pairedSamplers) {
                RequireFilterMinmax(context, textures[paired.texture]->ViewFormat(), paired.mask, shaderSamplers);
                RequireDegammaFormat(paired.format, paired.mask, shaderSamplers);
                RequireBorderSwizzle(paired.bcSwizzle, paired.mask, shaderSamplers);
                RequireNonSeamlessCube(paired.cube, paired.mask, shaderSamplers);
            }
            for (auto& table : tableStages) {
                if (table.program == shader.program && table.stage == shader.stage) resolveImageTable(table, firstSampler);
            }
        }
        if (!tableStages.empty()) finishImageTables(bindings);
        Require(storageBuffers <= context.limits.maxDescriptorSetStorageBuffers, "pipeline descriptors exceed device limits");
        timing.Mark("bindings");
        // Storage images note their GPU writes while bindings are made, maybe after a texture over the
        // same memory was validated for this work.
        if (context.textureCache) {
            for (const auto& image : storageImages) {
                const auto [begin, end] = image->Range();
                context.textureCache->NoteDrawWrites(begin, end);
            }
        }
        guestMemory.Upload(usesBda);
        if (usesBda) bda = std::make_unique<BdaResources>(context, guestMemory);
        else if (usesFaultBuffer) bda = std::make_unique<BdaResources>(context);
        // The tables' poison entries, for the table faults the fault buffer reports.
        if (!tableStages.empty() && bda != nullptr) {
            std::vector<BdaResources::ImageTableFaults> faults;
            std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
            for (auto& table : tableStages) {
                faults.push_back({IrStage(table.stage), table.program->imageTableShader, std::move(table.poison), std::move(table.messages)});
                ranges.insert(ranges.end(), table.program->imageTableRanges.begin(), table.program->imageTableRanges.end());
            }
            bda->SetImageTables(std::move(faults), std::move(ranges));
        }
        tableStages.clear();
        timing.Mark("memory_upload");
        std::vector<VkDescriptorSetLayoutBinding> description;
        std::vector<VkDescriptorBindingFlags> bindingFlags;
        const bool flagged = std::any_of(bindings.begin(), bindings.end(), [](const Binding& binding) { return binding.flags != 0; });
        // The image table's set holds a variable count of its descriptors: its elements, rounded up
        // to a power of two (at least 16) so sets of tables of similar sizes are reused.
        std::optional<std::uint32_t> variableCount;
        std::map<VkDescriptorType, std::uint32_t> counts;
        for (const auto& binding : bindings) {
            description.push_back(binding.layout);
            if (flagged) bindingFlags.push_back(binding.flags);
            layoutKey.insert(layoutKey.end(), {binding.layout.binding, static_cast<std::uint32_t>(binding.layout.descriptorType), binding.layout.descriptorCount, binding.layout.stageFlags});
            if (binding.flags != 0) layoutKey.push_back(binding.flags);
            auto count = binding.layout.descriptorCount;
            if ((binding.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) != 0) {
                count = std::min(binding.layout.descriptorCount, std::max<std::uint32_t>(16u, std::bit_ceil(std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(binding.imageAllocations.size())))));
                variableCount = count;
            }
            counts[binding.layout.descriptorType] += count;
        }
        std::vector<VkDescriptorPoolSize> sizes;
        for (const auto& [type, count] : counts) {
            if (count != 0) sizes.push_back({type, count});
        }
        // Sets are cached by layout and, with an image table, by its variable count.
        auto allocationKey = layoutKey;
        if (variableCount) allocationKey.insert(allocationKey.end(), {0xffffffffu, *variableCount});
        if (!context.descriptorCache) context.descriptorCache = std::make_shared<DescriptorCache>();
        descriptors = context.descriptorCache->Take(allocationKey);
        if (!descriptors) descriptors = std::make_unique<DescriptorAllocation>(context, allocationKey, description, sizes, bindingFlags, variableCount);
        _layout = descriptors->Layout();
        _set = descriptors->Set();
        timing.Mark("descriptor_acquire");
        std::vector<VkDescriptorBufferInfo> buffers;
        std::vector<VkDescriptorImageInfo> images;
        std::vector<VkWriteDescriptorSet> writes;
        // Sized up front: the writes point into these arrays.
        std::size_t bufferCount = 0;
        std::size_t imageCount = 0;
        for (const auto& binding : bindings) {
            bufferCount += binding.allocations.size();
            imageCount += binding.imageAllocations.size();
        }
        buffers.reserve(bufferCount);
        images.reserve(imageCount);
        writes.reserve(bindings.size());
        for (const auto& binding : bindings) {
            if (binding.flags != 0) {
                // A table: runs of bound elements to write (rejected entries and the capacity's tail
                // stay unbound). A cached set usually comes from a draw of the same table: elements
                // it already holds are skipped (ANYPS5_NO_TABLE_WRITE_SKIP=1 writes them all).
                static const bool writeAll = std::getenv("ANYPS5_NO_TABLE_WRITE_SKIP") != nullptr;
                const bool samplerTable = binding.layout.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER;
                auto& held = descriptors->Written(binding.layout.binding);
                if (held.size() < binding.imageAllocations.size()) held.resize(binding.imageAllocations.size());
                const auto current = [&](std::size_t element) {
                    const auto index = binding.imageAllocations[element];
                    if (index == NoElement) return true;
                    if (writeAll) return false;
                    if (samplerTable) return held[element].Holds(samplers[index].get(), reinterpret_cast<std::uint64_t>(samplers[index]->Handle()), VK_IMAGE_LAYOUT_UNDEFINED);
                    return held[element].Holds(textures[index].get(), reinterpret_cast<std::uint64_t>(textures[index]->View()), textures[index]->SampledLayout());
                };
                for (std::size_t first = 0; first < binding.imageAllocations.size();) {
                    if (current(first)) {
                        ++first;
                        continue;
                    }
                    auto last = first;
                    while (last < binding.imageAllocations.size() && !current(last)) ++last;
                    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    write.dstSet = _set;
                    write.dstBinding = binding.layout.binding;
                    write.dstArrayElement = static_cast<std::uint32_t>(first);
                    write.descriptorCount = static_cast<std::uint32_t>(last - first);
                    write.descriptorType = binding.layout.descriptorType;
                    write.pImageInfo = images.data() + images.size();
                    for (auto element = first; element < last; ++element) {
                        const auto index = binding.imageAllocations[element];
                        if (samplerTable) {
                            images.push_back({samplers[index]->Handle(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                            held[element] = {samplers[index], samplers[index].get(), reinterpret_cast<std::uint64_t>(samplers[index]->Handle()), VK_IMAGE_LAYOUT_UNDEFINED};
                        } else {
                            images.push_back({VK_NULL_HANDLE, textures[index]->View(), textures[index]->SampledLayout()});
                            held[element] = {textures[index], textures[index].get(), reinterpret_cast<std::uint64_t>(textures[index]->View()), textures[index]->SampledLayout()};
                        }
                    }
                    writes.push_back(write);
                    first = last;
                }
                continue;
            }
            const auto bufferOffset = buffers.size();
            const auto imageOffset = images.size();
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = _set;
            write.dstBinding = binding.layout.binding;
            write.descriptorCount = binding.layout.descriptorCount;
            write.descriptorType = binding.layout.descriptorType;
            switch (binding.layout.descriptorType) {
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    for (const auto index : binding.allocations) buffers.push_back(descriptor(allocations[index]));
                    write.pBufferInfo = buffers.data() + bufferOffset;
                    break;
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    for (const auto index : binding.imageAllocations) images.push_back({VK_NULL_HANDLE, index == NullStorageImage ? VK_NULL_HANDLE : storageImages[index]->View(), VK_IMAGE_LAYOUT_GENERAL});
                    write.pImageInfo = images.data() + imageOffset;
                    break;
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                    for (const auto index : binding.imageAllocations) images.push_back({VK_NULL_HANDLE, textures[index]->View(), textures[index]->SampledLayout()});
                    write.pImageInfo = images.data() + imageOffset;
                    break;
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                    for (const auto index : binding.imageAllocations) images.push_back({samplers[index]->Handle(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                    write.pImageInfo = images.data() + imageOffset;
                    break;
                default: throw std::runtime_error("AGC graphics: ShaderResources encountered an unknown descriptor type while writing the descriptor set");
            }
            writes.push_back(write);
        }
        if (!writes.empty()) context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        timing.Mark("descriptor_update");
    } catch (...) {
        release();
        throw;
    }
}

std::size_t ShaderResources::addGuestBuffer(std::span<const std::uint32_t> words, std::span<const ColorTarget> targets, std::uint64_t indexAddress, std::size_t indexBytes, bool written) {
    Require(words.size() == 4, "buffer descriptor must contain four DWORDs");
    const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1], words[2], words[3]}};
    const auto address = descriptor.Base48();
    const auto byteSize = descriptor.GetSize();
    // Debug aid: APS5_TRACE_LARGE_BUFFERS=1 prints the first 20 guest buffers over 256 MB.
    static const bool traceLarge = std::getenv("APS5_TRACE_LARGE_BUFFERS") != nullptr;
    if (traceLarge && byteSize > (256ull << 20)) {
        static std::atomic<int> printed{0};
        if (printed.fetch_add(1) < 20) std::fprintf(stderr, "[large-buffer] 0x%llx bytes at 0x%llx (stride %u, records %u, written %d): %08x %08x %08x %08x\n", static_cast<unsigned long long>(byteSize), static_cast<unsigned long long>(address), descriptor.Stride(), descriptor.NumRecords(), written ? 1 : 0, words[0], words[1], words[2], words[3]);
    }
    // Games leave descriptor slots unfilled for resources a draw never reaches; the hardware only
    // faults if such a descriptor is used. An unusable one binds an empty buffer: shader accesses
    // are bounds-checked against the bound size, so reads return zero as with NUM_RECORDS = 0.
    const auto unusable = [&](const char* reason) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) std::fprintf(stderr, "[AnyPS5] binding an empty buffer for an unusable buffer descriptor (%s): %08x %08x %08x %08x\n", reason, words[0], words[1], words[2], words[3]);
        const std::array<std::uint32_t, 4> empty{};
        return addDataBuffer(empty);
    };
    if ((words[1] & 0x40000000u) != 0 || descriptor.Type() != 0u) return unusable("reserved bits or type");
    if (address == 0 || byteSize == 0) return unusable("null address or size");
    if (byteSize > context.limits.maxStorageBufferRange || byteSize > std::numeric_limits<std::size_t>::max()) return unusable("size");
    const auto size = static_cast<std::size_t>(byteSize);
    // With imported guest memory the shader reads and writes the range itself: earlier draws'
    // writes need a GPU barrier, not a CPU wait (an upload fallback reads through GuestMemory::Read,
    // which waits as a CPU access).
    std::optional<GuestMemory::GpuAccessScope> gpuAccess;
    if (context.guestGpuMemory != nullptr) gpuAccess.emplace();
    // A written buffer's write check covers the read check (it validates readability too); the
    // read check then only tells an unmapped range (unusable) from a read-only one (an error).
    try {
        GuestMemory::CheckRange(reinterpret_cast<const void*>(address), size, 1, written);
    } catch (const std::runtime_error&) {
        if (!written) return unusable("unmapped range");
        try {
            GuestMemory::CheckRange(reinterpret_cast<const void*>(address), size, 1, false);
        } catch (const std::runtime_error&) {
            return unusable("unmapped range");
        }
        throw;
    }
    for (const auto& target : targets) Require(!overlap(address, size, target.address, target.bytes), "shader buffer aliases the render target");
    Require(!written || !overlap(address, size, indexAddress, indexBytes), "writable shader buffer aliases the index buffer");
    // The view starts BufferViewMisalignment bytes below the base to meet the descriptor offset
    // alignment; the recompiler passed the same difference to the shader. The extra bytes are
    // uploaded but never written back.
    const auto alignment = static_cast<std::uint32_t>(context.limits.minStorageBufferOffsetAlignment);
    const auto below = ShaderRecompiler::BufferViewMisalignment(address, alignment);
    if (alignment > 1u && address % alignment != 0u && below == 0u) {
        // The shader was given no misalignment: the view needs a buffer of its own. A base off a
        // DWORD boundary is read by the shader from the DWORD below it (it joins two dwords per
        // access, upstream 38e89df1), so that buffer starts and ends on DWORD boundaries.
        const auto begin = address & ~std::uint64_t{3};
        const auto bytes = static_cast<std::size_t>(((address + size + 3u) & ~std::uint64_t{3}) - begin);
        guestMemory.AddDetached(begin, bytes);
        allocations.push_back({begin, bytes, true, nullptr});
        return allocations.size() - 1;
    }
    // The range was checked above (writable when written).
    if (written) guestMemory.AddWritable(address, size, below, true);
    else guestMemory.AddReadOnly(address, size, below, true);
    allocations.push_back({address - below, size + below, true, nullptr});
    return allocations.size() - 1;
}

std::size_t ShaderResources::addDataBuffer(std::span<const std::uint32_t> words) {
    const auto size = words.size() * sizeof(std::uint32_t);
    Require(size <= context.limits.maxStorageBufferRange, "shader data buffer exceeds descriptor range limit");
    // Placed in a chunk shared by queued draws (ANYPS5_DATA_BUFFERS=1: a pooled buffer each).
    static const bool ownBuffers = std::getenv("ANYPS5_DATA_BUFFERS") != nullptr;
    if (!ownBuffers && context.drawQueue != nullptr) {
        auto [chunk, offset] = context.drawQueue->TakeData(context, size);
        if (chunk) {
            std::memcpy(chunk->Bytes().data() + offset, words.data(), size);
            allocations.push_back({0, size, false, nullptr, ShaderRecompiler::DescriptorRole::ShaderData, std::move(chunk), offset});
            return allocations.size() - 1;
        }
    }
    auto buffer = std::make_unique<Buffer>(context, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(buffer->Bytes().data(), words.data(), size);
    allocations.push_back({0, size, false, std::move(buffer)});
    return allocations.size() - 1;
}

namespace {

// A zero-filled guest allocation that texture descriptors with a null base address are pointed at.
// On the console such a descriptor is bound but not sampled (an optional texture behind a branch),
// as shadPS4's null image assumes; sampling it here reads zeros instead of failing the draw
// (Balatro binds one while its textures load). 1 MiB covers a 64 KiB tile block for six cube faces.
std::uint64_t NullTextureMemory() {
    static const std::uint64_t address = [] {
        void* mapped = nullptr;
        constexpr int readWrite = GuestMemoryBacking::kProtCpuRead | GuestMemoryBacking::kProtCpuWrite | GuestMemoryBacking::kProtGpuRead;
        const auto status = GuestMemoryBacking::GuestVirtualMap_nid_postfix(&mapped, 1u << 20u, 1u << 16u, GuestMemoryBacking::Kind::Flexible, readWrite, 0, -1);
        Require(status == GuestMemoryBacking::Status::Ok && mapped != nullptr, "cannot map the null texture's guest memory");
        return reinterpret_cast<std::uint64_t>(mapped);
    }();
    return address;
}

}

ShaderResources::SampledLookup ShaderResources::lookupSampledTexture(std::span<const std::uint32_t> words, ShaderRecompiler::DescriptorImageShape shape, bool depthCompare, bool unnormalized) {
    // A null T# (no base address, or TYPE 0; IsNullTextureResource): a 1x1, one level, one layer
    // texture over zeroed guest memory, keyed by the patched descriptor.
    std::array<std::uint32_t, 8> nullWords{};
    const bool nullBase = IsNullTextureResource(words);
    if (nullBase) {
        std::copy(words.begin(), words.end(), nullWords.begin());
        const auto base = NullTextureMemory() >> 8u;
        // An all-zero descriptor (no texture bound) or a TYPE 0 one has no usable type or format: a
        // linear 8_8_8_8 UNORM image of the shader's declared shape whose selects read zero.
        const auto type = (nullWords[3] >> 28u) & 0xfu;
        const auto format = (nullWords[1] >> 20u) & 0x1ffu;
        if (type < 8u || format == 0u) {
            nullWords = {};
            std::uint32_t shapeType = 9u;
            switch (shape) {
                case ShaderRecompiler::DescriptorImageShape::Image1D: shapeType = 8u; break;
                case ShaderRecompiler::DescriptorImageShape::Image2D: shapeType = 9u; break;
                case ShaderRecompiler::DescriptorImageShape::Image3D: shapeType = 10u; break;
                case ShaderRecompiler::DescriptorImageShape::ImageCube: shapeType = 11u; break;
                case ShaderRecompiler::DescriptorImageShape::Image2DArray: shapeType = 13u; break;
            }
            nullWords[1] = 56u << 20u;
            nullWords[3] = shapeType << 28u;
        }
        nullWords[0] = static_cast<std::uint32_t>(base);
        nullWords[1] = (nullWords[1] & ~0xffu) | static_cast<std::uint32_t>((base >> 32u) & 0xffu);
        words = nullWords;
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) std::fprintf(stderr, "[AnyPS5] sampling zeros for a texture descriptor with a null base address\n");
    }
    auto resource = DecodeTextureResource(words);
    if (nullBase) {
        resource.width = 1;
        resource.height = 1;
        resource.mipCount = 1;
        resource.baseLevel = 0;
        resource.lastLevel = 0;
        resource.baseArray = 0;
        resource.depthOrLastArray = 0;
    }
    // A 1D T# under a 2D image (the recompiler samples it as a 2D image of height 1): the
    // texture is made 2D, one row high, so a 2D view of it exists.
    const bool twoDimensionalShape = shape == ShaderRecompiler::DescriptorImageShape::Image2D || shape == ShaderRecompiler::DescriptorImageShape::Image2DArray;
    if (twoDimensionalShape && (resource.dimension == TextureDimension::k1D || resource.dimension == TextureDimension::k1DArray)) {
        resource.dimension = resource.dimension == TextureDimension::k1D ? TextureDimension::k2D : TextureDimension::k2DArray;
        resource.height = 1;
    }
    const auto view = SampledViewDimension(shape, resource.dimension);
    if (!(view.has_value())) Require(false, "guest texture dimension " + std::to_string(static_cast<int>(resource.dimension)) + " (" + std::to_string(resource.width) + "x" + std::to_string(resource.height) + ") cannot be viewed with the shader's declared image shape " + std::to_string(static_cast<int>(shape)));
    resource.viewDimension = *view;
    // A converted format's shader applies DST_SEL itself (upstream baa202d2): its view keeps
    // the packed dword in X.
    const VkComponentMapping components = IsConvertedTextureFormat(resource.format)
        ? VkComponentMapping{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A}
        : VkComponentMapping{TextureComponentChannel(resource.format, ComponentSwizzleFor(resource.dstSelX)), TextureComponentChannel(resource.format, ComponentSwizzleFor(resource.dstSelY)), TextureComponentChannel(resource.format, ComponentSwizzleFor(resource.dstSelZ)), TextureComponentChannel(resource.format, ComponentSwizzleFor(resource.dstSelW))};
    auto texture = context.textureCache->Get(words, resource, components, depthCompare);
    // A texture sampled with unnormalized coordinates (upstream 76ddd9b6): the host sampler
    // selects the S#'s texels only on a single-level, single-layer 1D or 2D view of mip 0.
    if (unnormalized) {
        const auto range = texture->SampledViewRange();
        const bool singleLevel = range.levels == 1u && range.layers == 1u && resource.baseLevel == 0u && EffectiveMinLod(resource) == 0.0f;
        if (!singleLevel || (range.type != VK_IMAGE_VIEW_TYPE_1D && range.type != VK_IMAGE_VIEW_TYPE_2D)) throw std::runtime_error("AGC graphics: guest texture sampled with unnormalized coordinates is not a single-level, single-layer 1D or 2D view starting at mip 0, which is not implemented (base level " + std::to_string(resource.baseLevel) + ", levels " + std::to_string(range.levels) + ", layers " + std::to_string(range.layers) + ", view type " + std::to_string(static_cast<int>(range.type)) + ")");
    }
    return {std::move(texture), nullBase || DescriptorSingleLevel(words), resource.viewDimension == TextureDimension::kCube, resource.bcSwizzle, resource.format};
}

void ShaderResources::addImageBinding(const ShaderRecompiler::DescriptorBinding& binding, std::span<const std::uint32_t> descriptorWords, VkShaderStageFlags flags, std::vector<Binding>& bindings) {
    Require(binding.count != 0, "empty descriptor binding");
    const bool sampledImage = binding.kind == ShaderRecompiler::DescriptorKind::SampledImage;
    const bool samplerKind = binding.kind == ShaderRecompiler::DescriptorKind::Sampler;
    if (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) {
        Require(binding.role == ShaderRecompiler::DescriptorRole::GuestImages, "storage image descriptor role disagrees with its kind");
        Require(descriptorWords.size() == static_cast<std::size_t>(binding.count) * 8, "guest storage image descriptor must contain 8 dwords per element");
        Require(binding.imageShape.has_value(), "guest storage image binding is missing an image shape");
        Require(binding.count <= context.limits.maxPerStageDescriptorStorageImages, "shader storage-image descriptors exceed per-stage limits");
        Binding item{{binding.binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, binding.count, flags, nullptr}, {}, {}};
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const auto words = descriptorWords.subspan(static_cast<std::size_t>(element) * 8, 8);
            // A null T# (no base address, or TYPE 0) in a typed heap: with nullDescriptor the element
            // binds no view, so its loads read zero and its stores are dropped, as on the hardware.
            if (IsNullTextureResource(words) && context.nullDescriptors) {
                item.imageAllocations.push_back(NullStorageImage);
                continue;
            }
            auto resource = DecodeTextureResource(words);
            // A 2D access (MIMG DIM 2D) to a 2D array carries no slice coordinate, so the hardware
            // addresses slice 0 of the view, the BASE_ARRAY layer: bound as a 2D image of that
            // layer (upstream edb13581).
            const bool firstLayer = *binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2D && resource.dimension == TextureDimension::k2DArray;
            if (firstLayer) resource.dimension = TextureDimension::k2D;
            if (!MatchesGuestDimension(*binding.imageShape, resource.dimension)) {
                throw std::runtime_error("AGC graphics: guest storage image dimension " + std::to_string(static_cast<int>(resource.dimension)) + " disagrees with the shader's declared image shape " + std::to_string(static_cast<int>(*binding.imageShape)));
            }
            if (*binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2DArray) resource.dimension = TextureDimension::k2DArray;
            const bool atomic = element < binding.imageAtomic.size() && binding.imageAtomic[element];
            const bool atomic64 = element < binding.imageAtomic64.size() && binding.imageAtomic64[element];
            storageImages.push_back(std::make_unique<StorageImage>(context, resource, StorageImageFormat(ResolveTextureFormat(resource.format), atomic, atomic64)));
            storageWritten.push_back(element >= binding.imageWritten.size() || binding.imageWritten[element] || atomic);
            item.imageAllocations.push_back(storageImages.size() - 1);
        }
        Require(storageImages.size() <= context.limits.maxDescriptorSetStorageImages, "pipeline storage-image descriptors exceed device limits");
        bindings.push_back(std::move(item));
        return;
    }
    if (!(sampledImage || samplerKind)) Require(false, std::string("unsupported descriptor kind ") + kindName(binding.kind) + " for role " + roleName(binding.role));
    Require((sampledImage && binding.role == ShaderRecompiler::DescriptorRole::GuestImages) || (samplerKind && binding.role == ShaderRecompiler::DescriptorRole::GuestSamplers), "guest image descriptor role disagrees with its kind");
    Require(descriptorWords.size() % binding.count == 0, "guest image descriptor size is not a multiple of the binding count");
    const auto elementWords = descriptorWords.size() / binding.count;

    Binding item{{binding.binding, sampledImage ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, binding.count, flags, nullptr}, {}, {}};

    if (sampledImage) {
        Require(elementWords == 8, "guest texture descriptor must contain 8 dwords");
        Require(binding.imageShape.has_value(), "guest image binding is missing an image shape");
        Require(context.detiler != nullptr, "device texture detiler is unavailable");
        Require(context.textureCache != nullptr, "device texture cache is unavailable");
        Require(binding.count <= context.limits.maxPerStageDescriptorSampledImages, "shader sampled-image descriptors exceed per-stage limits");
        // An element repeating the one just before it (the same eight words, depth-compare and
        // unnormalized flags) binds the same texture: nothing runs between the two lookups, so the
        // second would find what the first left (upstream 63fb5779). ANYPS5_NO_REPEATED_TEXTURE=1
        // looks every element up.
        static const bool lookUpRepeats = std::getenv("ANYPS5_NO_REPEATED_TEXTURE") != nullptr;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            auto words = descriptorWords.subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
            if (!lookUpRepeats && element != 0 && !pairedSamplers.empty() && pairedSamplers.back().texture + 1 == textures.size()) {
                const auto previous = descriptorWords.subspan(static_cast<std::size_t>(element - 1) * elementWords, elementWords);
                const auto flag = [](const std::vector<bool>& flags, std::uint32_t index) { return index < flags.size() && flags[index]; };
                if (std::equal(words.begin(), words.end(), previous.begin()) && flag(binding.imageDepthCompare, element) == flag(binding.imageDepthCompare, element - 1) && flag(binding.imageUnnormalized, element) == flag(binding.imageUnnormalized, element - 1)) {
                    auto paired = pairedSamplers.back();
                    paired.texture = textures.size();
                    paired.mask = element < binding.imageSamplers.size() ? binding.imageSamplers[element] : 0u;
                    pairedSamplers.push_back(paired);
                    textures.push_back(textures.back());
                    item.imageAllocations.push_back(textures.size() - 1);
                    continue;
                }
            }
            const bool depthCompare = element < binding.imageDepthCompare.size() && binding.imageDepthCompare[element];
            const bool unnormalized = element < binding.imageUnnormalized.size() && binding.imageUnnormalized[element];
            auto looked = lookupSampledTexture(words, *binding.imageShape, depthCompare, unnormalized);
            // The sampler elements paired with it, checked once the shader's samplers are bound.
            pairedSamplers.push_back({textures.size(), element < binding.imageSamplers.size() ? binding.imageSamplers[element] : 0u, looked.singleLevel, looked.cube, looked.bcSwizzle, looked.format});
            textures.push_back(std::move(looked.texture));
            item.imageAllocations.push_back(textures.size() - 1);
        }
        Require(textures.size() <= context.limits.maxDescriptorSetSampledImages, "pipeline sampled-image descriptors exceed device limits");
    } else {
        Require(elementWords == 4, "guest sampler descriptor must contain 4 dwords");
        Require(binding.count <= context.limits.maxPerStageDescriptorSamplers, "shader sampler descriptors exceed per-stage limits");
        Require(binding.samplerDepthCompare.size() == binding.count, "guest sampler binding is missing depth comparison metadata");
        Require(binding.samplerUnnormalized.empty() || binding.samplerUnnormalized.size() == binding.count, "guest sampler binding has unnormalized coordinate metadata of another size");
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const auto words = descriptorWords.subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
            const bool unnormalized = element < binding.samplerUnnormalized.size() && binding.samplerUnnormalized[element];
            auto resource = DecodeSamplerResource(words, unnormalized);
            resource.compareEnable = binding.samplerDepthCompare.at(element);
            if (!context.samplerCache) context.samplerCache = std::make_shared<SamplerCache>();
            samplers.push_back(context.samplerCache->Get(context, words, resource));
            samplerInputs.push_back({{words[0], words[1], words[2], words[3]}, resource.compareEnable, unnormalized, element < binding.samplerIntegerBorder.size() && binding.samplerIntegerBorder[element]});
            item.imageAllocations.push_back(samplers.size() - 1);
        }
        Require(samplers.size() <= context.limits.maxDescriptorSetSamplers, "pipeline sampler descriptors exceed device limits");
    }

    bindings.push_back(std::move(item));
}

std::uint32_t ImageTableCapacity(const Context& context) {
    if (!context.runtimeDescriptorArray || !context.partiallyBound || !context.variableDescriptorCount || !context.descriptorIndexing) return 0u;
    // What the typed heaps of every stage may take of the per-stage and per-set limits stays theirs.
    constexpr std::uint64_t sampledHeaps = static_cast<std::uint64_t>(ShaderRecompiler::RuntimeAbi::FirstStorageImageBinding - ShaderRecompiler::RuntimeAbi::FirstImageBinding) * ShaderRecompiler::RuntimeAbi::SampledHeapCapacity;
    constexpr std::uint64_t storageHeaps = static_cast<std::uint64_t>(static_cast<std::uint32_t>(ShaderRecompiler::RuntimeAbi::Binding::Samplers) - ShaderRecompiler::RuntimeAbi::FirstStorageImageBinding) * ShaderRecompiler::RuntimeAbi::StorageHeapCapacity;
    constexpr std::uint64_t buffers = ShaderRecompiler::RuntimeAbi::BufferCapacity + 8u;
    const auto room = [](std::uint64_t limit, std::uint64_t reserve) { return limit > reserve ? limit - reserve : 0u; };
    std::uint64_t capacity = static_cast<std::uint64_t>(ShaderRecompiler::RuntimeAbi::StageCount) * ShaderRecompiler::ImageTableAbi::ImageElementBudget;
    capacity = std::min(capacity, room(context.limits.maxPerStageDescriptorSampledImages, sampledHeaps));
    capacity = std::min(capacity, room(context.limits.maxPerStageResources, sampledHeaps + storageHeaps + buffers));
    capacity = std::min(capacity, room(context.limits.maxDescriptorSetSampledImages, sampledHeaps * ShaderRecompiler::RuntimeAbi::StageCount));
    return static_cast<std::uint32_t>(capacity);
}

std::uint32_t SamplerTableCapacity(const Context& context) {
    if (!context.runtimeDescriptorArray || !context.partiallyBound) return 0u;
    const auto room = [](std::uint64_t limit, std::uint64_t reserve) { return limit > reserve ? limit - reserve : 0u; };
    std::uint64_t capacity = ShaderRecompiler::ImageTableAbi::SamplerElementBudget;
    capacity = std::min<std::uint64_t>(capacity, room(context.limits.maxPerStageDescriptorSamplers, ShaderRecompiler::RuntimeAbi::SamplerHeapCapacity));
    capacity = std::min<std::uint64_t>(capacity, room(context.limits.maxDescriptorSetSamplers / ShaderRecompiler::RuntimeAbi::StageCount, ShaderRecompiler::RuntimeAbi::SamplerHeapCapacity));
    return static_cast<std::uint32_t>(capacity - capacity % ShaderRecompiler::ImageTableAbi::SamplerEntryElements);
}

ShaderResources::TableStage& ShaderResources::tableStage(const CompiledShader& shader) {
    for (auto& table : tableStages) {
        if (table.program == shader.program && table.stage == shader.stage) return table;
    }
    auto& table = tableStages.emplace_back();
    table.stage = shader.stage;
    table.program = shader.program;
    table.poison = shader.program->imageTablePoison;
    table.messages.assign(table.poison.size(), std::string{});
    return table;
}

// A table entry this driver cannot bind: a poison entry of its own, whose code the map gives the
// entry, so the shader faults (naming the message) only if it selects it.
std::uint32_t ShaderResources::rejectTableEntry(TableStage& table, std::span<const std::uint32_t> words, const std::string& message) {
    ShaderRecompiler::ImageTableEntryPoison poison{};
    const auto count = std::min(words.size(), poison.words.size());
    std::copy_n(words.begin(), count, poison.words.begin());
    poison.dwordCount = static_cast<std::uint32_t>(count);
    poison.resource = std::numeric_limits<std::uint32_t>::max();
    poison.reason = static_cast<std::uint32_t>(ShaderRecompiler::ImageTableAbi::PoisonReason::DriverRejected);
    table.poison.push_back(poison);
    table.messages.push_back(message);
    return ShaderRecompiler::ImageTableAbi::PoisonCode(static_cast<std::uint32_t>(table.poison.size() - 1u));
}

void ShaderResources::addSamplerTable(const ShaderRecompiler::DescriptorBinding& binding, std::span<const std::uint32_t> words, VkShaderStageFlags flags, TableStage& table, std::vector<Binding>& bindings) {
    namespace Abi = ShaderRecompiler::ImageTableAbi;
    Require(binding.kind == ShaderRecompiler::DescriptorKind::Sampler && words.size() == static_cast<std::size_t>(binding.count) * 4u && binding.samplerDepthCompare.size() == binding.count && binding.count % Abi::SamplerEntryElements == 0u, "sampler table binding is malformed");
    Require(context.samplerTableCapacity != 0, "sampler tables need runtimeDescriptorArray and descriptorBindingPartiallyBound, which the device lacks");
    if (!context.samplerCache) context.samplerCache = std::make_shared<SamplerCache>();
    // A fixed layout count (the capacity), partially bound: the pipeline layout stays the same
    // whatever the table holds.
    Binding item{{binding.binding, VK_DESCRIPTOR_TYPE_SAMPLER, context.samplerTableCapacity, flags, nullptr}, {}, {}, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
    item.imageAllocations.reserve(binding.count);
    for (std::uint32_t entry = 0; entry < binding.count / Abi::SamplerEntryElements; ++entry) {
        const auto first = entry * Abi::SamplerEntryElements;
        std::array<GuestSamplerResource, Abi::SamplerEntryElements> resources{};
        std::string rejected;
        if (first + Abi::SamplerEntryElements > context.samplerTableCapacity) rejected = "the sampler table exceeds the device's capacity of " + std::to_string(context.samplerTableCapacity) + " samplers";
        for (std::uint32_t element = first; rejected.empty() && element < first + Abi::SamplerEntryElements; ++element) {
            try {
                const auto elementWords = std::span<const std::uint32_t, 4>(words.data() + static_cast<std::size_t>(element) * 4u, 4u);
                auto resource = DecodeSamplerResource(elementWords);
                resource.compareEnable = binding.samplerDepthCompare.at(element);
                Sampler::Validate(context, resource);
                // What the direct path pairs per texture, a table cannot (its pairs are chosen at
                // run time): such entries are rejected.
                Require(!Sampler::ReadsOpaqueBlackBorder(resource), "a sampler table entry with an opaque black border needs dynamic texture BC-swizzle pairing, which is not implemented");
                Require(resource.addressModeU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER && resource.addressModeV != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER && resource.addressModeW != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, "a sampler table entry with a border needs dynamic texture numeric-class pairing, which is not implemented");
                Require(!resource.nonSeamlessCube || context.nonSeamlessCubeMap, "a sampler table entry that disables seamless cube filtering needs VK_EXT_non_seamless_cube_map because dynamic texture/sampler pairing is not implemented");
                Require(!SingleLevelSamplerWords(elementWords, true, false).has_value(), "a sampler table entry with anisotropic ANISO_OVERRIDE needs dynamic texture mip-level pairing, which is not implemented");
                resources[element - first] = resource;
            } catch (const std::runtime_error& error) {
                rejected = error.what();
            }
        }
        if (!rejected.empty()) {
            table.samplerRejections.emplace_back(entry, rejectTableEntry(table, words.subspan(static_cast<std::size_t>(first) * 4u, 4u), rejected));
            item.imageAllocations.insert(item.imageAllocations.end(), Abi::SamplerEntryElements, NoElement);
            continue;
        }
        for (std::uint32_t element = first; element < first + Abi::SamplerEntryElements; ++element) {
            const auto elementWords = words.subspan(static_cast<std::size_t>(element) * 4u, 4u);
            const auto& resource = resources[element - first];
            samplers.push_back(context.samplerCache->Get(context, elementWords, resource));
            samplerInputs.push_back({{elementWords[0], elementWords[1], elementWords[2], elementWords[3]}, resource.compareEnable, false, false});
            item.imageAllocations.push_back(samplers.size() - 1u);
        }
    }
    Require(samplers.size() <= context.limits.maxDescriptorSetSamplers, "pipeline sampler descriptors exceed device limits");
    bindings.push_back(std::move(item));
}

// The stage's image table entries: a texture each (repeated T#s share one lookup), or a poison
// code in the map for an entry this driver cannot bind. firstSampler: the stage's first sampler,
// the pairs its entries name.
void ShaderResources::resolveImageTable(TableStage& table, std::size_t firstSampler) {
    namespace Abi = ShaderRecompiler::ImageTableAbi;
    Require(table.mapBinding != NoElement, "an image table stage has no map");
    auto& map = table.map;
    Require(map.size() >= Abi::HeaderWords, "image table map is truncated");
    const auto elements = table.images != nullptr ? table.images->count : 0u;
    Require(map[Abi::HeaderElementCount] == elements, "image table map disagrees with its image binding");
    const auto faults = map[Abi::HeaderElementFaults];
    const auto samplerFaults = map[Abi::HeaderSamplerFaults];
    Require(static_cast<std::uint64_t>(faults) + elements <= map.size() && static_cast<std::uint64_t>(samplerFaults) + map[Abi::HeaderSamplerCount] / Abi::SamplerEntryElements <= map.size() && static_cast<std::uint64_t>(Abi::TableHeader(map[Abi::HeaderTableCount])) <= map.size(), "image table map is truncated");
    if (table.images == nullptr) return;
    Require(context.imageTableCapacity != 0, "image tables need runtimeDescriptorArray, descriptorBindingPartiallyBound and descriptorBindingVariableDescriptorCount, which the device lacks");
    Require(context.textureCache != nullptr && context.detiler != nullptr, "device texture cache is unavailable");
    const auto& binding = *table.images;
    const auto elementBase = tableElements;
    map[Abi::HeaderElementBase] = elementBase;
    tableElements += elements;
    tableStageFlags |= VulkanStage(table.stage);
    const auto stageSamplers = std::span<const std::shared_ptr<Sampler>>(samplers).subspan(firstSampler);
    // T# words, depth compare, image shape and sampler pairs: entries alike bind one texture (or
    // share one rejection).
    using EntryKey = std::array<std::uint32_t, 11>;
    struct EntryHash {
        std::size_t operator()(const EntryKey& key) const noexcept {
            std::uint64_t value = 0xcbf29ce484222325ull;
            for (const auto word : key) value = (value ^ word) * 0x100000001b3ull;
            return static_cast<std::size_t>(value ^ (value >> 32u));
        }
    };
    std::unordered_map<EntryKey, std::pair<std::size_t, std::uint32_t>, EntryHash> resolved;
    resolved.reserve(elements);
    tableTextures.reserve(tableTextures.size() + elements);
    for (std::uint32_t element = 0; element < elements; ++element) {
        const auto words = table.imageWords.subspan(static_cast<std::size_t>(element) * 8u, 8u);
        if (static_cast<std::uint64_t>(elementBase) + element >= context.imageTableCapacity) {
            map[faults + element] = rejectTableEntry(table, words, "the image table exceeds the device's capacity of " + std::to_string(context.imageTableCapacity) + " images");
            tableTextures.push_back(NoElement);
            continue;
        }
        const bool depthCompare = binding.imageDepthCompare.at(element);
        const auto shape = binding.imageShapes.at(element);
        const auto mask = binding.imageSamplers.at(element);
        std::array<std::uint32_t, 11> key{};
        std::copy(words.begin(), words.end(), key.begin());
        key[8] = depthCompare ? 1u : 0u;
        key[9] = static_cast<std::uint32_t>(shape);
        key[10] = mask;
        if (const auto found = resolved.find(key); found != resolved.end()) {
            if (found->second.first == NoElement) map[faults + element] = found->second.second;
            tableTextures.push_back(found->second.first);
            continue;
        }
        try {
            auto looked = lookupSampledTexture(words, shape, depthCompare, false);
            RequireFilterMinmax(context, looked.texture->ViewFormat(), mask, stageSamplers);
            RequireDegammaFormat(looked.format, mask, stageSamplers);
            RequireBorderSwizzle(looked.bcSwizzle, mask, stageSamplers);
            RequireNonSeamlessCube(looked.cube, mask, stageSamplers);
            // A sampler's border color class was chosen for the images the recompiler paired with
            // it; a table entry of the other class would read the wrong border.
            const auto numeric = ShaderRecompiler::DepthBitsTextureWidth(words[1], words[3]) != 0u ? ShaderRecompiler::IrTextureNumericClass::Float : ShaderRecompiler::SampledTextureNumericClass(static_cast<ShaderRecompiler::IrBufferFormat>(looked.format));
            const bool integerImage = numeric == ShaderRecompiler::IrTextureNumericClass::Uint || numeric == ShaderRecompiler::IrTextureNumericClass::Sint;
            for (std::uint32_t sampler = 0; sampler < stageSamplers.size() && sampler < 32u; ++sampler) {
                if (((mask >> sampler) & 1u) == 0u) continue;
                const auto& input = samplerInputs.at(firstSampler + sampler);
                const auto descriptor = DecodeSamplerResource(input.words, input.unnormalized);
                const bool border = descriptor.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || descriptor.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || descriptor.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                Require(!border || input.integerBorder == integerImage, "an image table entry's sampler border color numeric class disagrees with its sampled image class, which needs dynamic pairing and is not implemented");
                Require(!SingleLevelSamplerWords(std::span<const std::uint32_t, 4>(input.words), true, false).has_value(), "an image table entry paired with anisotropic ANISO_OVERRIDE needs dynamic texture mip-level pairing, which is not implemented");
            }
            textures.push_back(std::move(looked.texture));
            resolved.emplace(key, std::pair<std::size_t, std::uint32_t>{textures.size() - 1u, 0u});
            tableTextures.push_back(textures.size() - 1u);
        } catch (const std::runtime_error& error) {
            // An entry over memory the guest has not mapped (a stale or garbage record) is named so.
            std::string message = error.what();
            if (message.find("is not readable") != std::string::npos) message = "the table entry's surface at 0x" + HexText((static_cast<std::uint64_t>(words[1] & 0xffu) << 40u) | (static_cast<std::uint64_t>(words[0]) << 8u)) + " is not mapped (" + message + ")";
            map[faults + element] = rejectTableEntry(table, words, message);
            resolved.emplace(key, std::pair<std::size_t, std::uint32_t>{NoElement, map[faults + element]});
            tableTextures.push_back(NoElement);
        }
    }
    Require(textures.size() <= context.limits.maxDescriptorSetSampledImages, "pipeline sampled-image descriptors exceed device limits");
}

// The maps (with the driver's fault codes) into their buffers, and the image table binding.
void ShaderResources::finishImageTables(std::vector<Binding>& bindings) {
    namespace Abi = ShaderRecompiler::ImageTableAbi;
    for (auto& table : tableStages) {
        auto& map = table.map;
        Require(table.mapBinding != NoElement, "an image table stage has no map");
        for (const auto& [entry, code] : table.samplerRejections) map.at(map[Abi::HeaderSamplerFaults] + entry) = code;
        // A table the work itself stores to would change under it: its selections fault.
        for (std::uint32_t index = 0; index < map[Abi::HeaderTableCount]; ++index) {
            const auto header = Abi::TableHeader(index);
            if (map[header + Abi::TableFault] != 0u) continue;
            const auto base = static_cast<std::uint64_t>(map[header + Abi::TableBaseLow]) | (static_cast<std::uint64_t>(map[header + Abi::TableBaseHigh]) << 32u);
            const auto size = static_cast<std::uint64_t>(map[header + Abi::TableSizeLow]) | (static_cast<std::uint64_t>(map[header + Abi::TableSizeHigh]) << 32u);
            // The table's extent, or for one reached through a pointer (no extent) the records the
            // capture read from it.
            std::vector<std::pair<std::uint64_t, std::uint64_t>> extents;
            if (size != 0) extents.emplace_back(base, size);
            if (index < table.program->imageTableReadRanges.size()) extents.insert(extents.end(), table.program->imageTableReadRanges[index].begin(), table.program->imageTableReadRanges[index].end());
            const bool written = std::any_of(extents.begin(), extents.end(), [&](const std::pair<std::uint64_t, std::uint64_t>& extent) {
                if (extent.second == 0) return false;
                if (guestMemory.WritesOverlap(extent.first, static_cast<std::size_t>(extent.second))) return true;
                for (std::size_t image = 0; image < storageImages.size(); ++image) {
                    if (storageWritten[image] && storageImages[image]->Overlaps(extent.first, static_cast<std::size_t>(extent.second))) return true;
                }
                return false;
            });
            if (!written) continue;
            ShaderRecompiler::ImageTableEntryPoison poison{};
            poison.resource = index;
            poison.reason = static_cast<std::uint32_t>(Abi::PoisonReason::HeapWritten);
            table.poison.push_back(poison);
            table.messages.push_back("the table at 0x" + HexText(base) + " is written by its own dispatch");
            map[header + Abi::TableFault] = Abi::PoisonCode(static_cast<std::uint32_t>(table.poison.size() - 1u));
        }
        map[Abi::HeaderPoisonCount] = static_cast<std::uint32_t>(table.poison.size());
        bindings.at(table.mapBinding).allocations = {addDataBuffer(map)};
    }
    if (tableStageFlags == 0) return;
    // One binding for every stage's table, the highest-numbered of the set (its count is variable),
    // with a descriptor even for empty tables.
    tableTextures.resize(std::min<std::size_t>(tableElements, context.imageTableCapacity), NoElement);
    bindings.push_back({{ShaderRecompiler::RuntimeAbi::ImageTableBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, context.imageTableCapacity, tableStageFlags, nullptr}, {}, std::move(tableTextures), VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT});
    tableTextures.clear();
}

ShaderResources::~ShaderResources() {
    release();
}

void ShaderResources::release() noexcept {
    if (descriptors) context.descriptorCache->Put(std::move(descriptors));
    _layout = VK_NULL_HANDLE;
    _set = VK_NULL_HANDLE;
}

VkDescriptorSetLayout ShaderResources::Layout() const {
    return _layout;
}

void ShaderResources::Bind(VkCommandBuffer commands, VkPipelineBindPoint bindPoint, VkPipelineLayout layout) const {
    if (_set == VK_NULL_HANDLE) return;
    context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, bindPoint, layout, 0, 1, &_set, 0, nullptr);
}

void ShaderResources::RecordUploads(VkCommandBuffer commands) const {
    for (const auto& texture : textures) texture->PrepareSampling(commands);
    for (const auto& image : storageImages) image->RecordUpload(commands);
}

bool ShaderResources::RecordsOutsidePass() const {
    if (!storageImages.empty()) return true;
    for (const auto& texture : textures)
        if (texture->Direct()) return true;
    return false;
}

void ShaderResources::RecordDownloads(VkCommandBuffer commands) const {
    for (const auto& image : storageImages) image->RecordDownload(commands);
}

void ShaderResources::WriteBack() {
    // A reported fault (a BDA or image table fault) is thrown after the work's results were written
    // back: the shader ran to its end, as on the hardware.
    std::exception_ptr fault;
    if (bda) {
        try {
            bda->CheckFault();
        } catch (...) {
            fault = std::current_exception();
        }
    }
    guestMemory.WriteBack();
    for (const auto& image : storageImages) image->WriteBack();
    if (fault) std::rethrow_exception(fault);
}

void ShaderResources::AppendWrites(std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) const {
    guestMemory.AppendWrites(ranges);
    for (const auto& image : storageImages) ranges.push_back(image->Range());
}

bool ShaderResources::WritesOverlap(std::uint64_t address, std::size_t bytes) const {
    if (guestMemory.WritesOverlap(address, bytes)) return true;
    for (const auto& image : storageImages) {
        if (image->Overlaps(address, bytes)) return true;
    }
    return false;
}

}
