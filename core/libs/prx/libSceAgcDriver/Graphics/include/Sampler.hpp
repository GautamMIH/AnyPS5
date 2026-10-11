#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SAMPLER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SAMPLER_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestSamplerResource.hpp"
#include <array>
#include <map>
#include <memory>
#include <span>
#include <mutex>

namespace AgcDriver::Graphics {

class Sampler {
public:
    Sampler(const Context& context, const GuestSamplerResource& descriptor);
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    VkSampler Handle() const;
    // A min or max reduction with linear filtering: sampled views need SAMPLED_IMAGE_FILTER_MINMAX.
    bool RequiresFilterMinmax() const { return requiresFilterMinmax; }
    // FORCE_DEGAMMA (paired textures must be sRGB), an opaque black border through a clamp-to-border
    // axis, and DISABLE_CUBE_WRAP on a device without VK_EXT_non_seamless_cube_map.
    bool ForcesDegamma() const { return forcesDegamma; }
    bool ReadsOpaqueBlackBorder() const { return opaqueBlackBorder; }
    bool RequiresNonSeamlessCube() const { return requiresNonSeamlessCube; }

private:
    void release() noexcept;

    VkDevice device;
    PFN_vkDestroySampler destroySampler;
    VkSampler sampler = VK_NULL_HANDLE;
    bool requiresFilterMinmax = false;
    bool forcesDegamma = false;
    bool opaqueBlackBorder = false;
    bool requiresNonSeamlessCube = false;
};

class SamplerCache {
public:
    std::shared_ptr<Sampler> Get(const Context& context, std::span<const std::uint32_t> words, const GuestSamplerResource& descriptor);

private:
    std::mutex mutex;
    std::map<std::array<std::uint32_t, 5>, std::shared_ptr<Sampler>> entries;
};

// Throws when a texture of the view format is sampled through a min or max reduction sampler with
// linear filtering (samplers: the shader's sampler binding elements; samplerMask: the elements paired
// with the texture, DescriptorBinding::imageSamplers) and the format lacks min/max filtering.
void RequireFilterMinmax(const Context& context, VkFormat format, std::uint32_t samplerMask, std::span<const std::shared_ptr<Sampler>> samplers);
// A texture of a non-sRGB guest format sampled through a FORCE_DEGAMMA sampler (upstream 619ec7ad).
void RequireDegammaFormat(std::uint32_t guestFormat, std::uint32_t samplerMask, std::span<const std::shared_ptr<Sampler>> samplers);
// A BC swizzle that moves alpha, sampled through an opaque black border (upstream 85016605).
void RequireBorderSwizzle(std::uint32_t bcSwizzle, std::uint32_t samplerMask, std::span<const std::shared_ptr<Sampler>> samplers);
// A cube texture through a non-seamless sampler the device cannot create (upstream bf184200).
void RequireNonSeamlessCube(bool cube, std::uint32_t samplerMask, std::span<const std::shared_ptr<Sampler>> samplers);

}

#endif
