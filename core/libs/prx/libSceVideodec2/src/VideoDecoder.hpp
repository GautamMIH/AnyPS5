#ifndef CORE_LIBS_PRX_LIBSCEVIDEODEC2_SRC_VIDEODECODER_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEODEC2_SRC_VIDEODECODER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace Videodec2 {

enum class Codec { Avc, Hevc, Vp9 };

// A decoded picture, already stored as NV12 in the caller's buffer: a luma plane of pitch x
// alignedHeight bytes followed by interleaved chroma of pitch x alignedHeight / 2 bytes.
// width and height are the coded picture's (FFmpeg does not crop, as the console writes the coded
// picture: 1920x1088 for 1080p H.264); crop* is the stream's cropping to the display picture.
struct Picture {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pitch;
    std::uint32_t alignedHeight;
    std::uint64_t pts;
    std::uint64_t dts;
    std::uint64_t attachedData;
    // The stream's key-frame flag, profile and level (picture information).
    bool keyFrame = false;
    int profile = 0;
    int level = 0;
    std::uint32_t cropLeft = 0;
    std::uint32_t cropRight = 0;
    std::uint32_t cropTop = 0;
    std::uint32_t cropBottom = 0;
};

// Bytes of the NV12 layout for a picture: height aligned to 16 (shadPS4) and the pitch to 256 bytes,
// the row alignment of linear GPU surfaces, so a linear texture over the frame lines up with it.
std::uint32_t Nv12Pitch(std::uint32_t width);
std::uint64_t Nv12Bytes(std::uint32_t width, std::uint32_t height);

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    // Feeds one access unit and writes the next picture, if the decoder has one, into output.
    // Throws std::length_error when the picture does not fit and std::runtime_error on errors.
    virtual std::optional<Picture> Decode(std::span<const std::byte> accessUnit, std::uint64_t pts, std::uint64_t dts, std::uint64_t attachedData, std::span<std::byte> output) = 0;
    // Drains one buffered picture after the stream ended.
    virtual std::optional<Picture> Flush(std::span<std::byte> output) = 0;
    virtual void Reset() = 0;
};

// Returns null when this build has no video decoder.
std::unique_ptr<VideoDecoder> CreateVideoDecoder(Codec codec);

}

#endif
