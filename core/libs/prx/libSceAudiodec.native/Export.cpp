#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "src/AacDecoder.hpp"

// libSceAudiodec decodes whole access units synchronously. Codec ids and error codes follow fpPS4
// (ps4_libsceaudiodec.pas); the M4AAC parameter and stream-information layouts were read from titles
// (see docs/research-notes.md).
namespace {

constexpr std::uint32_t kCodecAt9 = 1;
constexpr std::uint32_t kCodecMp3 = 2;
constexpr std::uint32_t kCodecM4aac = 3;

constexpr int kErrorInvalidType = static_cast<int>(0x807F0001);
constexpr int kErrorInvalidParamSize = static_cast<int>(0x807F0004);
constexpr int kErrorInvalidBsiInfoSize = static_cast<int>(0x807F0005);
constexpr int kErrorInvalidAuInfoSize = static_cast<int>(0x807F0006);
constexpr int kErrorInvalidPcmItemSize = static_cast<int>(0x807F0007);
constexpr int kErrorInvalidCtrlPointer = static_cast<int>(0x807F0008);
constexpr int kErrorInvalidParamPointer = static_cast<int>(0x807F0009);
constexpr int kErrorInvalidBsiInfoPointer = static_cast<int>(0x807F000A);
constexpr int kErrorInvalidAuInfoPointer = static_cast<int>(0x807F000B);
constexpr int kErrorInvalidPcmItemPointer = static_cast<int>(0x807F000C);
constexpr int kErrorInvalidAuPointer = static_cast<int>(0x807F000D);
constexpr int kErrorInvalidPcmPointer = static_cast<int>(0x807F000E);
constexpr int kErrorInvalidHandle = static_cast<int>(0x807F000F);
constexpr int kErrorInvalidWordLength = static_cast<int>(0x807F0010);
constexpr int kErrorInvalidAuSize = static_cast<int>(0x807F0011);
constexpr int kErrorInvalidPcmSize = static_cast<int>(0x807F0012);
constexpr int kErrorApiFail = static_cast<int>(0x807F0000);
constexpr int kErrorM4aacInvalidSamplingFreq = static_cast<int>(0x807F0300);
constexpr int kErrorM4aacInvalidEnableHeaac = static_cast<int>(0x807F0302);
constexpr int kErrorM4aacInvalidConfigNumber = static_cast<int>(0x807F0303);
constexpr int kErrorM4aacInvalidMaxChannels = static_cast<int>(0x807F0304);
constexpr int kErrorM4aacInvalidEnableNondelayOutput = static_cast<int>(0x807F0305);

// PCM word sizes: 2 is 32-bit float (titles size output as channels * 4 bytes), others 16-bit.
constexpr std::uint32_t kWordSizeFloat = 2;

struct M4aacParam {
    std::uint32_t size;
    std::uint32_t wordSize;
    std::uint32_t configNumber;  // 1 ADTS, 2 raw (shadPS4 AJM ConfigType)
    std::uint32_t samplingFrequencyIndex;
    std::uint32_t maxChannels;
    std::uint32_t enableHeaac;
    std::uint32_t enableNondelayOutput;
    std::uint32_t surroundChannelInterleaveOrder;
};
static_assert(sizeof(M4aacParam) == 0x20);

// Titles read the channel count from the third word (they index channel maps by channels - 1 and
// size PCM as channels * 4 bytes).
struct M4aacBsiInfo {
    std::uint32_t size;
    std::uint32_t samplingFrequency;
    std::uint32_t channels;
    std::uint32_t bitrate;
    std::uint32_t heaac;
};
static_assert(sizeof(M4aacBsiInfo) == 0x14);

struct Decoder {
    std::uint32_t codec;
    std::unique_ptr<Audiodec::AacDecoder> aac;
};

std::mutex& decodersMutex() {
    static std::mutex value;
    return value;
}

std::map<int, std::shared_ptr<Decoder>>& decoders() {
    static std::map<int, std::shared_ptr<Decoder>> value;
    return value;
}

int nextHandle = 1;

bool knownCodec(std::uint32_t codec) {
    return codec == kCodecAt9 || codec == kCodecMp3 || codec == kCodecM4aac;
}

std::shared_ptr<Decoder> find(int handle) {
    std::lock_guard lock(decodersMutex());
    const auto it = decoders().find(handle);
    return it == decoders().end() ? nullptr : it->second;
}

// Debug aid: ANYPS5_TRACE_AUDIODEC=1 dumps codec types and the first bytes of each control block.
bool tracing() {
    static const bool enabled = std::getenv("ANYPS5_TRACE_AUDIODEC") != nullptr;
    return enabled;
}

void traceBlock(const char* name, const void* block) {
    if (block == nullptr) { std::fprintf(stderr, "[audiodec]   %s=null\n", name); return; }
    const auto* words = static_cast<const std::uint32_t*>(block);
    std::fprintf(stderr, "[audiodec]   %s=%p:", name, block);
    for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %08x", words[i]);
    std::fprintf(stderr, "\n");
}

void trace(const char* function, std::uint32_t value, const AudiodecCtrl* ctrl) {
    if (!tracing()) return;
    std::fprintf(stderr, "[audiodec] %s %u\n", function, value);
    if (ctrl == nullptr) return;
    traceBlock("param", ctrl->pParam);
    traceBlock("bsi", ctrl->pBsiInfo);
    traceBlock("au", ctrl->pAuInfo);
    if (ctrl->pAuInfo != nullptr && ctrl->pAuInfo->p_au_addr != nullptr) traceBlock("au-data", ctrl->pAuInfo->p_au_addr);
    traceBlock("pcm", ctrl->pPcmItem);
}

int createM4aac(const AudiodecCtrl& ctrl, Decoder& decoder) {
    const auto& param = *static_cast<const M4aacParam*>(ctrl.pParam);
    if (param.size != sizeof(M4aacParam)) return kErrorInvalidParamSize;
    if (static_cast<const M4aacBsiInfo*>(ctrl.pBsiInfo)->size < sizeof(M4aacBsiInfo)) return kErrorInvalidBsiInfoSize;
    if (param.configNumber != 1 && param.configNumber != 2) return kErrorM4aacInvalidConfigNumber;
    if (param.samplingFrequencyIndex > 11) return kErrorM4aacInvalidSamplingFreq;
    if (param.maxChannels == 0 || param.maxChannels > 8) return kErrorM4aacInvalidMaxChannels;
    if (param.enableHeaac > 1) return kErrorM4aacInvalidEnableHeaac;
    if (param.enableNondelayOutput > 1) return kErrorM4aacInvalidEnableNondelayOutput;
    decoder.aac = Audiodec::CreateAacDecoder({param.configNumber == 1, param.samplingFrequencyIndex, param.wordSize == kWordSizeFloat, param.enableNondelayOutput != 0});
    if (!decoder.aac) {
        NotImplemented_nid_no_patch("sceAudiodecCreateDecoder (AAC: this build has no FFmpeg)");
        return kErrorApiFail;
    }
    return 0;
}

int decodeM4aac(Decoder& decoder, AudiodecCtrl& ctrl) {
    auto& au = *ctrl.pAuInfo;
    auto& pcm = *ctrl.pPcmItem;
    try {
        const auto frame = decoder.aac->Decode(std::span(static_cast<const std::byte*>(au.p_au_addr), au.ui_au_size), std::span(static_cast<std::byte*>(pcm.p_pcm_addr), pcm.ui_pcm_size));
        au.ui_au_size = static_cast<std::uint32_t>(frame.consumedBytes);
        pcm.ui_pcm_size = static_cast<std::uint32_t>(frame.pcmBytes);
        auto& bsi = *static_cast<M4aacBsiInfo*>(ctrl.pBsiInfo);
        bsi.channels = frame.channels;
        bsi.samplingFrequency = frame.sampleRate;
        bsi.bitrate = frame.bitrate;
        bsi.heaac = frame.heaac ? 1u : 0u;
        if (tracing()) std::fprintf(stderr, "[audiodec] decoded %zu bytes into %zu PCM bytes (%u channels, %u Hz, HE-AAC %d)\n", frame.consumedBytes, frame.pcmBytes, frame.channels, frame.sampleRate, frame.heaac ? 1 : 0);
        return 0;
    } catch (const std::length_error&) {
        return kErrorInvalidPcmSize;
    } catch (const std::runtime_error& error) {
        if (tracing()) std::fprintf(stderr, "[audiodec] decode failed: %s\n", error.what());
        return kErrorApiFail;
    }
}

}

extern "C" {

int32_t APS5_VABI sceAudiodecInitLibrary(uint32_t codec_type) {
    trace(__func__, codec_type, nullptr);
    return knownCodec(codec_type) ? 0 : kErrorInvalidType;
}

int32_t APS5_VABI sceAudiodecTermLibrary(uint32_t codec_type) {
    return knownCodec(codec_type) ? 0 : kErrorInvalidType;
}

int32_t APS5_VABI sceAudiodecCreateDecoder(AudiodecCtrl* ctrl, uint32_t codec_type) {
    trace(__func__, codec_type, ctrl);
    if (ctrl == nullptr) return kErrorInvalidCtrlPointer;
    if (ctrl->pParam == nullptr) return kErrorInvalidParamPointer;
    if (ctrl->pBsiInfo == nullptr) return kErrorInvalidBsiInfoPointer;
    if (!knownCodec(codec_type)) return kErrorInvalidType;
    auto decoder = std::make_shared<Decoder>();
    decoder->codec = codec_type;
    if (codec_type != kCodecM4aac) {
        // The AT9 and MP3 parameter layouts have not been confirmed from titles yet.
        NotImplemented_nid_no_patch(codec_type == kCodecAt9 ? "sceAudiodecCreateDecoder (AT9)" : "sceAudiodecCreateDecoder (MP3)");
        return kErrorApiFail;
    }
    if (const auto result = createM4aac(*ctrl, *decoder); result != 0) return result;
    std::lock_guard lock(decodersMutex());
    const auto handle = nextHandle++;
    decoders().emplace(handle, std::move(decoder));
    return handle;
}

int32_t APS5_VABI sceAudiodecDecode(int32_t handle, AudiodecCtrl* ctrl) {
    trace(__func__, static_cast<std::uint32_t>(handle), ctrl);
    const auto decoder = find(handle);
    if (!decoder) return kErrorInvalidHandle;
    if (ctrl == nullptr) return kErrorInvalidCtrlPointer;
    if (ctrl->pParam == nullptr) return kErrorInvalidParamPointer;
    if (ctrl->pBsiInfo == nullptr) return kErrorInvalidBsiInfoPointer;
    if (ctrl->pAuInfo == nullptr) return kErrorInvalidAuInfoPointer;
    if (ctrl->pPcmItem == nullptr) return kErrorInvalidPcmItemPointer;
    if (ctrl->pAuInfo->ui_size != sizeof(AudiodecAuInfo)) return kErrorInvalidAuInfoSize;
    if (ctrl->pPcmItem->ui_size != sizeof(AudiodecPcmItem)) return kErrorInvalidPcmItemSize;
    if (ctrl->pAuInfo->p_au_addr == nullptr) return kErrorInvalidAuPointer;
    if (ctrl->pPcmItem->p_pcm_addr == nullptr) return kErrorInvalidPcmPointer;
    if (ctrl->pAuInfo->ui_au_size == 0) return kErrorInvalidAuSize;
    if (static_cast<const M4aacParam*>(ctrl->pParam)->wordSize > 3) return kErrorInvalidWordLength;
    return decodeM4aac(*decoder, *ctrl);
}

int32_t APS5_VABI sceAudiodecClearContext(int32_t handle) {
    const auto decoder = find(handle);
    if (!decoder) return kErrorInvalidHandle;
    if (decoder->aac) decoder->aac->Reset();
    return 0;
}

int32_t APS5_VABI sceAudiodecDeleteDecoder(int32_t handle) {
    std::lock_guard lock(decodersMutex());
    return decoders().erase(handle) != 0 ? 0 : kErrorInvalidHandle;
}

}
