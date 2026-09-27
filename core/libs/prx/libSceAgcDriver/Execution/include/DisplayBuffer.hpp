#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DISPLAYBUFFER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DISPLAYBUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace AgcDriver {

struct DisplayBuffer {
    std::uint64_t address;
    std::uint64_t pixelFormat;
    std::uint32_t width;
    std::uint32_t height;
};

// Channel layout of a 32-bit video-out surface; codes follow KytyPS5's video-out format table.
enum class DisplayPixelLayout {
    Bgra8,
    Rgba8,
    Rgb10A2,
    Bgr10A2
};

DisplayPixelLayout DecodeDisplayPixelFormat(std::uint64_t pixelFormat);
std::size_t DisplayBufferSize(const DisplayBuffer& buffer);
std::vector<std::byte> DecodeDisplayBuffer(const DisplayBuffer& buffer, std::span<const std::byte> source);
std::vector<std::byte> ReadDisplayBuffer(const DisplayBuffer& buffer);

}

extern "C" std::size_t AgcDriverDisplayBufferSize_nid_postfix(const AgcDriver::DisplayBuffer& buffer);

#endif
