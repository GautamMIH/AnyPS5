#include "prx/libSceAgc/DcbState/include/Workload.hpp"

#include "prx/libSceAgc/Command/include/Packet.hpp"
#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr std::uint32_t OpcodeNop = 0x10;

uint32_t* EmitNop(CommandBuffer* buf, const char* function) {
 Agc::Command::Require(buf != nullptr, function, "null command buffer");
 auto* packet = Agc::Command::Allocate(buf, 2, function);
 packet[0] = Agc::Command::Header(OpcodeNop, 2, 0);
 packet[1] = 0;
 return packet;
}

}

extern "C" {

uint32_t* APS5_VABI sceAgcDcbSetWorkloadComplete(CommandBuffer* buf, uint32_t stream_id, uint32_t workload_id) {
 (void)stream_id;
 (void)workload_id;
 return EmitNop(buf, __func__);
}

uint32_t* APS5_VABI sceAgcDcbSetWorkloadsActive(CommandBuffer* buf, uint32_t stream_id, const uint32_t* workload_ids, uint32_t workload_count) {
 (void)stream_id;
 (void)workload_ids;
 (void)workload_count;
 return EmitNop(buf, __func__);
}

}
