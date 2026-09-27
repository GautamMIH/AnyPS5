#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "src/VideoDecoder.hpp"

// libSceVideodec2 decodes H.264/HEVC access units on the CPU. Structures, error codes and the output
// layout (NV12 frame followed by the picture information) follow shadPS4's videodec2; see
// docs/research-notes.md.
namespace {

constexpr int kErrorApiFail = static_cast<int>(0x811D0100);
constexpr int kErrorStructSize = static_cast<int>(0x811D0101);
constexpr int kErrorArgumentPointer = static_cast<int>(0x811D0102);
constexpr int kErrorDecoderInstance = static_cast<int>(0x811D0103);
constexpr int kErrorMemoryPointer = static_cast<int>(0x811D0105);
constexpr int kErrorFrameBufferSize = static_cast<int>(0x811D0106);
constexpr int kErrorFrameBufferPointer = static_cast<int>(0x811D0107);
constexpr int kErrorAccessUnitSize = static_cast<int>(0x811D010D);
constexpr int kErrorAccessUnitPointer = static_cast<int>(0x811D010E);
constexpr int kErrorConfigInfo = static_cast<int>(0x811D0200);
constexpr int kErrorComputePipeId = static_cast<int>(0x811D0201);
constexpr int kErrorComputeQueueId = static_cast<int>(0x811D0202);
constexpr int kErrorCodecType = static_cast<int>(0x811D0204);

constexpr std::uint32_t kCodecAvc = 1;
constexpr std::uint32_t kCodecHevc = 974921;
// Memory the library asks for; decoding runs on the host, so the amounts only need to be plausible.
constexpr std::uint64_t kWorkMemoryBytes = 16ull << 20;

struct ComputeMemoryInfo {
    std::uint64_t thisSize;
    std::uint64_t cpuGpuMemorySize;
    void* cpuGpuMemory;
};
static_assert(sizeof(ComputeMemoryInfo) == 0x18);

struct ComputeConfigInfo {
    std::uint64_t thisSize;
    std::uint16_t computePipeId;
    std::uint16_t computeQueueId;
    bool checkMemoryType;
    std::uint8_t reserved0;
    std::uint16_t reserved1;
};
static_assert(sizeof(ComputeConfigInfo) == 0x10);

struct DecoderConfigInfo {
    std::uint64_t thisSize;
    std::uint32_t resourceType;
    std::uint32_t codecType;
    std::uint32_t profile;
    std::uint32_t maxLevel;
    std::int32_t maxFrameWidth;
    std::int32_t maxFrameHeight;
    std::int32_t maxDpbFrameCount;
    std::uint32_t decodePipelineDepth;
    void* computeQueue;
    std::uint64_t cpuAffinityMask;
    std::int32_t cpuThreadPriority;
    bool optimizeProgressiveVideo;
    bool checkMemoryType;
    std::uint8_t reserved0;
    std::uint8_t reserved1;
    void* extraConfigInfo;
};
static_assert(sizeof(DecoderConfigInfo) == 0x48);

struct DecoderMemoryInfo {
    std::uint64_t thisSize;
    std::uint64_t cpuMemorySize;
    void* cpuMemory;
    std::uint64_t gpuMemorySize;
    void* gpuMemory;
    std::uint64_t cpuGpuMemorySize;
    void* cpuGpuMemory;
    std::uint64_t maxFrameBufferSize;
    std::uint32_t frameBufferAlignment;
    std::uint32_t reserved0;
};
static_assert(sizeof(DecoderMemoryInfo) == 0x48);

struct InputData {
    std::uint64_t thisSize;
    void* auData;
    std::uint64_t auSize;
    std::uint64_t ptsData;
    std::uint64_t dtsData;
    std::uint64_t attachedData;
};
static_assert(sizeof(InputData) == 0x30);

// Older SDKs pass the structure without the last two fields (0x30 bytes).
struct OutputInfo {
    std::uint64_t thisSize;
    bool isValid;
    bool isErrorFrame;
    std::uint8_t pictureCount;
    std::uint32_t codecType;
    std::uint32_t frameWidth;
    std::uint32_t framePitch;
    std::uint32_t frameHeight;
    void* frameBuffer;
    std::uint64_t frameBufferSize;
    std::uint32_t frameFormat;
    std::uint32_t framePitchInBytes;
};
static_assert(sizeof(OutputInfo) == 0x38);

struct FrameBuffer {
    std::uint64_t thisSize;
    void* frameBuffer;
    std::uint64_t frameBufferSize;
    bool isAccepted;
};
static_assert(sizeof(FrameBuffer) == 0x20);

// The per-picture information stored after the frame (shadPS4 videodec2.h); only the fields the
// host decoder knows are filled.
struct AvcPictureInfo {
    std::uint64_t this_size;

    bool is_valid;

    std::uint64_t pts_data;
    std::uint64_t dts_data;
    std::uint64_t attached_data;

    std::uint8_t idr_pictureflag;

    std::uint8_t profile_idc;
    std::uint8_t level_idc;
    std::uint32_t pic_width_in_mbs_minus1;
    std::uint32_t pic_height_in_map_units_minus1;
    std::uint8_t frame_mbs_only_flag;

    std::uint8_t frame_cropping_flag;
    std::uint32_t frame_crop_left_offset;
    std::uint32_t frame_crop_right_offset;
    std::uint32_t frame_crop_top_offset;
    std::uint32_t frame_crop_bottom_offset;

    std::uint8_t aspect_ratio_info_present_flag;
    std::uint8_t aspect_ratio_idc;
    std::uint16_t sar_width;
    std::uint16_t sar_height;

    std::uint8_t video_signal_type_present_flag;
    std::uint8_t video_format;
    std::uint8_t video_full_range_flag;
    std::uint8_t colour_description_present_flag;
    std::uint8_t colour_primaries;
    std::uint8_t transfer_characteristics;
    std::uint8_t matrix_coefficients;

    std::uint8_t timing_info_present_flag;
    std::uint32_t num_units_in_tick;
    std::uint32_t time_scale;
    std::uint8_t fixed_frame_rate_flag;

    std::uint8_t bitstream_restriction_flag;
    std::uint8_t max_dec_frame_buffering;

    std::uint8_t pic_struct_present_flag;
    std::uint8_t pic_struct;
    std::uint8_t field_pic_flag;
    std::uint8_t bottom_field_flag;

    std::uint8_t sequence_parameter_set_present_flag;
    std::uint8_t picture_parameter_set_present_flag;
    std::uint8_t au_delimiter_present_flag;
    std::uint8_t end_of_sequence_present_flag;
    std::uint8_t end_of_stream_present_flag;
    std::uint8_t filler_data_present_flag;
    std::uint8_t picture_timing_sei_present_flag;
    std::uint8_t buffering_period_sei_present_flag;

    std::uint8_t constraint_set0_flag;
    std::uint8_t constraint_set1_flag;
    std::uint8_t constraint_set2_flag;
    std::uint8_t constraint_set3_flag;
    std::uint8_t constraint_set4_flag;
    std::uint8_t constraint_set5_flag;
};
static_assert(sizeof(AvcPictureInfo) == 0x78);

struct HevcPictureInfo {
    std::uint64_t this_size;
    bool is_valid;
    std::uint64_t pts_data;
    std::uint64_t dts_data;
    std::uint64_t attached_data;
    std::uint32_t pic_width_in_luma_samples;
    std::uint32_t pic_height_in_luma_samples;
    std::uint8_t bit_depth_luma_minus8;
    std::uint8_t bit_depth_chroma_minus8;
    std::uint8_t timing_info_present_flag;
    std::uint32_t num_units_in_tick;
    std::uint32_t time_scale;
    std::uint32_t aspect_ratio_info_present_flag;
    std::uint8_t aspect_ratio_idc;
    std::uint16_t sar_width;
    std::uint16_t sar_height;
    std::uint8_t video_signal_type_present_flag;
    std::uint8_t video_format;
    std::uint8_t video_full_range_flag;
    std::uint8_t colour_description_present_flag;
    std::uint8_t colour_primaries;
    std::uint8_t transfer_characteristics;
    std::uint8_t matrix_coeffs;
    std::uint8_t frame_field_info_present_flag;
    std::uint32_t pic_struct;
    std::uint32_t source_scan_type;
    std::uint32_t duplicate_flag;
    std::uint32_t conformance_window_flag;
    std::uint32_t conf_win_left_offset;
    std::uint32_t conf_win_right_offset;
    std::uint32_t conf_win_top_offset;
    std::uint32_t conf_win_bottom_offset;
    std::uint32_t default_display_window_flag;
    std::uint32_t def_disp_win_left_offset;
    std::uint32_t def_disp_win_right_offset;
    std::uint32_t def_disp_win_top_offset;
    std::uint32_t def_disp_win_bottom_offset;
    std::uint8_t chroma_loc_info_present_flag;
    std::uint8_t chroma_sample_loc_type_top_field;
    std::uint8_t chroma_sample_loc_type_bottom_field;
    std::uint8_t field_seq_flag;
    std::uint8_t video_parameter_set_present_flag;
    std::uint8_t sequence_parameter_set_present_flag;
    std::uint8_t picture_parameter_set_present_flag;
    std::uint8_t au_delimiter_present_flag;
    std::uint8_t end_of_sequence_present_flag;
    std::uint8_t end_of_stream_present_flag;
    std::uint8_t filler_data_present_flag;
    std::uint8_t picture_timing_sei_present_flag;
    std::uint8_t buffering_period_sei_present_flag;
    std::uint8_t frame_packing_arrangement_sei_present_flag;
    std::uint8_t alternative_transfer_characteristics_sei_present_flag;
    std::uint8_t idr_pictureflag;
    std::uint8_t irap_picture_flag;
    std::uint8_t general_profile_space;
    std::uint8_t general_tier_flag;
    std::uint8_t general_profile_idc;
    std::uint8_t general_progressive_source_flag;
    std::uint8_t general_interlaced_source_flag;
    std::uint8_t general_frame_only_constraint_flag;
    std::uint8_t general_level_idc;
    std::uint8_t sub_layer_profile_present_flag;
    std::uint8_t sub_layer_level_present_flag;
    std::uint8_t sub_layer_profile_space;
    std::uint8_t sub_layer_tier_flag;
    std::uint8_t sub_layer_profile_idc;
    std::uint8_t sub_layer_level_idc;
    std::uint8_t sub_layer_ordering_info_present_flag;
    std::uint8_t max_dec_pic_buffering_minus1;
    std::uint8_t preferred_transfer_characteristics;
    std::uint8_t frame_cropping_flag;
    std::uint32_t frame_crop_left_offset;
    std::uint32_t frame_crop_right_offset;
    std::uint32_t frame_crop_top_offset;
    std::uint32_t frame_crop_bottom_offset;
};
static_assert(sizeof(HevcPictureInfo) == 0xB8);

struct Decoder {
    std::uint32_t codec;
    std::unique_ptr<Videodec2::VideoDecoder> video;
};

std::mutex& decodersMutex() {
    static std::mutex value;
    return value;
}

std::set<Decoder*>& decoders() {
    static std::set<Decoder*> value;
    return value;
}

bool known(Decoder* decoder) {
    std::lock_guard lock(decodersMutex());
    return decoders().contains(decoder);
}

bool outputInfoSize(const OutputInfo& info) {
    return info.thisSize == sizeof(OutputInfo) || info.thisSize == offsetof(OutputInfo, frameFormat);
}

std::uint64_t pictureInfoBytes(std::uint32_t codec) {
    return codec == kCodecAvc ? sizeof(AvcPictureInfo) : sizeof(HevcPictureInfo);
}

bool tracing() {
    static const bool enabled = std::getenv("ANYPS5_TRACE_VIDEODEC") != nullptr;
    return enabled;
}

// Describes the picture in output and stores its information after the frame, where
// sceVideodec2GetPictureInfo reads it.
void publish(const Decoder& decoder, const Videodec2::Picture& picture, FrameBuffer& frameBuffer, OutputInfo& output) {
    frameBuffer.isAccepted = true;
    output.isValid = true;
    output.isErrorFrame = false;
    output.pictureCount = 1;
    output.codecType = decoder.codec;
    output.frameWidth = (picture.width + 15u) & ~15u;
    output.framePitch = picture.pitch;
    output.frameHeight = picture.alignedHeight;
    output.frameBuffer = frameBuffer.frameBuffer;
    output.frameBufferSize = Videodec2::Nv12Bytes(picture.width, picture.height);
    if (output.thisSize == sizeof(OutputInfo)) {
        output.frameFormat = 0;
        output.framePitchInBytes = picture.pitch;
    }
    auto* info = static_cast<std::byte*>(frameBuffer.frameBuffer) + output.frameBufferSize;
    const auto cropRight = picture.pitch - picture.width;
    const auto cropBottom = picture.alignedHeight - picture.height;
    if (decoder.codec == kCodecAvc) {
        auto& avc = *reinterpret_cast<AvcPictureInfo*>(info);
        avc = {};
        avc.this_size = sizeof(AvcPictureInfo);
        avc.is_valid = true;
        avc.pts_data = picture.pts;
        avc.dts_data = picture.dts;
        avc.attached_data = picture.attachedData;
        avc.frame_cropping_flag = cropRight != 0 || cropBottom != 0;
        avc.frame_crop_right_offset = cropRight;
        avc.frame_crop_bottom_offset = cropBottom;
    } else {
        auto& hevc = *reinterpret_cast<HevcPictureInfo*>(info);
        hevc = {};
        hevc.this_size = sizeof(HevcPictureInfo);
        hevc.is_valid = true;
        hevc.pts_data = picture.pts;
        hevc.dts_data = picture.dts;
        hevc.attached_data = picture.attachedData;
        hevc.frame_cropping_flag = cropRight != 0 || cropBottom != 0;
        hevc.frame_crop_right_offset = cropRight;
        hevc.frame_crop_bottom_offset = cropBottom;
    }
    if (tracing()) std::fprintf(stderr, "[videodec2] picture %ux%u pitch %u pts %llu\n", picture.width, picture.height, picture.pitch, static_cast<unsigned long long>(picture.pts));
}

void clearOutput(FrameBuffer& frameBuffer, OutputInfo& output) {
    frameBuffer.isAccepted = false;
    output.isValid = false;
    output.pictureCount = 0;
}

std::span<std::byte> frameSpace(const Decoder& decoder, const FrameBuffer& frameBuffer) {
    const auto infoBytes = pictureInfoBytes(decoder.codec);
    const auto usable = frameBuffer.frameBufferSize > infoBytes ? frameBuffer.frameBufferSize - infoBytes : 0;
    return {static_cast<std::byte*>(frameBuffer.frameBuffer), static_cast<std::size_t>(usable)};
}

template<typename TRun>
int run(Decoder& decoder, FrameBuffer& frameBuffer, OutputInfo& output, TRun&& produce) {
    clearOutput(frameBuffer, output);
    try {
        if (const auto picture = produce()) publish(decoder, *picture, frameBuffer, output);
        return 0;
    } catch (const std::length_error&) {
        return kErrorFrameBufferSize;
    } catch (const std::runtime_error& error) {
        if (tracing()) std::fprintf(stderr, "[videodec2] %s\n", error.what());
        return kErrorApiFail;
    }
}

}

extern "C" {

int32_t APS5_VABI sceVideodec2QueryComputeMemoryInfo(ComputeMemoryInfo* info) {
    if (info == nullptr) return kErrorArgumentPointer;
    if (info->thisSize != sizeof(ComputeMemoryInfo)) return kErrorStructSize;
    info->cpuGpuMemory = nullptr;
    info->cpuGpuMemorySize = kWorkMemoryBytes;
    return 0;
}

int32_t APS5_VABI sceVideodec2AllocateComputeQueue(const ComputeConfigInfo* config, const ComputeMemoryInfo* memory, void** queue) {
    if (config == nullptr || memory == nullptr || queue == nullptr) return kErrorArgumentPointer;
    if (config->thisSize != sizeof(ComputeConfigInfo) || memory->thisSize != sizeof(ComputeMemoryInfo)) return kErrorStructSize;
    if (config->reserved0 != 0 || config->reserved1 != 0) return kErrorConfigInfo;
    if (config->computePipeId > 4) return kErrorComputePipeId;
    if (config->computeQueueId > 7) return kErrorComputeQueueId;
    if (memory->cpuGpuMemory == nullptr) return kErrorMemoryPointer;
    // The queue handle points into the memory the title provided, as on the console (shadPS4).
    *queue = memory->cpuGpuMemory;
    return 0;
}

int32_t APS5_VABI sceVideodec2ReleaseComputeQueue(void* queue) {
    (void)queue;
    return 0;
}

int32_t APS5_VABI sceVideodec2QueryDecoderMemoryInfo(const DecoderConfigInfo* config, DecoderMemoryInfo* memory) {
    if (config == nullptr || memory == nullptr) return kErrorArgumentPointer;
    if (config->thisSize != sizeof(DecoderConfigInfo) || memory->thisSize != sizeof(DecoderMemoryInfo)) return kErrorStructSize;
    if (config->codecType != kCodecAvc && config->codecType != kCodecHevc) return kErrorCodecType;
    // The largest picture the configuration allows, as NV12 plus its picture information (shadPS4).
    auto width = config->maxFrameWidth;
    auto height = config->maxFrameHeight;
    if (width <= 0 || height <= 0) {
        width = config->maxLevel >= 150 ? 3840 : 1920;
        height = config->maxLevel >= 150 ? 2160 : 1080;
    }
    const auto frameBytes = Videodec2::Nv12Bytes(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    memory->cpuMemory = nullptr;
    memory->gpuMemory = nullptr;
    memory->cpuGpuMemory = nullptr;
    memory->cpuMemorySize = kWorkMemoryBytes;
    memory->gpuMemorySize = kWorkMemoryBytes;
    memory->cpuGpuMemorySize = kWorkMemoryBytes;
    memory->maxFrameBufferSize = ((frameBytes + 0xffu) & ~0xffull) + 0x4000u;
    memory->frameBufferAlignment = 0x100;
    return 0;
}

int32_t APS5_VABI sceVideodec2CreateDecoder(const DecoderConfigInfo* config, const DecoderMemoryInfo* memory, Decoder** decoder) {
    if (config == nullptr || memory == nullptr || decoder == nullptr) return kErrorArgumentPointer;
    if (config->thisSize != sizeof(DecoderConfigInfo) || memory->thisSize != sizeof(DecoderMemoryInfo)) return kErrorStructSize;
    if (config->codecType != kCodecAvc && config->codecType != kCodecHevc) return kErrorCodecType;
    auto created = std::make_unique<Decoder>();
    created->codec = config->codecType;
    try {
        created->video = Videodec2::CreateVideoDecoder(config->codecType == kCodecAvc ? Videodec2::Codec::Avc : Videodec2::Codec::Hevc);
    } catch (const std::runtime_error& error) {
        if (tracing()) std::fprintf(stderr, "[videodec2] %s\n", error.what());
        return kErrorApiFail;
    }
    if (!created->video) {
        NotImplemented_nid_no_patch("sceVideodec2CreateDecoder (this build has no FFmpeg)");
        return kErrorApiFail;
    }
    std::lock_guard lock(decodersMutex());
    *decoder = created.get();
    decoders().insert(created.release());
    return 0;
}

int32_t APS5_VABI sceVideodec2DeleteDecoder(Decoder* decoder) {
    {
        std::lock_guard lock(decodersMutex());
        if (decoders().erase(decoder) == 0) return kErrorDecoderInstance;
    }
    delete decoder;
    return 0;
}

int32_t APS5_VABI sceVideodec2Decode(Decoder* decoder, const InputData* input, FrameBuffer* frameBuffer, OutputInfo* output) {
    if (!known(decoder)) return kErrorDecoderInstance;
    if (input == nullptr || frameBuffer == nullptr || output == nullptr) return kErrorArgumentPointer;
    if (input->thisSize != sizeof(InputData) || frameBuffer->thisSize != sizeof(FrameBuffer) || !outputInfoSize(*output)) return kErrorStructSize;
    if (input->auData == nullptr) return kErrorAccessUnitPointer;
    if (input->auSize == 0) return kErrorAccessUnitSize;
    if (frameBuffer->frameBuffer == nullptr) return kErrorFrameBufferPointer;
    return run(*decoder, *frameBuffer, *output, [&] {
        return decoder->video->Decode({static_cast<const std::byte*>(input->auData), static_cast<std::size_t>(input->auSize)}, input->ptsData, input->dtsData, input->attachedData, frameSpace(*decoder, *frameBuffer));
    });
}

int32_t APS5_VABI sceVideodec2Flush(Decoder* decoder, FrameBuffer* frameBuffer, OutputInfo* output) {
    if (!known(decoder)) return kErrorDecoderInstance;
    if (frameBuffer == nullptr || output == nullptr) return kErrorArgumentPointer;
    if (frameBuffer->thisSize != sizeof(FrameBuffer) || !outputInfoSize(*output)) return kErrorStructSize;
    if (frameBuffer->frameBuffer == nullptr) return kErrorFrameBufferPointer;
    return run(*decoder, *frameBuffer, *output, [&] { return decoder->video->Flush(frameSpace(*decoder, *frameBuffer)); });
}

int32_t APS5_VABI sceVideodec2Reset(Decoder* decoder) {
    if (!known(decoder)) return kErrorDecoderInstance;
    decoder->video->Reset();
    return 0;
}

// Copies the picture information stored after the frame, keeping the caller's size field.
int32_t APS5_VABI sceVideodec2GetPictureInfo(const OutputInfo* output, void* firstPicture, void* secondPicture) {
    (void)secondPicture;
    if (output == nullptr) return kErrorArgumentPointer;
    if (!outputInfoSize(*output)) return kErrorStructSize;
    if (output->pictureCount == 0 || firstPicture == nullptr) return 0;
    const auto available = pictureInfoBytes(output->codecType);
    const auto requested = *static_cast<const std::uint64_t*>(firstPicture);
    const auto bytes = std::min<std::uint64_t>(requested, available);
    if (bytes <= sizeof(std::uint64_t)) return kErrorStructSize;
    const auto* source = static_cast<const std::byte*>(output->frameBuffer) + output->frameBufferSize;
    std::memcpy(static_cast<std::byte*>(firstPicture) + sizeof(std::uint64_t), source + sizeof(std::uint64_t), static_cast<std::size_t>(bytes - sizeof(std::uint64_t)));
    return 0;
}

int32_t APS5_VABI sceVideodec2GetAvcPictureInfo(const OutputInfo* output, void* firstPicture, void* secondPicture) {
    return sceVideodec2GetPictureInfo(output, firstPicture, secondPicture);
}

int32_t APS5_VABI sceVideodec2GetHevcPictureInfo(const OutputInfo* output, void* picture) {
    return sceVideodec2GetPictureInfo(output, picture, nullptr);
}

}
