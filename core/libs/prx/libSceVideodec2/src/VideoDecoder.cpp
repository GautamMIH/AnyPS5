#include "src/VideoDecoder.hpp"

namespace Videodec2 {

namespace {

std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

}

std::uint32_t Nv12Pitch(std::uint32_t width) {
    return alignUp(width, 256);
}

std::uint64_t Nv12Bytes(std::uint32_t width, std::uint32_t height) {
    return static_cast<std::uint64_t>(Nv12Pitch(width)) * alignUp(height, 16) * 3u / 2u;
}

}

#ifdef APS5_HAVE_FFMPEG

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace Videodec2 {
namespace {

std::string errorText(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, text, sizeof(text));
    return text;
}

class FfmpegVideoDecoder final : public VideoDecoder {
public:
    explicit FfmpegVideoDecoder(Codec codec) {
        const auto id = codec == Codec::Avc ? AV_CODEC_ID_H264 : codec == Codec::Hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_VP9;
        const auto* decoder = avcodec_find_decoder(id);
        if (decoder == nullptr) throw std::runtime_error(std::string("FFmpeg has no ") + avcodec_get_name(id) + " decoder");
        context = avcodec_alloc_context3(decoder);
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        converted = av_frame_alloc();
        if (context == nullptr || packet == nullptr || frame == nullptr || converted == nullptr) {
            release();
            throw std::bad_alloc();
        }
        // Attached data travels with its packet to the picture it produces.
        context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        if (const auto error = avcodec_open2(context, decoder, nullptr); error < 0) {
            release();
            throw std::runtime_error("cannot open the video decoder: " + errorText(error));
        }
    }

    ~FfmpegVideoDecoder() override {
        release();
    }

    std::optional<Picture> Decode(std::span<const std::byte> accessUnit, std::uint64_t pts, std::uint64_t dts, std::uint64_t attachedData, std::span<std::byte> output) override {
        padded.assign(accessUnit.size() + AV_INPUT_BUFFER_PADDING_SIZE, std::byte{});
        std::memcpy(padded.data(), accessUnit.data(), accessUnit.size());
        packet->data = reinterpret_cast<std::uint8_t*>(padded.data());
        packet->size = static_cast<int>(accessUnit.size());
        packet->pts = static_cast<std::int64_t>(pts);
        packet->dts = static_cast<std::int64_t>(dts);
        packet->opaque = reinterpret_cast<void*>(static_cast<std::uintptr_t>(attachedData));
        auto error = avcodec_send_packet(context, packet);
        if (error == AVERROR_EOF) {
            // A new stream after a flush.
            avcodec_flush_buffers(context);
            error = avcodec_send_packet(context, packet);
        }
        packet->data = nullptr;
        packet->size = 0;
        if (error < 0) throw std::runtime_error("video decode failed: " + errorText(error));
        return receive(output);
    }

    std::optional<Picture> Flush(std::span<std::byte> output) override {
        if (!draining) {
            avcodec_send_packet(context, nullptr);
            draining = true;
        }
        return receive(output);
    }

    void Reset() override {
        avcodec_flush_buffers(context);
        draining = false;
    }

private:
    AVCodecContext* context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* converted = nullptr;
    SwsContext* scaler = nullptr;
    std::vector<std::byte> padded;
    bool draining = false;

    std::optional<Picture> receive(std::span<std::byte> output) {
        const auto error = avcodec_receive_frame(context, frame);
        if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) return std::nullopt;
        if (error < 0) throw std::runtime_error("video decode produced no picture: " + errorText(error));
        const AVFrame* nv12 = frame;
        if (frame->format != AV_PIX_FMT_NV12) nv12 = convert();
        const auto width = static_cast<std::uint32_t>(nv12->width);
        const auto height = static_cast<std::uint32_t>(nv12->height);
        Picture picture{width, height, Nv12Pitch(width), alignUp(height, 16), static_cast<std::uint64_t>(frame->pts), static_cast<std::uint64_t>(frame->pkt_dts), static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(frame->opaque))};
        if (Nv12Bytes(width, height) > output.size()) {
            av_frame_unref(frame);
            throw std::length_error("decoded picture exceeds the frame buffer");
        }
        copyNv12(*nv12, picture, output.data());
        av_frame_unref(frame);
        return picture;
    }

    const AVFrame* convert() {
        scaler = sws_getCachedContext(scaler, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (scaler == nullptr) throw std::runtime_error("cannot convert the decoded picture to NV12");
        if (converted->width != frame->width || converted->height != frame->height || converted->format != AV_PIX_FMT_NV12) {
            av_frame_unref(converted);
            converted->format = AV_PIX_FMT_NV12;
            converted->width = frame->width;
            converted->height = frame->height;
            if (av_frame_get_buffer(converted, 0) < 0) throw std::bad_alloc();
        }
        if (av_frame_make_writable(converted) < 0) throw std::bad_alloc();
        if (sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, converted->data, converted->linesize) < 0) throw std::runtime_error("NV12 conversion failed");
        return converted;
    }

    // Rows past the picture repeat its last row, filling the aligned height.
    static void copyNv12(const AVFrame& source, const Picture& picture, std::byte* destination) {
        const auto chromaRows = (picture.height + 1u) / 2u;
        const auto rowBytes = (picture.width + 1u) & ~1u;
        auto* luma = destination;
        auto* chroma = destination + static_cast<std::size_t>(picture.pitch) * picture.alignedHeight;
        for (std::uint32_t y = 0; y < picture.alignedHeight; ++y) {
            const auto row = std::min(y, picture.height - 1u);
            std::memcpy(luma + static_cast<std::size_t>(y) * picture.pitch, source.data[0] + static_cast<std::ptrdiff_t>(row) * source.linesize[0], picture.width);
        }
        for (std::uint32_t y = 0; y < picture.alignedHeight / 2u; ++y) {
            const auto row = std::min(y, chromaRows - 1u);
            std::memcpy(chroma + static_cast<std::size_t>(y) * picture.pitch, source.data[1] + static_cast<std::ptrdiff_t>(row) * source.linesize[1], rowBytes);
        }
    }

    void release() noexcept {
        if (scaler != nullptr) sws_freeContext(scaler);
        scaler = nullptr;
        if (converted != nullptr) av_frame_free(&converted);
        if (frame != nullptr) av_frame_free(&frame);
        if (packet != nullptr) av_packet_free(&packet);
        if (context != nullptr) avcodec_free_context(&context);
    }
};

}

std::unique_ptr<VideoDecoder> CreateVideoDecoder(Codec codec) {
    return std::make_unique<FfmpegVideoDecoder>(codec);
}

}

#else

namespace Videodec2 {

std::unique_ptr<VideoDecoder> CreateVideoDecoder(Codec) {
    return nullptr;
}

}

#endif
