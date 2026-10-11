#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr int VdecswErrorOutputPending = static_cast<int>(0x81510115u);
constexpr int VdecswErrorInputQueueEmpty = static_cast<int>(0x81510116u);
constexpr int VdecswErrorDecodePending = static_cast<int>(0x81510117u);
constexpr std::size_t MaxQueuedPictures = 4;

struct InputData {
    std::uint64_t thisSize;
    const std::uint8_t* auData;
    std::uint64_t auSize;
    std::uint64_t ptsData;
    std::uint64_t dtsData;
    std::uint64_t attachedData;
};

struct InputResult {
    std::uint64_t thisSize;
    const void* decodedAu;
    std::uint32_t outputFrameCount;
    std::uint32_t reserved;
};

struct FrameBuffer {
    std::uint64_t thisSize;
    void* frameBuffer;
    std::uint64_t frameBufferSize;
};

struct OutputInfo {
    std::uint64_t thisSize;
    bool isValid;
    bool isLastFrame;
    bool isErrorFrame;
    std::uint8_t pictureCount;
    std::uint32_t codecType;
    std::uint32_t frameWidth;
    std::uint32_t framePitch;
    std::uint32_t frameHeight;
    bool isDiscardedFrame;
    void* frameBuffer;
    std::uint64_t frameBufferSize;
    std::uint32_t frameFormat;
    std::uint32_t framePitchInBytes;
};

struct AvcPictureInfo {
    std::uint64_t thisSize;
    bool isValid;
    std::uint64_t ptsData;
    std::uint64_t dtsData;
    std::uint64_t attachedData;
    std::uint8_t idrPictureFlag;
    std::uint8_t profileIdc;
    std::uint8_t levelIdc;
    std::uint32_t picWidthInMbsMinus1;
    std::uint32_t picHeightInMapUnitsMinus1;
    std::uint8_t frameMbsOnlyFlag;
    std::uint8_t frameCroppingFlag;
    std::uint32_t frameCropLeftOffset;
    std::uint32_t frameCropRightOffset;
    std::uint32_t frameCropTopOffset;
    std::uint32_t frameCropBottomOffset;
    std::uint8_t vui[44];
};

static_assert(sizeof(InputData) == 0x30 && sizeof(InputResult) == 0x18 && sizeof(FrameBuffer) == 0x18);
static_assert(sizeof(OutputInfo) == 0x38 && offsetof(OutputInfo, isLastFrame) == 9 && offsetof(OutputInfo, pictureCount) == 0xb && offsetof(OutputInfo, isDiscardedFrame) == 0x1c);
static_assert(sizeof(AvcPictureInfo) == 0x78 && offsetof(AvcPictureInfo, picWidthInMbsMinus1) == 0x2c && offsetof(AvcPictureInfo, frameCropBottomOffset) == 0x44);

struct Videodec2FrameBuffer {
    std::uint64_t thisSize;
    void* frameBuffer;
    std::uint64_t frameBufferSize;
    bool isAccepted;
};

struct Videodec2OutputInfo {
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
};

// Vdecsw's decoder configuration (0x50 bytes) shares its leading fields with Videodec2's (0x48
// bytes, as shadPS4), which our Videodec2 checks by size; it is translated before forwarding.
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
    std::uint64_t computeQueue;
    std::uint64_t cpuAffinityMask;
    std::int32_t cpuThreadPriority;
    bool optimizeProgressiveVideo;
    std::uint8_t reserved[19];
};
static_assert(sizeof(DecoderConfigInfo) == 0x50);

struct Videodec2DecoderConfigInfo {
    std::uint64_t thisSize;
    std::uint32_t resourceType;
    std::uint32_t codecType;
    std::uint32_t profile;
    std::uint32_t maxLevel;
    std::int32_t maxFrameWidth;
    std::int32_t maxFrameHeight;
    std::int32_t maxDpbFrameCount;
    std::uint32_t decodePipelineDepth;
    std::uint64_t computeQueue;
    std::uint64_t cpuAffinityMask;
    std::int32_t cpuThreadPriority;
    bool optimizeProgressiveVideo;
    bool checkMemoryType;
    std::uint8_t reserved0;
    std::uint8_t reserved1;
    void* extraConfigInfo;
};
static_assert(sizeof(Videodec2DecoderConfigInfo) == 0x48);

Videodec2DecoderConfigInfo videodec2Config(const void* config) {
    if (config == nullptr) throw std::runtime_error("Vdecsw: null decoder configuration");
    const auto& source = *static_cast<const DecoderConfigInfo*>(config);
    if (source.thisSize != sizeof(DecoderConfigInfo)) throw std::runtime_error("Vdecsw: unexpected decoder configuration size " + std::to_string(source.thisSize));
    return {sizeof(Videodec2DecoderConfigInfo), source.resourceType, source.codecType, source.profile, source.maxLevel, source.maxFrameWidth, source.maxFrameHeight,
            source.maxDpbFrameCount, source.decodePipelineDepth, source.computeQueue, source.cpuAffinityMask, source.cpuThreadPriority, source.optimizeProgressiveVideo, false, 0, 0, nullptr};
}

}

extern "C" {
int APS5_VABI sceVideodec2QueryComputeMemoryInfo(void* info);
int APS5_VABI sceVideodec2AllocateComputeQueue(const void* config, const void* memory, std::uint64_t* queue);
int APS5_VABI sceVideodec2ReleaseComputeQueue(std::uint64_t queue);
int APS5_VABI sceVideodec2QueryDecoderMemoryInfo(const void* config, void* memory);
int APS5_VABI sceVideodec2CreateDecoder(const void* config, const void* memory, std::uint64_t* handle);
int APS5_VABI sceVideodec2DeleteDecoder(std::uint64_t handle);
int APS5_VABI sceVideodec2Decode(std::uint64_t handle, const InputData* input, Videodec2FrameBuffer* frame, Videodec2OutputInfo* output);
int APS5_VABI sceVideodec2Flush(std::uint64_t handle, Videodec2FrameBuffer* frame, Videodec2OutputInfo* output);
int APS5_VABI sceVideodec2Reset(std::uint64_t handle);
int APS5_VABI sceVideodec2GetAvcPictureInfo(const Videodec2OutputInfo* output, AvcPictureInfo* first, AvcPictureInfo* second);
}

namespace {

struct PendingInput {
    InputData input;
    std::vector<std::uint8_t> data;
};

struct ConsumedInput {
    const void* au;
    std::uint32_t frames;
};

struct Picture {
    std::vector<std::uint8_t> pixels;
    Videodec2OutputInfo output;
    AvcPictureInfo info;
};

struct Decoder {
    std::mutex mutex;
    std::uint64_t frameBytes = 0;
    std::deque<PendingInput> inputs;
    std::deque<ConsumedInput> consumed;
    std::deque<FrameBuffer> outputs;
    std::deque<Picture> pictures;
    std::map<const void*, AvcPictureInfo> delivered;
    bool finalized = false;
    bool drained = false;
};

std::mutex decodersMutex;
std::map<std::uint64_t, std::shared_ptr<Decoder>> decoders;

std::shared_ptr<Decoder> findDecoder(std::uint64_t handle) {
    std::lock_guard lock(decodersMutex);
    const auto found = decoders.find(handle);
    if (found == decoders.end()) throw std::runtime_error("Vdecsw: invalid decoder handle");
    return found->second;
}

// Our Videodec2 reports the frame's 16-aligned height and its pitch, with the cropping that
// recovers the picture (as shadPS4: right = pitch - width, bottom = aligned height - height), in the
// picture information it keeps in the console's layout, which Vdecsw shares. Vdecsw reports the
// picture's own size, with the chroma plane right after the picture's rows; the output and the
// pixels are corrected here before the picture information is derived.
AvcPictureInfo pictureInfo(Videodec2OutputInfo& output, std::vector<std::uint8_t>& pixels) {
    AvcPictureInfo source{sizeof(AvcPictureInfo)};
    sceVideodec2GetAvcPictureInfo(&output, &source, nullptr);
    if (!source.isValid) throw std::runtime_error("Vdecsw: decoded picture has no picture info");
    if (source.frameCroppingFlag) {
        output.frameWidth = output.framePitch - source.frameCropRightOffset;
        const std::uint64_t alignedHeight = output.frameHeight;
        output.frameHeight -= source.frameCropBottomOffset;
        if (output.frameHeight != alignedHeight) {
            const std::uint64_t pitch = output.framePitch;
            const std::uint64_t chromaBytes = pitch * ((output.frameHeight + 1u) / 2u);
            if (pitch * alignedHeight + chromaBytes > pixels.size()) throw std::runtime_error("Vdecsw: decoded picture exceeds its buffer");
            std::memmove(pixels.data() + pitch * output.frameHeight, pixels.data() + pitch * alignedHeight, chromaBytes);
            output.frameBufferSize = pitch * output.frameHeight + chromaBytes;
        }
    }
    const auto widthInMbs = (output.frameWidth + 15u) / 16u;
    const auto heightInMbs = (output.frameHeight + 15u) / 16u;
    AvcPictureInfo info{};
    info.thisSize = sizeof(AvcPictureInfo);
    info.isValid = true;
    info.ptsData = source.ptsData;
    info.dtsData = source.dtsData;
    info.attachedData = source.attachedData;
    info.idrPictureFlag = source.idrPictureFlag;
    info.profileIdc = source.profileIdc;
    info.levelIdc = source.levelIdc;
    info.picWidthInMbsMinus1 = widthInMbs - 1u;
    info.picHeightInMapUnitsMinus1 = heightInMbs - 1u;
    info.frameMbsOnlyFlag = 1;
    info.frameCroppingFlag = widthInMbs * 16u != output.frameWidth || heightInMbs * 16u != output.frameHeight;
    info.frameCropRightOffset = (widthInMbs * 16u - output.frameWidth) / 2u;
    info.frameCropBottomOffset = (heightInMbs * 16u - output.frameHeight) / 2u;
    return info;
}

void advance(std::uint64_t handle, Decoder& decoder) {
    while (decoder.pictures.size() < MaxQueuedPictures) {
        std::vector<std::uint8_t> pixels(decoder.frameBytes);
        Videodec2FrameBuffer frame{sizeof(Videodec2FrameBuffer), pixels.data(), pixels.size(), false};
        Videodec2OutputInfo output{sizeof(Videodec2OutputInfo)};
        if (!decoder.inputs.empty()) {
            const auto* au = static_cast<const void*>(decoder.inputs.front().input.auData);
            auto input = decoder.inputs.front().input;
            input.auData = decoder.inputs.front().data.data();
            sceVideodec2Decode(handle, &input, &frame, &output);
            decoder.inputs.pop_front();
            decoder.consumed.push_back({au, output.isValid ? 1u : 0u});
        } else if (decoder.finalized && !decoder.drained) {
            sceVideodec2Flush(handle, &frame, &output);
            decoder.drained = !output.isValid;
        } else {
            return;
        }
        if (output.isValid) {
            const auto info = pictureInfo(output, pixels);
            decoder.pictures.push_back({std::move(pixels), output, info});
        }
    }
}

}

extern "C" {

int APS5_VABI sceVdecswQueryComputeMemoryInfo(void* info) {
    return sceVideodec2QueryComputeMemoryInfo(info);
}

int APS5_VABI sceVdecswAllocateComputeQueue(const void* config, const void* memory, std::uint64_t* queue) {
    return sceVideodec2AllocateComputeQueue(config, memory, queue);
}

int APS5_VABI sceVdecswReleaseComputeQueue(std::uint64_t queue) {
    return sceVideodec2ReleaseComputeQueue(queue);
}

int APS5_VABI sceVdecswQueryDecoderMemoryInfo(const void* config, void* memory) {
    const auto translated = videodec2Config(config);
    return sceVideodec2QueryDecoderMemoryInfo(&translated, memory);
}

int APS5_VABI sceVdecswCreateDecoder(const void* config, const DecoderMemoryInfo* memory, std::uint64_t* handle) {
    if (handle == nullptr || memory == nullptr || memory->thisSize < sizeof(DecoderMemoryInfo) || memory->maxFrameBufferSize == 0) throw std::runtime_error("Vdecsw: invalid decoder creation arguments");
    const auto translated = videodec2Config(config);
    const auto result = sceVideodec2CreateDecoder(&translated, memory, handle);
    if (result != 0) return result;
    auto decoder = std::make_shared<Decoder>();
    decoder->frameBytes = memory->maxFrameBufferSize;
    std::lock_guard lock(decodersMutex);
    decoders.emplace(*handle, std::move(decoder));
    return 0;
}

int APS5_VABI sceVdecswDeleteDecoder(std::uint64_t handle) {
    {
        std::lock_guard lock(decodersMutex);
        if (decoders.erase(handle) == 0) throw std::runtime_error("Vdecsw: invalid decoder handle");
    }
    return sceVideodec2DeleteDecoder(handle);
}

int APS5_VABI sceVdecswSetDecodeInput(std::uint64_t handle, const InputData* input) {
    if (input == nullptr || input->thisSize < sizeof(InputData)) throw std::runtime_error("Vdecsw: invalid decode input");
    const auto decoder = findDecoder(handle);
    std::lock_guard lock(decoder->mutex);
    if (decoder->finalized) throw std::runtime_error("Vdecsw: decode input after the sequence was finalized");
    if (input->auData == nullptr || input->auSize == 0) throw std::runtime_error("Vdecsw: empty access unit");
    decoder->inputs.push_back({*input, std::vector<std::uint8_t>(input->auData, input->auData + input->auSize)});
    advance(handle, *decoder);
    return 0;
}

int APS5_VABI sceVdecswTrySyncDecodeInput(std::uint64_t handle, InputResult* result) {
    if (result == nullptr || result->thisSize < sizeof(InputResult)) throw std::runtime_error("Vdecsw: invalid decode input result");
    const auto decoder = findDecoder(handle);
    std::lock_guard lock(decoder->mutex);
    advance(handle, *decoder);
    result->decodedAu = nullptr;
    result->outputFrameCount = 0;
    result->reserved = 0;
    if (!decoder->consumed.empty()) {
        result->decodedAu = decoder->consumed.front().au;
        result->outputFrameCount = decoder->consumed.front().frames;
        decoder->consumed.pop_front();
        return 0;
    }
    return decoder->inputs.empty() ? VdecswErrorInputQueueEmpty : VdecswErrorDecodePending;
}

int APS5_VABI sceVdecswSetDecodeOutput(std::uint64_t handle, const FrameBuffer* frame) {
    if (frame == nullptr || frame->thisSize < sizeof(FrameBuffer) || frame->frameBuffer == nullptr) throw std::runtime_error("Vdecsw: invalid decode output");
    const auto decoder = findDecoder(handle);
    std::lock_guard lock(decoder->mutex);
    decoder->outputs.push_back(*frame);
    return 0;
}

int APS5_VABI sceVdecswTrySyncDecodeOutput(std::uint64_t handle, OutputInfo* output) {
    if (output == nullptr || output->thisSize < sizeof(OutputInfo)) throw std::runtime_error("Vdecsw: invalid decode output info");
    const auto decoder = findDecoder(handle);
    std::lock_guard lock(decoder->mutex);
    advance(handle, *decoder);
    if (decoder->outputs.empty()) throw std::runtime_error("Vdecsw: output sync without an output buffer");
    const bool ended = decoder->finalized && decoder->drained;
    if (decoder->pictures.size() < (ended ? 1u : 2u)) {
        if (!ended) return VdecswErrorOutputPending;
        std::memset(reinterpret_cast<std::uint8_t*>(output) + sizeof(output->thisSize), 0, sizeof(OutputInfo) - sizeof(output->thisSize));
        output->isLastFrame = true;
        decoder->outputs.pop_front();
        return 0;
    }
    const auto target = decoder->outputs.front();
    auto picture = std::move(decoder->pictures.front());
    decoder->pictures.pop_front();
    if (target.frameBufferSize < picture.output.frameBufferSize) throw std::runtime_error("Vdecsw: output buffer is smaller than the decoded picture");
    std::memcpy(target.frameBuffer, picture.pixels.data(), picture.output.frameBufferSize);
    decoder->outputs.pop_front();
    output->isValid = true;
    output->isLastFrame = ended && decoder->pictures.empty();
    output->isErrorFrame = picture.output.isErrorFrame;
    output->pictureCount = picture.output.pictureCount;
    output->codecType = picture.output.codecType;
    output->frameWidth = picture.output.frameWidth;
    output->framePitch = picture.output.framePitch;
    output->frameHeight = picture.output.frameHeight;
    output->isDiscardedFrame = false;
    output->frameBuffer = target.frameBuffer;
    output->frameBufferSize = picture.output.frameBufferSize;
    output->frameFormat = picture.output.frameFormat;
    output->framePitchInBytes = picture.output.framePitchInBytes;
    decoder->delivered[target.frameBuffer] = picture.info;
    return 0;
}

int APS5_VABI sceVdecswGetAvcPictureInfo(const OutputInfo* output, AvcPictureInfo* first, AvcPictureInfo* second) {
    if (output == nullptr || first == nullptr || first->thisSize < sizeof(std::uint64_t)) throw std::runtime_error("Vdecsw: invalid argument pointer");
    if (output->pictureCount > 1) throw std::runtime_error("Vdecsw: second field picture info is not implemented");
    if (output->pictureCount == 0) return 0;
    static_cast<void>(second);
    std::vector<std::shared_ptr<Decoder>> all;
    {
        std::lock_guard lock(decodersMutex);
        for (const auto& [handle, decoder] : decoders) all.push_back(decoder);
    }
    for (const auto& decoder : all) {
        std::lock_guard lock(decoder->mutex);
        const auto found = decoder->delivered.find(output->frameBuffer);
        if (found == decoder->delivered.end()) continue;
        const auto bytes = std::min<std::uint64_t>(first->thisSize, sizeof(AvcPictureInfo)) - sizeof(std::uint64_t);
        std::memcpy(reinterpret_cast<std::uint8_t*>(first) + sizeof(std::uint64_t), reinterpret_cast<const std::uint8_t*>(&found->second) + sizeof(std::uint64_t), bytes);
        return 0;
    }
    throw std::runtime_error("Vdecsw: picture info requested for a frame buffer no decoder returned");
}

int APS5_VABI sceVdecswFinalizeDecodeSequence(std::uint64_t handle) {
    const auto decoder = findDecoder(handle);
    std::lock_guard lock(decoder->mutex);
    decoder->finalized = true;
    advance(handle, *decoder);
    return 0;
}

int APS5_VABI sceVdecswResetDecoder(std::uint64_t handle) {
    const auto decoder = findDecoder(handle);
    std::lock_guard lock(decoder->mutex);
    decoder->inputs.clear();
    decoder->consumed.clear();
    decoder->outputs.clear();
    decoder->pictures.clear();
    decoder->delivered.clear();
    decoder->finalized = false;
    decoder->drained = false;
    return sceVideodec2Reset(handle);
}

}
