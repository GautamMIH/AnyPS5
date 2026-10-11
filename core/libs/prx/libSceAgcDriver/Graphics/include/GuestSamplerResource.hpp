#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTSAMPLERRESOURCE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTSAMPLERRESOURCE_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace AgcDriver::Graphics {

struct GuestSamplerResource {
    VkFilter magFilter;
    VkFilter minFilter;
    VkSamplerMipmapMode mipmapMode;
    VkSamplerAddressMode addressModeU;
    VkSamplerAddressMode addressModeV;
    VkSamplerAddressMode addressModeW;
    bool anisotropyEnable;
    float maxAnisotropy;
    float minLod;
    float maxLod;
    float lodBias;
    VkBorderColor borderColor;
    // FILTER_MODE 1 and 2: min and max reduction (VK_EXT_sampler_filter_minmax).
    VkSamplerReductionMode reductionMode = VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT;
    bool compareEnable = false;
    VkCompareOp compareOp = VK_COMPARE_OP_NEVER;
    bool unnormalizedCoordinates = false;
    // FORCE_DEGAMMA: decode sRGB, which an sRGB texture's view already does (Sampler::ForcesDegamma).
    bool forceDegamma = false;
    // DISABLE_CUBE_WRAP: cube faces filter without seamless wrapping (VK_EXT_non_seamless_cube_map).
    bool nonSeamlessCube = false;
};

// unnormalizedProven: the recompiler proved every use of the S# selects the same texels through a
// Vulkan sampler with unnormalized coordinates (DescriptorBinding::samplerUnnormalized); an S# with
// FORCE_UNNORMALIZED throws without it, and the proof without the bit throws too.
GuestSamplerResource DecodeSamplerResource(std::span<const std::uint32_t> words, bool unnormalizedProven = false);
// ANISO_OVERRIDE turns anisotropic filtering off for single-level images (the T#'s own BASE_LEVEL ==
// LAST_LEVEL): the S# words without the aniso filters when that applies to a sampler paired only with
// single-level images, nullopt when it does not apply. Throws for a sampler paired with both kinds.
std::optional<std::array<std::uint32_t, 4>> SingleLevelSamplerWords(std::span<const std::uint32_t, 4> words, bool singleLevelImage, bool mipmappedImage);
// Whether a T#'s view has one level (BASE_LEVEL == LAST_LEVEL in word 3).
inline bool DescriptorSingleLevel(std::span<const std::uint32_t> words) { return ((words[3] >> 12u) & 0xfu) == ((words[3] >> 16u) & 0xfu); }

}

#endif
