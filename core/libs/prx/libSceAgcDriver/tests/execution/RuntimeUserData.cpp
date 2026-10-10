#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/SrtWalker.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Runtime user data (Recompile(request, capture, memoHit, runtimeUserData)): a result that reads user
// data only as push data is shared across dispatches that differ only in user data, and each dispatch
// supplies its own words (ApplyRuntimeUserData) as push data.

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

alignas(256) std::array<std::uint32_t, 4> Output{};

// v_mov_b32 v3, s0; v_mov_b32 v1, 0; buffer_store_dword v3, v1, s[4:7], 0 idxen;
// v_mov_b32 v3, s1; v_mov_b32 v1, 1; buffer_store_dword v3, v1, s[4:7], 0 idxen; s_endpgm
alignas(256) constexpr std::array<std::uint32_t, 9> StoreUserDataCode{
    0x7e060200, 0x7e020280, 0xe0702000, 0x80010301,
    0x7e060201, 0x7e020281, 0xe0702000, 0x80010301,
    0xbf810000,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), count, 0x11016facu};
}

struct Invocation {
    std::shared_ptr<const ShaderRecompiler::RecompileResult> result;
    bool memoHit = false;
    bool overridden = false;
};

Invocation Dispatch(AgcDriver::VulkanDevice& device, std::vector<std::uint32_t>& userData, std::uint32_t first, std::uint32_t second, bool runtimeUserData) {
    userData[0] = first;
    userData[1] = second;
    const std::span<const std::uint32_t> code(StoreUserDataCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{64, 1, 1}, 0, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    ShaderRecompiler::SrtRuntime runtime{};
    runtime.userData = userData;
    runtime.shaderBase = request.shader.codeAddress;
    const auto capture = ShaderRecompiler::CaptureResources(request, runtime);
    Invocation invocation;
    invocation.result = ShaderRecompiler::Recompile(request, *capture, &invocation.memoHit, runtimeUserData);
    std::vector<std::byte> push;
    invocation.overridden = ShaderRecompiler::ApplyRuntimeUserData(*invocation.result, userData, push);
    Output.fill(0xdeadbeefu);
    device.Dispatch(*invocation.result, 1, 1, 1, {}, 0, invocation.overridden ? &push : nullptr);
    device.WaitIdle();
    Require(Output[0] == first && Output[1] == second, "runtime user data: dispatch stored " + std::to_string(Output[0]) + ", " + std::to_string(Output[1]) + ", expected " + std::to_string(first) + ", " + std::to_string(second));
    return invocation;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (device->Target().subgroupSize < 64 && device->Target().subgroupSize != 32) return VulkanTestSkipped;
        std::vector<std::uint32_t> userData(8, 0u);
        const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size()));
        std::copy(output.begin(), output.end(), userData.begin() + 4);

        const auto first = Dispatch(*device, userData, 11, 12, true);
        Require(!first.memoHit && !first.overridden, "runtime user data: the first dispatch should materialize its own result");
        Require(first.result->pushUserData.size() >= 2, "runtime user data: the program does not read its user data as push data");
        // Other user data: the result is shared and the dispatch supplies its words.
        const auto second = Dispatch(*device, userData, 21, 22, true);
        Require(second.memoHit && second.result == first.result && second.overridden, "runtime user data: a dispatch differing only in user data did not share the result");
        // The first user data again: the shared result's own push data holds the second's words.
        const auto third = Dispatch(*device, userData, 11, 12, true);
        Require(third.result == first.result, "runtime user data: the result was not kept");
        // Without runtime user data every distinct user data has its own result, its push data its own.
        const auto own = Dispatch(*device, userData, 31, 32, false);
        Require(!own.overridden && own.result != first.result, "runtime user data: a dispatch without it shared a result across user data");
        const auto ownAgain = Dispatch(*device, userData, 41, 42, false);
        Require(!ownAgain.overridden && ownAgain.result != own.result, "runtime user data: a dispatch without it shared a result across user data");
        std::puts("runtime user data tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
