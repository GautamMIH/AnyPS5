// Guest resource decoding ported from upstream (S# reduction modes, unnormalized coordinates and
// border colour tables; texture formats 6 and 30, sRGB shader decode and storage image formats),
// checked without a device.
#include "prx/libSceAgcDriver/Graphics/include/GuestSamplerResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace AgcDriver::Graphics;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
void Reject(TAction action, std::string_view reason) {
    try {
        action();
    } catch (const std::exception& error) {
        Require(std::string_view(error.what()).find(reason) != std::string_view::npos, std::string("unexpected rejection: ") + error.what() + " (expected " + std::string(reason) + ")");
        return;
    }
    throw std::runtime_error("expected a rejection: " + std::string(reason));
}

// An S#: clamp-to-edge on every axis, point filters, no mips.
struct SamplerWords {
    std::array<std::uint32_t, 4> words{2u | (2u << 3u) | (2u << 6u), 0u, 0u, 0u};

    SamplerWords& clamp(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        words[0] = (words[0] & ~0x1ffu) | x | (y << 3u) | (z << 6u);
        return *this;
    }
    SamplerWords& word0Bit(std::uint32_t bit) { words[0] |= 1u << bit; return *this; }
    SamplerWords& filterMode(std::uint32_t mode) { words[0] = (words[0] & ~(3u << 29u)) | (mode << 29u); return *this; }
    SamplerWords& filters(std::uint32_t mag, std::uint32_t min) { words[2] = (words[2] & ~(0xfu << 20u)) | (mag << 20u) | (min << 22u); return *this; }
    SamplerWords& mipFilter(std::uint32_t filter) { words[2] = (words[2] & ~(3u << 26u)) | (filter << 26u); return *this; }
    SamplerWords& lods(std::uint32_t minLod, std::uint32_t maxLod) { words[1] = minLod | (maxLod << 12u); return *this; }
    SamplerWords& border(std::uint32_t type) { words[3] = (words[3] & ~(3u << 30u)) | (type << 30u); return *this; }
};

void reductionTests() {
    Require(DecodeSamplerResource(SamplerWords{}.words).reductionMode == VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT, "FILTER_MODE 0 must be a weighted average");
    Require(DecodeSamplerResource(SamplerWords{}.filterMode(1).words).reductionMode == VK_SAMPLER_REDUCTION_MODE_MIN_EXT, "FILTER_MODE 1 must be a min reduction");
    Require(DecodeSamplerResource(SamplerWords{}.filterMode(2).filters(1, 1).words).reductionMode == VK_SAMPLER_REDUCTION_MODE_MAX_EXT, "FILTER_MODE 2 must be a max reduction");
    Reject([] { DecodeSamplerResource(SamplerWords{}.filterMode(3).words); }, "unknown reduction filter mode 3");
    Reject([] { DecodeSamplerResource(SamplerWords{}.filterMode(1).filters(2, 2).words); }, "min or max reduction with anisotropic filtering");
    Reject([] { DecodeSamplerResource(SamplerWords{}.filterMode(2).mipFilter(2).lods(0, 0x100).words); }, "min or max reduction with a linear mip filter");
    const auto pointMips = DecodeSamplerResource(SamplerWords{}.filterMode(1).mipFilter(1).lods(0, 0x100).words);
    Require(pointMips.reductionMode == VK_SAMPLER_REDUCTION_MODE_MIN_EXT && pointMips.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST, "a min reduction with point mips must decode");
}

void unnormalizedTests() {
    constexpr std::uint32_t forceUnnormalized = 15u;
    constexpr std::uint32_t mcCoordTrunc = 19u;
    constexpr std::uint32_t truncCoord = 27u;
    Reject([] { DecodeSamplerResource(SamplerWords{}.word0Bit(forceUnnormalized).words); }, "unnormalized coordinates which are not implemented");
    Reject([] { DecodeSamplerResource(SamplerWords{}.words, true); }, "bound as unnormalized without FORCE_UNNORMALIZED");
    const auto linear = DecodeSamplerResource(SamplerWords{}.word0Bit(forceUnnormalized).filters(1, 1).mipFilter(1).lods(0x100, 0x400).words, true);
    Require(linear.unnormalizedCoordinates && linear.magFilter == VK_FILTER_LINEAR && linear.minFilter == VK_FILTER_LINEAR, "a proven unnormalized S# must keep its filters");
    Require(linear.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST && linear.minLod == 0.0f && linear.maxLod == 0.0f && linear.lodBias == 0.0f && !linear.anisotropyEnable, "an unnormalized S# must sample mip 0 only, as Vulkan requires");
    const auto border = DecodeSamplerResource(SamplerWords{}.word0Bit(forceUnnormalized).clamp(6, 6, 0).words, true);
    Require(border.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER && border.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, "clamp-to-border must be kept with unnormalized coordinates");
    Require(!DecodeSamplerResource(SamplerWords{}.words).unnormalizedCoordinates, "a normalized S# must stay normalized");
    Reject([] { DecodeSamplerResource(SamplerWords{}.word0Bit(forceUnnormalized).filters(1, 0).words, true); }, "different minification and magnification filters");
    Reject([] { DecodeSamplerResource(SamplerWords{}.word0Bit(forceUnnormalized).filters(2, 2).words, true); }, "unnormalized coordinates with anisotropic filtering");
    Reject([] { DecodeSamplerResource(SamplerWords{}.word0Bit(forceUnnormalized).clamp(0, 2, 2).words, true); }, "clamp mode 0 on X");
    Reject([] { DecodeSamplerResource(SamplerWords{}.word0Bit(forceUnnormalized).clamp(2, 4, 2).words, true); }, "clamp mode 4 on Y");
    Reject([] { DecodeSamplerResource(SamplerWords{}.word0Bit(forceUnnormalized).word0Bit(truncCoord).words, true); }, "TRUNC_COORD");
    Reject([] { DecodeSamplerResource(SamplerWords{}.word0Bit(forceUnnormalized).word0Bit(mcCoordTrunc).words, true); }, "MC_COORD_TRUNC");
}

void borderTableTests() {
    for (const std::uint32_t mode : {0u, 1u, 2u, 3u}) {
        Require(DecodeSamplerResource(SamplerWords{}.clamp(mode, mode, mode).border(3).words).borderColor == VK_BORDER_COLOR_INT_TRANSPARENT_BLACK, "a border colour table no axis reads must decode (clamp mode " + std::to_string(mode) + ")");
    }
    for (const std::uint32_t mode : {4u, 5u, 6u, 7u}) {
        Reject([&] { DecodeSamplerResource(SamplerWords{}.clamp(2, 2, mode).border(3).words); }, "border color table");
        Reject([&] { DecodeSamplerResource(SamplerWords{}.clamp(mode, 2, 2).border(3).words); }, "border color table");
    }
    Require(DecodeSamplerResource(SamplerWords{}.clamp(6, 6, 6).border(2).words).borderColor == VK_BORDER_COLOR_INT_OPAQUE_WHITE, "border colour type 2 must stay opaque white");
}

VkFormat unsampledFormats[2]{};

void unsampledFormatProperties(VkPhysicalDevice, VkFormat format, VkFormatProperties* properties) {
    *properties = {};
    if (format != unsampledFormats[0] && format != unsampledFormats[1]) properties->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
}

std::uint32_t decodedWithout(VkFormat first = VK_FORMAT_UNDEFINED, VkFormat second = VK_FORMAT_UNDEFINED) {
    unsampledFormats[0] = first;
    unsampledFormats[1] = second;
    return SrgbDecodeFormats(unsampledFormatProperties, VK_NULL_HANDLE);
}

void textureFormatTests() {
    Require(ResolveTextureFormat(6) == VK_FORMAT_R8_SINT && BytesPerElement(6) == 1u, "format 6 must resolve to one-byte R8_SINT");
    Require(ResolveTextureFormat(30) == VK_FORMAT_R32_UINT && BytesPerElement(30) == 4u && IsConvertedTextureFormat(30), "format 30 (10_11_11_UNORM) must be read through R32_UINT");
    Require(ResolveTextureFormat(34) == VK_FORMAT_R32_UINT && IsConvertedTextureFormat(34), "format 34 (10_11_11_UINT) must be read through R32_UINT");
    Require(!IsConvertedTextureFormat(20) && !IsConvertedTextureFormat(6) && !IsConvertedTextureFormat(56), "formats with a Vulkan equivalent are not converted");

    constexpr std::uint32_t srgb8 = 1u;
    constexpr std::uint32_t srgb8_8 = 2u;
    Require(decodedWithout() == 0u, "a device that samples every sRGB format needs no shader decode");
    Require(decodedWithout(VK_FORMAT_R8G8_SRGB) == srgb8_8, "a device without sampled R8G8_SRGB must decode 8_8_SRGB in the shader");
    Require(decodedWithout(VK_FORMAT_R8_SRGB) == srgb8, "a device without sampled R8_SRGB must decode 8_SRGB in the shader");
    Require(decodedWithout(VK_FORMAT_R8_SRGB, VK_FORMAT_R8G8_SRGB) == (srgb8 | srgb8_8), "a device without either 8-bit sRGB format must decode both in the shader");
    Require(decodedWithout(VK_FORMAT_R8G8_SRGB, VK_FORMAT_R8G8_UNORM) == 0u, "8_8_SRGB without a sampled UNORM view must not be decoded in the shader");
    Require(decodedWithout(VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_BC1_RGBA_SRGB_BLOCK) == 0u, "only the 8 and 8_8 sRGB formats may be decoded in the shader");
    Require(SampledTextureFormat(0u, 129) == VK_FORMAT_R8G8_SRGB && SampledTextureFormat(0u, 128) == VK_FORMAT_R8_SRGB, "sRGB textures must keep their sRGB views without shader decode");
    Require(SampledTextureFormat(srgb8_8, 129) == VK_FORMAT_R8G8_UNORM && SampledTextureFormat(srgb8_8, 128) == VK_FORMAT_R8_SRGB, "only the decoded sRGB format is viewed as UNORM");
    Require(SampledTextureFormat(srgb8 | srgb8_8, 128) == VK_FORMAT_R8_UNORM, "8_SRGB decoded in the shader must be viewed as R8_UNORM");
    Require(SampledTextureFormat(srgb8 | srgb8_8, 130) == VK_FORMAT_R8G8B8A8_SRGB && SampledTextureFormat(srgb8 | srgb8_8, 14) == VK_FORMAT_R8G8_UNORM && SampledTextureFormat(srgb8 | srgb8_8, 170) == VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "formats without a shader decode must keep their views");

    const std::array<std::pair<VkFormat, VkFormat>, 8> sint{{
        {VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_UINT}, {VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_UINT},
        {VK_FORMAT_R16_SINT, VK_FORMAT_R16_UINT}, {VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_UINT},
        {VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_UINT}, {VK_FORMAT_R32_SINT, VK_FORMAT_R32_UINT},
        {VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_UINT}, {VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_UINT},
    }};
    for (const auto& [format, uint] : sint) Require(StorageImageFormat(format, false, false) == uint, "a SINT storage image of format " + std::to_string(format) + " must be bound through its UINT format");
    Require(StorageImageFormat(VK_FORMAT_R8G8B8A8_UNORM, false, false) == VK_FORMAT_R8G8B8A8_UNORM && StorageImageFormat(VK_FORMAT_R32_SFLOAT, false, false) == VK_FORMAT_R32_SFLOAT, "non-SINT storage images keep their format");
    Require(StorageImageFormat(VK_FORMAT_R32_SINT, true, false) == VK_FORMAT_R32_UINT && StorageImageFormat(VK_FORMAT_R32_SFLOAT, true, false) == VK_FORMAT_R32_UINT && StorageImageFormat(VK_FORMAT_R32_UINT, true, false) == VK_FORMAT_R32_UINT, "32-bit atomics use an R32_UINT view");
    for (const auto format : {VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_SFLOAT}) Require(StorageImageFormat(format, true, true) == VK_FORMAT_R64_UINT, "64-bit atomics on a 32_32 surface use an R64_UINT view");
}

}

int main() {
    try {
        reductionTests();
        unnormalizedTests();
        borderTableTests();
        textureFormatTests();
        std::puts("AGC resource decode tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
