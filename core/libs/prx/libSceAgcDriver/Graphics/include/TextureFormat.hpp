#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTUREFORMAT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTUREFORMAT_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <cstdint>

namespace AgcDriver::Graphics {

VkFormat ResolveTextureFormat(std::uint32_t guestFormat);
std::uint32_t BytesPerElement(std::uint32_t guestFormat);
// A guest format without a Vulkan equivalent, read and written through another format's view with
// the shader converting each texel (10_11_11 UNORM and UINT through R32_UINT).
bool IsConvertedTextureFormat(std::uint32_t guestFormat);
// The 8 and 8_8 sRGB formats (ShaderRecompiler::SrgbDecodeBit) the device cannot sample but can
// sample as UNORM: the shader decodes them (SpirvTarget::srgbDecodeFormats). APS5_SRGB_SHADER_DECODE=1
// decodes them in the shader on every device.
std::uint32_t SrgbDecodeFormats(PFN_vkGetPhysicalDeviceFormatProperties formatProperties, VkPhysicalDevice physical);
// The format a sampled texture of the guest format is created and viewed with: the UNORM format of an
// sRGB one the shader decodes, else ResolveTextureFormat.
VkFormat SampledTextureFormat(std::uint32_t srgbDecodeFormats, std::uint32_t guestFormat);
// The format a storage image of the given format is created and viewed with, matching how the
// recompiler declares it: R64_UINT for 64-bit atomics on a 32_32 surface, R32_UINT for 32-bit atomics
// on a 32-bit SINT or FLOAT one, and the same-size UINT format for a SINT one (SINT storage images are
// only written, through UINT image types).
VkFormat StorageImageFormat(VkFormat format, bool atomic, bool atomic64);
bool IsBlockCompressed(std::uint32_t guestFormat);
std::uint32_t BlockWidth(std::uint32_t guestFormat);
std::uint32_t BlockHeight(std::uint32_t guestFormat);

}

#endif
