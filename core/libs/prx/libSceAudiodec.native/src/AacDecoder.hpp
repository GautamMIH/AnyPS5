#ifndef CORE_LIBS_PRX_LIBSCEAUDIODEC_NATIVE_SRC_AACDECODER_HPP
#define CORE_LIBS_PRX_LIBSCEAUDIODEC_NATIVE_SRC_AACDECODER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Audiodec {

struct AacConfig {
    // ADTS streams carry their own headers; raw access units are described by the sampling
    // frequency index alone.
    bool adts;
    std::uint32_t samplingFrequencyIndex;
    bool floatOutput;
    // Without non-delay output the console holds back the decoder's first two frames (shadPS4).
    bool nonDelayOutput;
};

struct AacFrame {
    std::size_t consumedBytes = 0;
    std::size_t pcmBytes = 0;
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    // Bits per second of this access unit.
    std::uint32_t bitrate = 0;
    bool heaac = false;
};

class AacDecoder {
public:
    virtual ~AacDecoder() = default;
    // Decodes one access unit into interleaved PCM. Throws std::length_error when the PCM does not
    // fit and std::runtime_error when the unit cannot be decoded.
    virtual AacFrame Decode(std::span<const std::byte> accessUnit, std::span<std::byte> pcm) = 0;
    virtual void Reset() = 0;
};

// Returns null when this build has no AAC decoder.
std::unique_ptr<AacDecoder> CreateAacDecoder(const AacConfig& config);

}

#endif
