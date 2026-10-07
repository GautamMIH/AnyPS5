#include "prx/libSceAgcDriver/Graphics/include/Sampler.hpp"
#include <string>

namespace AgcDriver::Graphics {

    Sampler::Sampler(const Context& context, const GuestSamplerResource& descriptor) : device(context.device), destroySampler(context.Function<PFN_vkDestroySampler>("vkDestroySampler")) {
        Require(!descriptor.anisotropyEnable || context.samplerAnisotropy, "guest sampler descriptor requests anisotropic filtering which the device does not support");
        Require(descriptor.maxAnisotropy <= context.limits.maxSamplerAnisotropy, "guest sampler descriptor requests an anisotropy ratio beyond the device limit");
        Require(descriptor.lodBias >= -context.limits.maxSamplerLodBias && descriptor.lodBias <= context.limits.maxSamplerLodBias, "guest sampler descriptor requests a LOD bias beyond the device limit");

        Require(!(descriptor.unnormalizedCoordinates && descriptor.compareEnable), "guest sampler descriptor with unnormalized coordinates enables depth comparison, which is not implemented");

        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        VkSamplerReductionModeCreateInfoEXT reduction{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT, nullptr, descriptor.reductionMode};
        if (descriptor.reductionMode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT) {
            Require(context.samplerFilterMinmax, "guest sampler descriptor uses a min or max reduction which the device does not support");
            Require(!descriptor.compareEnable, "guest sampler descriptor combines a min or max reduction with depth comparison, which is not implemented");
            info.pNext = &reduction;
            requiresFilterMinmax = descriptor.magFilter == VK_FILTER_LINEAR || descriptor.minFilter == VK_FILTER_LINEAR;
        }
        info.magFilter = descriptor.magFilter;
        info.minFilter = descriptor.minFilter;
        info.mipmapMode = descriptor.mipmapMode;
        info.addressModeU = descriptor.addressModeU;
        info.addressModeV = descriptor.addressModeV;
        info.addressModeW = descriptor.addressModeW;
        info.mipLodBias = descriptor.lodBias;
        info.anisotropyEnable = descriptor.anisotropyEnable ? VK_TRUE : VK_FALSE;
        info.maxAnisotropy = descriptor.maxAnisotropy;
        info.compareEnable = descriptor.compareEnable ? VK_TRUE : VK_FALSE;
        info.compareOp = descriptor.compareOp;
        info.minLod = descriptor.minLod;
        info.maxLod = descriptor.maxLod;
        info.borderColor = descriptor.borderColor;
        info.unnormalizedCoordinates = descriptor.unnormalizedCoordinates ? VK_TRUE : VK_FALSE;
        Check(context.Function<PFN_vkCreateSampler>("vkCreateSampler")(context.device, &info, nullptr, &sampler), "vkCreateSampler");
    }

    Sampler::~Sampler() {
        release();
    }

    void Sampler::release() noexcept {
        if (sampler) destroySampler(device, sampler, nullptr);
    }

    VkSampler Sampler::Handle() const {
        return sampler;
    }

    std::shared_ptr<Sampler> SamplerCache::Get(const Context& context, std::span<const std::uint32_t> words, const GuestSamplerResource& descriptor) {
        Require(words.size() == 4, "sampler cache descriptor must contain four DWORDs");
        const std::array<std::uint32_t, 5> key{words[0], words[1], words[2], words[3], (descriptor.compareEnable ? 1u : 0u) | (descriptor.unnormalizedCoordinates ? 2u : 0u)};
        std::lock_guard lock(mutex);
        const auto found = entries.find(key);
        if (found != entries.end()) return found->second;
        auto sampler = std::make_shared<Sampler>(context, descriptor);
        if (entries.size() >= 256) entries.erase(entries.begin());
        entries.emplace(key, sampler);
        return sampler;
    }

    void RequireFilterMinmax(const Context& context, VkFormat format, std::uint32_t samplerMask, std::span<const std::shared_ptr<Sampler>> samplers) {
        bool filtered = false;
        for (std::uint32_t element = 0; element < 32u; ++element) {
            if (((samplerMask >> element) & 1u) == 0u) continue;
            if (element >= samplers.size()) Require(false, "a sampled texture is paired with sampler element " + std::to_string(element) + ", which its shader does not bind");
            filtered = filtered || samplers[element]->RequiresFilterMinmax();
        }
        if (!filtered) return;
        VkFormatProperties properties{};
        context.formatProperties(context.physical, format, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT_EXT) == 0) Require(false, "a sampled texture whose format " + std::to_string(format) + " does not support min/max filtering is sampled through a min or max reduction sampler with linear filtering");
    }

}
