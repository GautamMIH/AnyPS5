#include "src/AacDecoder.hpp"

#ifdef APS5_HAVE_FFMPEG

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

namespace Audiodec {
namespace {

// Syntax elements that open a raw_data_block (ISO/IEC 14496-3, table 4.85).
constexpr unsigned kSingleChannelElement = 0;
constexpr unsigned kChannelPairElement = 1;
constexpr int kAudioObjectTypeLowComplexity = 2;
constexpr unsigned kPrimingFrames = 2;

std::string errorText(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, text, sizeof(text));
    return text;
}

float sampleAt(const AVFrame& frame, int channel, int index) {
    const auto format = static_cast<AVSampleFormat>(frame.format);
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    const int channels = frame.ch_layout.nb_channels;
    const auto* data = planar ? frame.extended_data[channel] : frame.extended_data[0];
    const int position = planar ? index : index * channels + channel;
    switch (av_get_packed_sample_fmt(format)) {
        case AV_SAMPLE_FMT_FLT: return reinterpret_cast<const float*>(data)[position];
        case AV_SAMPLE_FMT_S16: return reinterpret_cast<const std::int16_t*>(data)[position] / 32768.0f;
        case AV_SAMPLE_FMT_S32: return static_cast<float>(reinterpret_cast<const std::int32_t*>(data)[position] / 2147483648.0);
        default: throw std::runtime_error("AAC decoder produced an unsupported sample format");
    }
}

class FfmpegAacDecoder final : public AacDecoder {
public:
    explicit FfmpegAacDecoder(const AacConfig& config) : config(config) {
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (packet == nullptr || frame == nullptr) {
            release();
            throw std::bad_alloc();
        }
        // ADTS headers describe the stream, so the decoder opens now; raw streams wait for their
        // first access unit, which reveals the channel configuration.
        if (config.adts) open(-1);
    }

    ~FfmpegAacDecoder() override {
        release();
    }

    void Reset() override {
        if (context != nullptr) avcodec_flush_buffers(context);
        primingLeft = config.nonDelayOutput ? 0u : kPrimingFrames;
    }

    AacFrame Decode(std::span<const std::byte> accessUnit, std::span<std::byte> pcm) override {
        if (accessUnit.empty()) throw std::runtime_error("empty AAC access unit");
        AacFrame result;
        result.consumedBytes = config.adts ? adtsFrameBytes(accessUnit) : accessUnit.size();
        const auto unit = accessUnit.first(result.consumedBytes);
        if (context == nullptr) openRaw(unit);
        else decodeUnit(unit);
        result.channels = static_cast<std::uint32_t>(frame->ch_layout.nb_channels);
        result.sampleRate = static_cast<std::uint32_t>(frame->sample_rate);
        result.heaac = context->profile == AV_PROFILE_AAC_HE || context->profile == AV_PROFILE_AAC_HE_V2;
        result.bitrate = static_cast<std::uint32_t>(result.consumedBytes * 8u * result.sampleRate / static_cast<std::uint64_t>(frame->nb_samples));
        if (primingLeft != 0) {
            --primingLeft;
            return result;
        }
        const auto sampleBytes = config.floatOutput ? sizeof(float) : sizeof(std::int16_t);
        const auto samples = static_cast<std::size_t>(frame->nb_samples);
        result.pcmBytes = samples * result.channels * sampleBytes;
        if (result.pcmBytes > pcm.size()) throw std::length_error("decoded AAC frame exceeds the PCM buffer");
        auto* output = pcm.data();
        for (std::size_t index = 0; index < samples; ++index) {
            for (std::uint32_t channel = 0; channel < result.channels; ++channel) {
                const auto value = sampleAt(*frame, static_cast<int>(channel), static_cast<int>(index));
                if (config.floatOutput) {
                    std::memcpy(output, &value, sizeof(value));
                } else {
                    const auto scaled = static_cast<std::int16_t>(std::lrint(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
                    std::memcpy(output, &scaled, sizeof(scaled));
                }
                output += sampleBytes;
            }
        }
        return result;
    }

private:
    AacConfig config;
    AVCodecContext* context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    std::vector<std::byte> padded;
    unsigned primingLeft = config.nonDelayOutput ? 0u : kPrimingFrames;

    static std::size_t adtsFrameBytes(std::span<const std::byte> unit) {
        const auto byte = [&](std::size_t index) { return std::to_integer<unsigned>(unit[index]); };
        if (unit.size() < 7 || byte(0) != 0xff || (byte(1) & 0xf0u) != 0xf0u) throw std::runtime_error("AAC access unit lacks an ADTS header");
        const auto length = ((byte(3) & 3u) << 11u) | (byte(4) << 3u) | (byte(5) >> 5u);
        if (length < 7 || length > unit.size()) throw std::runtime_error("ADTS frame length exceeds the access unit");
        return length;
    }

    // channelConfiguration < 0 opens without an AudioSpecificConfig (ADTS).
    void open(int channelConfiguration) {
        const auto* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        if (codec == nullptr) throw std::runtime_error("FFmpeg has no AAC decoder");
        context = avcodec_alloc_context3(codec);
        if (context == nullptr) throw std::bad_alloc();
        if (channelConfiguration >= 0) {
            // AudioSpecificConfig: object type (5 bits), frequency index (4), channels (4), then
            // the GA specific config's three zero flags.
            const auto config16 = static_cast<unsigned>((kAudioObjectTypeLowComplexity << 11) | (config.samplingFrequencyIndex << 7) | (static_cast<unsigned>(channelConfiguration) << 3));
            context->extradata = static_cast<std::uint8_t*>(av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE));
            if (context->extradata == nullptr) throw std::bad_alloc();
            context->extradata[0] = static_cast<std::uint8_t>(config16 >> 8u);
            context->extradata[1] = static_cast<std::uint8_t>(config16);
            context->extradata_size = 2;
        }
        if (const auto error = avcodec_open2(context, codec, nullptr); error < 0) {
            avcodec_free_context(&context);
            throw std::runtime_error("cannot open the AAC decoder: " + errorText(error));
        }
    }

    // A raw stream does not signal its channel configuration; the first element narrows it (a
    // channel pair opens stereo, a single channel mono or a multichannel layout), and the first
    // configuration that decodes cleanly is kept.
    void openRaw(std::span<const std::byte> unit) {
        const auto element = std::to_integer<unsigned>(unit[0]) >> 5u;
        std::vector<int> candidates;
        if (element == kChannelPairElement) candidates = {2};
        else if (element == kSingleChannelElement) candidates = {1, 3, 4, 5, 6, 7};
        else throw std::runtime_error("raw AAC access unit does not start with a channel element");
        std::string lastError;
        for (const auto candidate : candidates) {
            open(candidate);
            try {
                decodeUnit(unit);
                return;
            } catch (const std::runtime_error& error) {
                lastError = error.what();
                avcodec_free_context(&context);
            }
        }
        throw std::runtime_error("no AAC channel configuration decodes the stream: " + lastError);
    }

    void decodeUnit(std::span<const std::byte> unit) {
        padded.assign(unit.size() + AV_INPUT_BUFFER_PADDING_SIZE, std::byte{});
        std::copy(unit.begin(), unit.end(), padded.begin());
        packet->data = reinterpret_cast<std::uint8_t*>(padded.data());
        packet->size = static_cast<int>(unit.size());
        if (const auto error = avcodec_send_packet(context, packet); error < 0) throw std::runtime_error("AAC decode failed: " + errorText(error));
        if (const auto error = avcodec_receive_frame(context, frame); error < 0) throw std::runtime_error("AAC decode produced no frame: " + errorText(error));
        if (frame->ch_layout.nb_channels <= 0 || frame->nb_samples <= 0) throw std::runtime_error("AAC decode produced an empty frame");
    }

    void release() noexcept {
        if (context != nullptr) avcodec_free_context(&context);
        if (frame != nullptr) av_frame_free(&frame);
        if (packet != nullptr) av_packet_free(&packet);
    }
};

}

std::unique_ptr<AacDecoder> CreateAacDecoder(const AacConfig& config) {
    return std::make_unique<FfmpegAacDecoder>(config);
}

}

#else

namespace Audiodec {

std::unique_ptr<AacDecoder> CreateAacDecoder(const AacConfig&) {
    return nullptr;
}

}

#endif
