#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADERRESOURCES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADERRESOURCES_HPP

#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BdaResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Sampler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/StorageImage.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DescriptorCache.hpp"
#include "Recompiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include <array>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

class ShaderResources {
public:
    ShaderResources(const Context& context, const ShaderRecompiler::RecompileResult& vertex, const ShaderRecompiler::RecompileResult& fragment, std::span<const ColorTarget> targets, std::uint64_t indexAddress, std::size_t indexBytes);
    ShaderResources(const Context& context, std::span<const CompiledShader> shaders, std::span<const ColorTarget> targets, std::uint64_t indexAddress, std::size_t indexBytes, std::span<const GuestMemorySnapshot> snapshots = {});
    ShaderResources(const Context& context, const CompiledShader& compute, std::span<const GuestMemorySnapshot> snapshots = {});
    ~ShaderResources();
    ShaderResources(const ShaderResources&) = delete;
    ShaderResources& operator=(const ShaderResources&) = delete;
    VkDescriptorSetLayout Layout() const;
    void Bind(VkCommandBuffer commands, VkPipelineBindPoint bindPoint, VkPipelineLayout layout) const;
    // Storage images move between guest memory and host images around the draw or dispatch:
    // uploads are recorded before it (outside any render pass) and downloads after it.
    void RecordUploads(VkCommandBuffer commands) const;
    // Whether RecordUploads or RecordDownloads record commands (storage images, direct depth views).
    bool RecordsOutsidePass() const;
    void RecordDownloads(VkCommandBuffer commands) const;
    void WriteBack();
    bool WritesOverlap(std::uint64_t address, std::size_t bytes) const;
    // Appends every guest range the work may write, as [begin, end).
    void AppendWrites(std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) const;
    const std::vector<std::uint32_t>& LayoutKey() const { return layoutKey; }
    // A texture views the depth target the work renders to (Texture::FeedbackView): the work's render
    // pass uses the depth attachment read-only.
    bool DepthFeedback() const {
        for (const auto& texture : textures) {
            if (texture->DepthFeedback()) return true;
        }
        return false;
    }

private:
    struct Allocation {
        std::uint64_t address;
        std::size_t size;
        bool guest;
        std::unique_ptr<Buffer> buffer;
        ShaderRecompiler::DescriptorRole role = ShaderRecompiler::DescriptorRole::ShaderData;
        // Shader data placed in a chunk shared with other draws (DrawQueue::TakeData), at chunkOffset.
        std::shared_ptr<Buffer> chunk;
        VkDeviceSize chunkOffset = 0;
    };

    struct Binding {
        VkDescriptorSetLayoutBinding layout;
        std::vector<std::size_t> allocations;
        std::vector<std::size_t> imageAllocations;
        // Table bindings (partially bound; the image table with a variable count): their elements
        // are written in runs, skipping the unbound ones (NoElement).
        VkDescriptorBindingFlags flags = 0;
    };
    static constexpr std::size_t NoElement = std::numeric_limits<std::size_t>::max();

    // A stage's image and sampler tables (DescriptorRole::ImageTable, SamplerTable, ImageTableMap):
    // the map the shader reads (its element base and the driver's per-entry fault codes filled
    // here) and the poison entries a table fault names.
    struct TableStage {
        ShaderRecompiler::ShaderStage stage = ShaderRecompiler::ShaderStage::Compute;
        const ShaderRecompiler::RecompileResult* program = nullptr;
        const ShaderRecompiler::DescriptorBinding* images = nullptr;
        std::span<const std::uint32_t> imageWords;
        std::size_t mapBinding = NoElement;
        std::vector<std::uint32_t> map;
        std::vector<ShaderRecompiler::ImageTableEntryPoison> poison;
        std::vector<std::string> messages;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> samplerRejections;
    };
    TableStage& tableStage(const CompiledShader& shader);
    std::uint32_t rejectTableEntry(TableStage& table, std::span<const std::uint32_t> words, const std::string& message);
    void addSamplerTable(const ShaderRecompiler::DescriptorBinding& binding, std::span<const std::uint32_t> words, VkShaderStageFlags flags, TableStage& table, std::vector<Binding>& bindings);
    void resolveImageTable(TableStage& table, std::size_t firstSampler);
    void finishImageTables(std::vector<Binding>& bindings);
    std::vector<TableStage> tableStages;
    // The image table binding's elements (every stage's table at its element base): texture indices.
    std::vector<std::size_t> tableTextures;
    VkShaderStageFlags tableStageFlags = 0;
    std::uint32_t tableElements = 0;

    // A sampled texture looked up for a T# as a shader of the given image shape reads it, with what
    // the sampler pairing checks need.
    struct SampledLookup {
        std::shared_ptr<Texture> texture;
        bool singleLevel;
        bool cube;
        std::uint32_t bcSwizzle;
        std::uint32_t format;
    };
    SampledLookup lookupSampledTexture(std::span<const std::uint32_t> words, ShaderRecompiler::DescriptorImageShape shape, bool depthCompare, bool unnormalized);

    void build(std::span<const CompiledShader> shaders, std::span<const ColorTarget> targets, std::uint64_t indexAddress, std::size_t indexBytes);
    std::size_t addGuestBuffer(std::span<const std::uint32_t> words, std::span<const ColorTarget> targets, std::uint64_t indexAddress, std::size_t indexBytes, bool written);
    std::size_t addDataBuffer(std::span<const std::uint32_t> words);
    void addImageBinding(const ShaderRecompiler::DescriptorBinding& binding, std::span<const std::uint32_t> words, VkShaderStageFlags flags, std::vector<Binding>& bindings);
    void release() noexcept;
    void prepareAddressBindings(std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots);
    VkDescriptorBufferInfo descriptor(const Allocation& allocation) const;
    Context context;
    std::vector<std::uint32_t> layoutKey;
    GuestBufferMemory guestMemory;
    std::unique_ptr<BdaResources> bda;
    bool usesBda = false;
    bool usesFaultBuffer = false;
    VkDescriptorSetLayout _layout = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    std::unique_ptr<DescriptorAllocation> descriptors;
    std::vector<Allocation> allocations;
    std::vector<std::shared_ptr<Texture>> textures;
    std::vector<std::shared_ptr<Sampler>> samplers;
    // While a shader's bindings are added: its sampled textures (index into textures) and the masks
    // of the sampler elements paired with each (DescriptorBinding::imageSamplers).
    // A sampled texture of the shader being bound with the sampler elements paired with it (mask), and
    // what the pairing checks need: one level, cube, BC swizzle, guest format.
    struct PairedTexture {
        std::size_t texture;
        std::uint32_t mask;
        bool singleLevel;
        bool cube;
        std::uint32_t bcSwizzle;
        std::uint32_t format;
    };
    std::vector<PairedTexture> pairedSamplers;
    // Each bound sampler's S# words and decode inputs (index as samplers), for ANISO_OVERRIDE variants.
    struct SamplerInput {
        std::array<std::uint32_t, 4> words;
        bool compareEnable;
        bool unnormalized;
        // DescriptorBinding::samplerIntegerBorder: its border color is read as an integer.
        bool integerBorder = false;
    };
    std::vector<SamplerInput> samplerInputs;
    std::vector<std::unique_ptr<StorageImage>> storageImages;
    // Whether the shader may store to each storage image (DescriptorBinding::imageWritten).
    std::vector<bool> storageWritten;
};

}

#endif
