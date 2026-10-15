#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Threads = 256;

alignas(256) constexpr std::array<std::uint32_t, 8> TgSizeWaveIdCode{
    0x9384ff03, 0x00060006, 0x34020082, 0x7e040204, 0xdc708000, 0x00000201, 0xbf810000, 0xbf810000,
};

// This driver grants address-table writes only to guest memory (not the host heap), so the stored-to
// block is a guest mapping.
class GuestBlock {
public:
    GuestBlock() : memory(BlockBytes) {}
    std::uint8_t* Data() { return memory.As<std::uint8_t>(); }

private:
    GuestTestMemory memory;
};

void Run(AgcDriver::VulkanDevice& device, GuestBlock& guest, std::uint32_t waveSize) {
    std::memset(guest.Data(), 0xff, BlockBytes);
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(guest.Data()));
    const std::vector<std::uint32_t> userData{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)};
    const std::span<const std::uint32_t> code(TgSizeWaveIdCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {true, false, false}, true, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    std::array<std::uint32_t, Threads> waves{};
    std::memcpy(waves.data(), guest.Data(), sizeof(waves));
    for (std::uint32_t thread = 0; thread < Threads; ++thread) {
        if (waves[thread] == thread / waveSize) continue;
        Require(false, "wave" + std::to_string(waveSize) + " thread " + std::to_string(thread) + " read wave " + std::to_string(waves[thread]) + " from TG_SIZE[11:6], expected " + std::to_string(thread / waveSize));
    }
    std::printf("wave%u: TG_SIZE[11:6] holds the wave index for all %u threads\n", waveSize, Threads);
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock guest;
        Run(*device, guest, 64);
        Run(*device, guest, 32);
        std::puts("TG_SIZE wave id tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
