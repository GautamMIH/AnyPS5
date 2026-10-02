#ifndef CORE_LIBS_PRX_LIBSCEAUDIODEC_NATIVE_SRC_CODECS_HPP
#define CORE_LIBS_PRX_LIBSCEAUDIODEC_NATIVE_SRC_CODECS_HPP

#include <cstddef>
#include <cstdint>
#include <memory>

// MP3 (FFmpeg) and ATRAC9 (LibAtrac9) decoders for libSceAudiodec, one access unit per call.
namespace Audiodec {

constexpr std::int32_t WORD_SIZE_16BIT = 1;
constexpr std::int32_t WORD_SIZE_FLOAT = 2;

enum class DecodeStatus {
    Ok,
    InvalidData,
    PartialInput,
    NotEnoughRoom,
};

struct Mp3Header {
    std::uint32_t word;
    std::size_t frameBytes;
    bool crc;
    std::uint8_t mode;
    std::uint8_t modeExtension;
    std::uint8_t copyright;
    std::uint8_t original;
    std::uint8_t emphasis;
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::Ok;
    std::size_t consumed = 0;
    std::size_t produced = 0;
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    Mp3Header mp3{};
};

struct At9Format {
    std::uint32_t channels;
    std::uint32_t sampleRate;
    std::uint32_t superframeSize;
    std::uint32_t framesInSuperframe;
    std::uint32_t frameSamples;
};

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual DecodeResult Decode(const std::uint8_t* data, std::size_t size, std::uint8_t* pcm, std::size_t pcmSize) = 0;
    virtual void Reset() = 0;
};

bool ParseMp3Header(const std::uint8_t* data, std::size_t size, Mp3Header& header);
bool ValidAt9Config(const std::uint8_t (&config)[4]);
std::unique_ptr<Decoder> CreateMp3(std::int32_t wordSize);
std::unique_ptr<Decoder> CreateAt9(std::int32_t wordSize, const std::uint8_t (&config)[4], At9Format& format);

}

#endif
