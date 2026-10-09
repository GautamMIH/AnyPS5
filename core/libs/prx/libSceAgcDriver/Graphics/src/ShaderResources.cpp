#include <algorithm>
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
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
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
            for (const auto& binding : shader.program->bindings) {
                Require(binding.descriptorSet == 0, "unexpected descriptor set: every shader resource must use descriptor set zero");
                Require(occupied.insert(binding.binding).second, "duplicate shader binding");
                ShaderRecompiler::RuntimeAbi::RequireVersion(shader.program->runtimeAbiVersion);
                const bool addressRole = binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable || binding.role == ShaderRecompiler::DescriptorRole::FaultBuffer;
                const bool bufferRole = addressRole || binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers || binding.role == ShaderRecompiler::DescriptorRole::ShaderData || binding.role == ShaderRecompiler::DescriptorRole::FlattenedSrt || binding.role == ShaderRecompiler::DescriptorRole::Gds;
                const bool imageRole = binding.role == ShaderRecompiler::DescriptorRole::GuestImages || binding.role == ShaderRecompiler::DescriptorRole::GuestSamplers;
                if (imageRole) {
                    addImageBinding(binding, flags, bindings);
                    continue;
                }
                Require(bufferRole, std::string("unsupported descriptor role ") + roleName(binding.role));
                Require(binding.kind == ShaderRecompiler::DescriptorKind::StorageBuffer, std::string("unsupported descriptor kind ") + kindName(binding.kind) + " for role " + roleName(binding.role) + ": only StorageBuffer is supported");
                Require(!binding.readOnly, "read-only descriptors are unsupported because the recompiler emits no NonWritable decoration");
                Require(binding.count != 0, "empty descriptor binding");
                stageDescriptors += binding.count;
                storageBuffers += binding.count;
                Require(stageDescriptors <= context.limits.maxPerStageDescriptorStorageBuffers && stageDescriptors <= context.limits.maxPerStageResources, "shader descriptors exceed per-stage limits");
                Binding item{{binding.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, binding.count, flags, nullptr}, {}};
                if (binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
                    Require(binding.guestDescriptor.size() == static_cast<std::uint64_t>(binding.count) * 4, "guest buffer descriptor must contain four DWORDs per array element");
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        // The recompiler proves elements read-only (bufferWritten false); the rest may be written.
                        const bool written = !binding.readOnly && (element >= binding.bufferWritten.size() || binding.bufferWritten[element]);
                        item.allocations.push_back(addGuestBuffer(std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 4, 4), targets, indexAddress, indexBytes, written));
                    }
                } else if (addressRole) {
                    item.allocations.push_back(allocations.size());
                    allocations.push_back({0, 0, false, nullptr, binding.role});
                } else if (binding.role == ShaderRecompiler::DescriptorRole::Gds) {
                    // The global data share: the driver-owned guest range DMA_DATA reads and writes
                    // (Pm4::GdsAddress), bound as a writable guest buffer.
                    Require(binding.count == 1 && binding.guestDescriptor.empty(), "invalid GDS descriptor contract");
                    const auto gds = Pm4::GdsAddress();
                    guestMemory.AddWritable(gds, Pm4::GdsBytes, 0, true);
                    item.allocations.push_back(allocations.size());
                    allocations.push_back({gds, Pm4::GdsBytes, true, nullptr});
                } else {
                    Require(binding.count == 1, "shader data and flattened SRT descriptors must not be arrays");
                    Require(!binding.guestDescriptor.empty(), "empty shader data descriptor");
                    if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData) ValidateRuntimeResources(*shader.program, binding.guestDescriptor);
                    item.allocations.push_back(addDataBuffer(binding.guestDescriptor));
                }
                bindings.push_back(std::move(item));
            }
            // Min/max reduction samplers with linear filtering need a view format that filters
            // min/max (upstream 1afcd202); the pairs name elements of this shader's sampler binding.
            const auto shaderSamplers = std::span<const std::shared_ptr<Sampler>>(samplers).subspan(firstSampler);
            for (const auto& [texture, mask] : pairedSamplers) RequireFilterMinmax(context, textures[texture]->ViewFormat(), mask, shaderSamplers);
        }
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
        timing.Mark("memory_upload");
        std::vector<VkDescriptorSetLayoutBinding> description;
        for (const auto& binding : bindings) {
            description.push_back(binding.layout);
            layoutKey.insert(layoutKey.end(), {binding.layout.binding, static_cast<std::uint32_t>(binding.layout.descriptorType), binding.layout.descriptorCount, binding.layout.stageFlags});
        }
        std::vector<VkDescriptorPoolSize> sizes;
        if (storageBuffers != 0) sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<std::uint32_t>(storageBuffers)});
        if (!textures.empty()) sizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, static_cast<std::uint32_t>(textures.size())});
        if (!samplers.empty()) sizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLER, static_cast<std::uint32_t>(samplers.size())});
        if (!storageImages.empty()) sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, static_cast<std::uint32_t>(storageImages.size())});
        if (!context.descriptorCache) context.descriptorCache = std::make_shared<DescriptorCache>();
        descriptors = context.descriptorCache->Take(layoutKey);
        if (!descriptors) descriptors = std::make_unique<DescriptorAllocation>(context, layoutKey, description, sizes);
        _layout = descriptors->Layout();
        _set = descriptors->Set();
        timing.Mark("descriptor_acquire");
        std::vector<VkDescriptorBufferInfo> buffers;
        std::vector<VkDescriptorImageInfo> images;
        std::vector<VkWriteDescriptorSet> writes;
        buffers.reserve(allocations.size());
        images.reserve(textures.size() + samplers.size() + storageImages.size());
        writes.reserve(bindings.size());
        for (const auto& binding : bindings) {
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
    try {
        GuestMemory::CheckRange(reinterpret_cast<const void*>(address), size, 1, false);
    } catch (const std::runtime_error&) {
        return unusable("unmapped range");
    }
    if (written) GuestMemory::CheckRange(reinterpret_cast<const void*>(address), size, 1, true);
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

void ShaderResources::addImageBinding(const ShaderRecompiler::DescriptorBinding& binding, VkShaderStageFlags flags, std::vector<Binding>& bindings) {
    Require(binding.count != 0, "empty descriptor binding");
    const bool sampledImage = binding.kind == ShaderRecompiler::DescriptorKind::SampledImage;
    const bool samplerKind = binding.kind == ShaderRecompiler::DescriptorKind::Sampler;
    if (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) {
        Require(binding.role == ShaderRecompiler::DescriptorRole::GuestImages, "storage image descriptor role disagrees with its kind");
        Require(binding.guestDescriptor.size() == static_cast<std::size_t>(binding.count) * 8, "guest storage image descriptor must contain 8 dwords per element");
        Require(binding.imageShape.has_value(), "guest storage image binding is missing an image shape");
        Require(binding.count <= context.limits.maxPerStageDescriptorStorageImages, "shader storage-image descriptors exceed per-stage limits");
        Binding item{{binding.binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, binding.count, flags, nullptr}, {}, {}};
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8, 8);
            // A null T# (no base address) in a typed heap: with nullDescriptor the element binds no
            // view, so its loads read zero and its stores are dropped, as on the hardware.
            if (words[0] == 0u && (words[1] & 0xffu) == 0u && context.nullDescriptors) {
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
            item.imageAllocations.push_back(storageImages.size() - 1);
        }
        Require(storageImages.size() <= context.limits.maxDescriptorSetStorageImages, "pipeline storage-image descriptors exceed device limits");
        bindings.push_back(std::move(item));
        return;
    }
    Require(sampledImage || samplerKind, std::string("unsupported descriptor kind ") + kindName(binding.kind) + " for role " + roleName(binding.role));
    Require((sampledImage && binding.role == ShaderRecompiler::DescriptorRole::GuestImages) || (samplerKind && binding.role == ShaderRecompiler::DescriptorRole::GuestSamplers), "guest image descriptor role disagrees with its kind");
    Require(binding.guestDescriptor.size() % binding.count == 0, "guest image descriptor size is not a multiple of the binding count");
    const auto elementWords = binding.guestDescriptor.size() / binding.count;

    Binding item{{binding.binding, sampledImage ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, binding.count, flags, nullptr}, {}, {}};

    if (sampledImage) {
        Require(elementWords == 8, "guest texture descriptor must contain 8 dwords");
        Require(binding.imageShape.has_value(), "guest image binding is missing an image shape");
        Require(context.detiler != nullptr, "device texture detiler is unavailable");
        Require(context.textureCache != nullptr, "device texture cache is unavailable");
        Require(binding.count <= context.limits.maxPerStageDescriptorSampledImages, "shader sampled-image descriptors exceed per-stage limits");
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
            // A null base address (BASE_ADDRESS, the low 40 bits of the first two dwords): a 1x1, one
            // level, one layer texture over zeroed guest memory, keyed by the patched descriptor.
            std::array<std::uint32_t, 8> nullWords{};
            const bool nullBase = words[0] == 0 && (words[1] & 0xffu) == 0;
            if (nullBase) {
                std::copy(words.begin(), words.end(), nullWords.begin());
                const auto base = NullTextureMemory() >> 8u;
                // An all-zero descriptor (no texture bound) has no type or format either: a linear
                // 8_8_8_8 UNORM image of the shader's declared shape whose selects read zero.
                const auto type = (nullWords[3] >> 28u) & 0xfu;
                const auto format = (nullWords[1] >> 20u) & 0x1ffu;
                if (type < 8u || format == 0u) {
                    nullWords = {};
                    std::uint32_t shapeType = 9u;
                    switch (*binding.imageShape) {
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
            const bool twoDimensionalShape = *binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2D || *binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2DArray;
            if (twoDimensionalShape && (resource.dimension == TextureDimension::k1D || resource.dimension == TextureDimension::k1DArray)) {
                resource.dimension = resource.dimension == TextureDimension::k1D ? TextureDimension::k2D : TextureDimension::k2DArray;
                resource.height = 1;
            }
            const auto view = SampledViewDimension(*binding.imageShape, resource.dimension);
            Require(view.has_value(), "guest texture dimension " + std::to_string(static_cast<int>(resource.dimension)) + " (" + std::to_string(resource.width) + "x" + std::to_string(resource.height) + ") cannot be viewed with the shader's declared image shape " + std::to_string(static_cast<int>(*binding.imageShape)));
            resource.viewDimension = *view;
            // A converted format's shader applies DST_SEL itself (upstream baa202d2): its view keeps
            // the packed dword in X.
            const VkComponentMapping components = IsConvertedTextureFormat(resource.format)
                ? VkComponentMapping{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A}
                : VkComponentMapping{ComponentSwizzleFor(resource.dstSelX), ComponentSwizzleFor(resource.dstSelY), ComponentSwizzleFor(resource.dstSelZ), ComponentSwizzleFor(resource.dstSelW)};
            const bool depthCompare = element < binding.imageDepthCompare.size() && binding.imageDepthCompare[element];
            auto texture = context.textureCache->Get(words, resource, components, depthCompare);
            // A texture sampled with unnormalized coordinates (upstream 76ddd9b6): the host sampler
            // selects the S#'s texels only on a single-level, single-layer 1D or 2D view of mip 0.
            if (element < binding.imageUnnormalized.size() && binding.imageUnnormalized[element]) {
                const auto range = texture->SampledViewRange();
                const bool singleLevel = range.levels == 1u && range.layers == 1u && resource.baseLevel == 0u && EffectiveMinLod(resource) == 0.0f;
                if (!singleLevel || (range.type != VK_IMAGE_VIEW_TYPE_1D && range.type != VK_IMAGE_VIEW_TYPE_2D)) throw std::runtime_error("AGC graphics: guest texture sampled with unnormalized coordinates is not a single-level, single-layer 1D or 2D view starting at mip 0, which is not implemented (base level " + std::to_string(resource.baseLevel) + ", levels " + std::to_string(range.levels) + ", layers " + std::to_string(range.layers) + ", view type " + std::to_string(static_cast<int>(range.type)) + ")");
            }
            // The sampler elements paired with it, checked once the shader's samplers are bound.
            pairedSamplers.push_back({textures.size(), element < binding.imageSamplers.size() ? binding.imageSamplers[element] : 0u});
            textures.push_back(std::move(texture));
            item.imageAllocations.push_back(textures.size() - 1);
        }
        Require(textures.size() <= context.limits.maxDescriptorSetSampledImages, "pipeline sampled-image descriptors exceed device limits");
    } else {
        Require(elementWords == 4, "guest sampler descriptor must contain 4 dwords");
        Require(binding.count <= context.limits.maxPerStageDescriptorSamplers, "shader sampler descriptors exceed per-stage limits");
        Require(binding.samplerDepthCompare.size() == binding.count, "guest sampler binding is missing depth comparison metadata");
        Require(binding.samplerUnnormalized.empty() || binding.samplerUnnormalized.size() == binding.count, "guest sampler binding has unnormalized coordinate metadata of another size");
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
            const bool unnormalized = element < binding.samplerUnnormalized.size() && binding.samplerUnnormalized[element];
            auto resource = DecodeSamplerResource(words, unnormalized);
            resource.compareEnable = binding.samplerDepthCompare.at(element);
            if (!context.samplerCache) context.samplerCache = std::make_shared<SamplerCache>();
            samplers.push_back(context.samplerCache->Get(context, words, resource));
            item.imageAllocations.push_back(samplers.size() - 1);
        }
        Require(samplers.size() <= context.limits.maxDescriptorSetSamplers, "pipeline sampler descriptors exceed device limits");
    }

    bindings.push_back(std::move(item));
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
    if (bda) bda->CheckFault();
    guestMemory.WriteBack();
    for (const auto& image : storageImages) image->WriteBack();
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
