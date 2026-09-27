#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

// Debug aid: ANYPS5_TRACE_AUDIODEC=1 dumps codec types and the first bytes of each control block.
void TraceBlock(const char* name, const void* block) {
    if (block == nullptr) { std::fprintf(stderr, "[audiodec]   %s=null\n", name); return; }
    const auto* words = static_cast<const std::uint32_t*>(block);
    std::fprintf(stderr, "[audiodec]   %s=%p:", name, block);
    for (int i = 0; i < 12; ++i) std::fprintf(stderr, " %08x", words[i]);
    std::fprintf(stderr, "\n");
}

void Trace(const char* function, std::uint32_t codec, const AudiodecCtrl* ctrl) {
    static const bool enabled = std::getenv("ANYPS5_TRACE_AUDIODEC") != nullptr;
    if (!enabled) return;
    std::fprintf(stderr, "[audiodec] %s codec=%u\n", function, codec);
    if (ctrl == nullptr) return;
    TraceBlock("param", ctrl->pParam);
    TraceBlock("bsi", ctrl->pBsiInfo);
    TraceBlock("au", ctrl->pAuInfo);
    TraceBlock("pcm", ctrl->pPcmItem);
}

}

extern "C" {

int32_t APS5_VABI sceAudiodecClearContext(int32_t handle) {
 (void)handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudiodecCreateDecoder(AudiodecCtrl* ctrl, uint32_t codec_type) {
 Trace(__func__, codec_type, ctrl);
 (void)ctrl;
 (void)codec_type;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudiodecDecode(int32_t handle, AudiodecCtrl* ctrl) {
 Trace(__func__, static_cast<std::uint32_t>(handle), ctrl);
 (void)handle;
 (void)ctrl;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudiodecDeleteDecoder(int32_t handle) {
 (void)handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudiodecInitLibrary(uint32_t codec_type) {
 Trace(__func__, codec_type, nullptr);
 (void)codec_type;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudiodecTermLibrary(uint32_t codec_type) {
 (void)codec_type;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

}
