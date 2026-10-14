#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "src/VideoDecoder.hpp"

// libSceVideodec2 decodes H.264/HEVC/VP9 access units on the CPU. Structures, error codes and the output
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
constexpr std::uint32_t kCodecVp9 = 2382845;
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

struct Vp9PictureInfo {
    std::uint64_t thisSize;
    bool isValid;
    std::uint64_t ptsData;
    std::uint64_t dtsData;
    std::uint64_t attachedData;
    std::uint32_t profile;
    std::uint32_t level;
    std::uint32_t keyFrameFlag;
    std::uint32_t intraOnly;
    std::uint8_t bitDepth;
    std::uint8_t colorRange;
    std::uint32_t colorSpace;
    std::uint32_t numFrames;
    std::uint32_t frameWidth;
    std::uint32_t frameHeight;
    std::uint32_t renderWidth;
    std::uint32_t renderHeight;
};

static_assert(sizeof(Vp9PictureInfo) == 88);
static_assert(offsetof(Vp9PictureInfo, colorSpace) == 60);

class Vp9Bits {
public:
    explicit Vp9Bits(std::span<const std::uint8_t> bytes) : bytes(bytes) {}
    std::uint32_t Read(unsigned count) {
        std::uint32_t result = 0;
        while (count-- != 0) {
            if (position / 8 >= bytes.size()) throw std::runtime_error("Videodec2: truncated VP9 header");
            result = (result << 1u) | ((bytes[position / 8] >> (7u - position % 8)) & 1u);
            ++position;
        }
        return result;
    }
private:
    std::span<const std::uint8_t> bytes;
    std::size_t position = 0;
};

struct Vp9Unit {
    std::span<const std::uint8_t> bytes;
    Vp9PictureInfo info{};
    bool shown = false;
};

Vp9Unit ParseVp9Unit(std::span<const std::uint8_t> bytes, std::array<Vp9PictureInfo, 8>& references, Vp9PictureInfo& colors, std::uint32_t count) {
    Vp9Bits bits(bytes);
    if (bits.Read(2) != 2) throw std::runtime_error("Videodec2: invalid VP9 frame marker");
    const auto low = bits.Read(1);
    const auto profile = low | (bits.Read(1) << 1u);
    if (profile == 3 && bits.Read(1) != 0) throw std::runtime_error("Videodec2: invalid VP9 reserved bit");
    Vp9Unit unit{bytes};
    auto& info = unit.info;
    if (bits.Read(1) != 0) {
        info = references[bits.Read(3)];
        if (!info.isValid) throw std::runtime_error("Videodec2: missing VP9 display reference");
        info.keyFrameFlag = 0;
        info.intraOnly = 0;
        info.profile = profile;
        info.numFrames = count;
        unit.shown = true;
        return unit;
    }
    const bool key = bits.Read(1) == 0;
    unit.shown = bits.Read(1) != 0;
    const bool resilient = bits.Read(1) != 0;
    const bool intra = !key && !unit.shown && bits.Read(1) != 0;
    if (!key && !resilient) bits.Read(2);
    const auto dimensions = [&] {
        info.frameWidth = bits.Read(16) + 1;
        info.frameHeight = bits.Read(16) + 1;
    };
    const auto render = [&] {
        const bool different = bits.Read(1) != 0;
        info.renderWidth = different ? bits.Read(16) + 1 : info.frameWidth;
        info.renderHeight = different ? bits.Read(16) + 1 : info.frameHeight;
    };
    std::uint32_t refresh = 0;
    if (key || intra) {
        if (bits.Read(24) != 0x498342) throw std::runtime_error("Videodec2: invalid VP9 sync code");
        info.bitDepth = 8;
        info.colorSpace = 1;
        if (key || profile != 0) {
            if (profile >= 2) info.bitDepth = bits.Read(1) != 0 ? 12 : 10;
            info.colorSpace = bits.Read(3);
            if (info.colorSpace != 7) {
                info.colorRange = static_cast<std::uint8_t>(bits.Read(1));
                if (profile == 1 || profile == 3) {
                    bits.Read(2);
                    if (bits.Read(1) != 0) throw std::runtime_error("Videodec2: invalid VP9 color reserved bit");
                }
            } else {
                info.colorRange = 1;
                if (profile == 1 || profile == 3) {
                    if (bits.Read(1) != 0) throw std::runtime_error("Videodec2: invalid VP9 RGB reserved bit");
                } else throw std::runtime_error("Videodec2: invalid VP9 RGB profile");
            }
        }
        refresh = key ? 255 : bits.Read(8);
        dimensions();
    } else {
        refresh = bits.Read(8);
        std::array<unsigned, 3> indexes{};
        for (auto& index : indexes) { index = bits.Read(3); bits.Read(1); }
        info = colors;
        if (!info.isValid) throw std::runtime_error("Videodec2: missing VP9 color configuration");
        bool inherited = false;
        for (const auto index : indexes) {
            if (bits.Read(1) != 0) {
                if (!references[index].isValid) throw std::runtime_error("Videodec2: missing VP9 size reference");
                info.frameWidth = references[index].frameWidth;
                info.frameHeight = references[index].frameHeight;
                inherited = true;
                break;
            }
        }
        if (!inherited) dimensions();
    }
    render();
    info.isValid = true;
    info.profile = profile;
    info.keyFrameFlag = key ? 1 : 0;
    info.intraOnly = intra ? 1 : 0;
    info.numFrames = count;
    if (key || intra) colors = info;
    for (unsigned index = 0; index < references.size(); ++index)
        if ((refresh & (1u << index)) != 0) references[index] = info;
    return unit;
}

std::vector<Vp9Unit> ParseVp9Packet(std::span<const std::uint8_t> bytes, std::array<Vp9PictureInfo, 8>& references, Vp9PictureInfo& colors) {
    std::vector<std::span<const std::uint8_t>> frames;
    const auto marker = bytes.back();
    const unsigned count = (marker & 7u) + 1;
    const unsigned magnitude = ((marker >> 3u) & 3u) + 1;
    const std::size_t indexSize = 2 + count * magnitude;
    if ((marker & 0xe0u) == 0xc0u && bytes.size() >= indexSize && bytes[bytes.size() - indexSize] == marker) {
        const auto end = bytes.size() - indexSize;
        std::size_t offset = 0;
        for (unsigned frame = 0; frame < count; ++frame) {
            std::size_t size = 0;
            for (unsigned byte = 0; byte < magnitude; ++byte)
                size |= static_cast<std::size_t>(bytes[end + 1 + frame * magnitude + byte]) << (8u * byte);
            if (size == 0 || size > end - offset) throw std::runtime_error("Videodec2: invalid VP9 superframe size");
            frames.push_back(bytes.subspan(offset, size));
            offset += size;
        }
        if (offset != end) throw std::runtime_error("Videodec2: VP9 superframe contains unindexed bytes");
    } else frames.push_back(bytes);
    std::vector<Vp9Unit> result;
    for (const auto frame : frames) result.push_back(ParseVp9Unit(frame, references, colors, static_cast<std::uint32_t>(frames.size())));
    return result;
}

struct Decoder {
    std::uint32_t codec;
    std::unique_ptr<Videodec2::VideoDecoder> video;
    // VP9: the reference and color state of the parsed frame headers, and the information of each
    // shown frame by its pts until its picture is published.
    std::array<Vp9PictureInfo, 8> vp9References{};
    Vp9PictureInfo vp9Colors{};
    std::map<std::uint64_t, Vp9PictureInfo> vp9Shown;
};

// Parses the VP9 frame headers of an access unit (superframes included) and keeps the shown
// frame's information for its picture.
void parseVp9(Decoder& decoder, const InputData& input) {
    auto references = decoder.vp9References;
    auto colors = decoder.vp9Colors;
    const auto units = ParseVp9Packet({static_cast<const std::uint8_t*>(input.auData), static_cast<std::size_t>(input.auSize)}, references, colors);
    for (const auto& unit : units) {
        if (!unit.shown) continue;
        auto info = unit.info;
        info.thisSize = sizeof(Vp9PictureInfo);
        info.ptsData = input.ptsData;
        info.dtsData = input.dtsData;
        info.attachedData = input.attachedData;
        decoder.vp9Shown[input.ptsData] = info;
    }
    decoder.vp9References = references;
    decoder.vp9Colors = colors;
    constexpr std::size_t kMaxPending = 64;
    while (decoder.vp9Shown.size() > kMaxPending) decoder.vp9Shown.erase(decoder.vp9Shown.begin());
}

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

bool supportedCodec(std::uint32_t codec) {
    return codec == kCodecAvc || codec == kCodecHevc || codec == kCodecVp9;
}

Videodec2::Codec hostCodec(std::uint32_t codec) {
    return codec == kCodecAvc ? Videodec2::Codec::Avc : codec == kCodecHevc ? Videodec2::Codec::Hevc : Videodec2::Codec::Vp9;
}

std::uint64_t pictureInfoBytes(std::uint32_t codec) {
    return codec == kCodecAvc ? sizeof(AvcPictureInfo) : codec == kCodecHevc ? sizeof(HevcPictureInfo) : codec == kCodecVp9 ? sizeof(Vp9PictureInfo) : 0;
}

bool tracing() {
    static const bool enabled = std::getenv("ANYPS5_TRACE_VIDEODEC") != nullptr;
    return enabled;
}

// Describes the picture in output and stores its information after the frame, where
// sceVideodec2GetPictureInfo reads it.
void publish(Decoder& decoder, const Videodec2::Picture& picture, FrameBuffer& frameBuffer, OutputInfo& output) {
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
        // As upstream's Videodec2 reports them (libSceVdecsw reads them): IDR from the key-frame
        // flag, profile_idc without FFmpeg's constraint bits, level_idc.
        avc.idr_pictureflag = picture.keyFrame ? 1 : 0;
        avc.profile_idc = static_cast<std::uint8_t>(picture.profile > 0 ? picture.profile & 0xff : 0);
        avc.level_idc = static_cast<std::uint8_t>(picture.level > 0 ? picture.level : 0);
        avc.frame_cropping_flag = cropRight != 0 || cropBottom != 0;
        avc.frame_crop_right_offset = cropRight;
        avc.frame_crop_bottom_offset = cropBottom;
    } else if (decoder.codec == kCodecHevc) {
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
    } else if (decoder.codec == kCodecVp9) {
        auto& vp9 = *reinterpret_cast<Vp9PictureInfo*>(info);
        const auto found = decoder.vp9Shown.find(picture.pts);
        if (found != decoder.vp9Shown.end()) {
            vp9 = found->second;
            decoder.vp9Shown.erase(found);
        } else {
            vp9 = {};
            vp9.thisSize = sizeof(Vp9PictureInfo);
            vp9.isValid = true;
            vp9.ptsData = picture.pts;
            vp9.dtsData = picture.dts;
            vp9.attachedData = picture.attachedData;
            vp9.keyFrameFlag = picture.keyFrame ? 1 : 0;
            vp9.frameWidth = picture.width;
            vp9.frameHeight = picture.height;
            vp9.renderWidth = picture.width;
            vp9.renderHeight = picture.height;
        }
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
    if (!supportedCodec(config->codecType)) return kErrorCodecType;
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
    if (!supportedCodec(config->codecType)) return kErrorCodecType;
    auto created = std::make_unique<Decoder>();
    created->codec = config->codecType;
    try {
        created->video = Videodec2::CreateVideoDecoder(hostCodec(config->codecType));
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
        if (decoder->codec == kCodecVp9) {
            // The header parse only fills picture information; FFmpeg decides whether the frame decodes.
            try {
                parseVp9(*decoder, *input);
            } catch (const std::runtime_error& error) {
                if (tracing()) std::fprintf(stderr, "[videodec2] VP9 picture information: %s\n", error.what());
            }
        }
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
    decoder->vp9References = {};
    decoder->vp9Colors = {};
    decoder->vp9Shown.clear();
    return 0;
}

// Copies the picture information stored after the frame, keeping the caller's size field.
int32_t APS5_VABI sceVideodec2GetPictureInfo(const OutputInfo* output, void* firstPicture, void* secondPicture) {
    (void)secondPicture;
    if (output == nullptr) return kErrorArgumentPointer;
    if (!outputInfoSize(*output)) return kErrorStructSize;
    if (output->pictureCount == 0 || firstPicture == nullptr) return 0;
    const auto available = pictureInfoBytes(output->codecType);
    if (available == 0) return kErrorApiFail;
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

int32_t APS5_VABI sceVideodec2GetVp9PictureInfo(const OutputInfo* output, void* picture) {
    return sceVideodec2GetPictureInfo(output, picture, nullptr);
}

}
