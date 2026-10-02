#include <algorithm>
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "Optimization/RequestMemoryView.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#if ANYPS5_ENABLE_SPIRV_TOOLS
#include "SpirvBackend/SpirvOptimizer.hpp"
#endif
#include "CacheKey.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <iostream>
#include <map>
#include <future>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
void expectFailure(TAction action, const char* expected, const char* message) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, "unexpected failure reason");
        return;
    }
    throw std::runtime_error(message);
}

void verifyResult(const ShaderRecompiler::RecompileResult& first, const ShaderRecompiler::RecompileResult& second) {
    require(first.spirv == second.spirv, "replayed SPIR-V differs");
    require(first.pushConstants == second.pushConstants, "replayed push constants differ");
    require(first.bdaAbiVersion == second.bdaAbiVersion && first.bindings.size() == second.bindings.size(), "replayed layout differs");
    for (std::size_t index = 0; index < first.bindings.size(); ++index) {
        const auto& left = first.bindings[index];
        const auto& right = second.bindings[index];
        require(left.kind == right.kind && left.role == right.role && left.descriptorSet == right.descriptorSet && left.binding == right.binding && left.count == right.count && left.guestDescriptor == right.guestDescriptor && left.readOnly == right.readOnly, "replayed binding differs");
    }
}

void verifyRegisterSources() {
    using namespace ShaderRecompiler;
    IrResourcePlan plan;
    IrValue samplerRegister(IrOpcode::Void, IrType::ScalarReg, 0);
    IrValue bufferRegister(IrOpcode::Void, IrType::ScalarReg, 1);
    IrValue sameSamplerRegister(IrOpcode::Void, IrType::ScalarReg, 2);
    samplerRegister.SetRegister({RegisterBank::Scalar, 8});
    bufferRegister.SetRegister({RegisterBank::Scalar, 12});
    sameSamplerRegister.SetRegister({RegisterBank::Scalar, 8});
    IrValue samplerRead(IrOpcode::GetUserData, IrType::U32, 3);
    IrValue bufferRead(IrOpcode::GetUserData, IrType::U32, 4);
    IrValue sameSamplerRead(IrOpcode::GetUserData, IrType::U32, 5);
    samplerRead.AddArgument(&samplerRegister);
    bufferRead.AddArgument(&bufferRegister);
    sameSamplerRead.AddArgument(&sameSamplerRegister);
    require(!EquivalentValue(plan, &samplerRead, &bufferRead), "sampler SGPRs were merged with buffer SGPRs");
    require(EquivalentValue(plan, &samplerRead, &sameSamplerRead), "identical user data reads were not recognized");
    sameSamplerRegister.SetRegister({RegisterBank::UserData, 8});
    require(!EquivalentValue(plan, &samplerRead, &sameSamplerRead), "different register banks were merged");
    IrValue firstVector(IrOpcode::Void, IrType::VectorReg, 6);
    IrValue secondVector(IrOpcode::Void, IrType::VectorReg, 7);
    firstVector.SetRegister({RegisterBank::Vector, 0});
    secondVector.SetRegister({RegisterBank::Vector, 1});
    require(!EquivalentValue(plan, &firstVector, &secondVector), "different vector registers were merged");
    require(!EquivalentValue(plan, &samplerRegister, &firstVector), "different register types were merged");
}

void verifyProgramCounterRelativeData() {
    using namespace ShaderRecompiler;
    static const std::array<std::uint32_t, 15> code{
        0xbe801f00u,
        0x800000ffu, 52u,
        0x82010180u,
        0xb0020010u,
        0xbe8303ffu, 0x10005004u,
        0xf4200100u, 0xfa000000u,
        0xbf8cc07fu,
        0x7e000204u,
        0xf80008cfu, 0u,
        0xbf810000u,
        0x3f800000u
    };
    const auto codeAddress = reinterpret_cast<std::uintptr_t>(code.data());
    RecompileRequest request{};
    request.shader = {ShaderStage::Vertex, codeAddress, code, 0, {}};
    request.context.waveSize = 64;
    request.context.userDataBaseRegister = 8;
    request.context.vertex = ShaderVertexStageInfo{};
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.target.fragmentShaderBarycentricEnabled = false;
    request.layout.pushConstantSizeBytes = 128;
    const auto dataBase = [](const RecompileResult& result) {
        for (const auto& binding : result.bindings) {
            if (binding.role != DescriptorRole::GuestBuffers || binding.guestDescriptor.size() < 4u) continue;
            return static_cast<std::uint64_t>(binding.guestDescriptor[0]) | (static_cast<std::uint64_t>(binding.guestDescriptor[1] & 0xffffu) << 32u);
        }
        throw std::runtime_error("program counter data: no guest buffer was bound");
    };
    AgcDriver::ShaderMemory memory({});
    static_cast<void>(memory.Capture(request));
    request.context.memory = memory.Regions();
    const auto first = Recompile(request);
    require(dataBase(first) == codeAddress + 56u, "program counter data: the V# does not name the data at the shader's address");
    auto relocated = request;
    relocated.shader.codeAddress += 0x1000u;
    const auto moved = Recompile(relocated);
    require(moved.cacheHit, "program counter data: relocating the shader recompiled it");
    require(dataBase(moved) == codeAddress + 0x1000u + 56u, "program counter data: the relocated shader bound the old address");
}
void verifyMeshConfiguration() {
    using namespace ShaderRecompiler;
    ShaderMeshInputInfo list;
    list.inputPrimitive = 4u;
    require(list.InputPrimitiveSize() == 3u && list.InputPrimitiveStep() == 3u && list.InputVertexCount(21u) == 63u && list.InputPrimitiveCount(63u) == 21u && list.InputPrimitiveCount(2u) == 0u && list.InputVertexCount(0u) == 0u, "triangle list subgroup sizes changed");
    ShaderMeshInputInfo strip;
    strip.inputPrimitive = 6u;
    require(strip.InputPrimitiveSize() == 3u && strip.InputPrimitiveStep() == 1u && strip.InputVertexCount(21u) == 23u && strip.InputPrimitiveCount(23u) == 21u, "triangle strip subgroup sizes changed");
    ShaderMeshInputInfo lines;
    lines.inputPrimitive = 2u;
    ShaderMeshInputInfo points;
    points.inputPrimitive = 1u;
    require(lines.InputPrimitiveSize() == 2u && lines.InputPrimitiveStep() == 2u && points.InputPrimitiveSize() == 1u && points.InputVertexCount(5u) == 5u, "line or point subgroup sizes changed");

    static constexpr std::array<std::uint32_t, 1> code{0xbf810000u};
    RecompileRequest request{};
    request.shader = {ShaderStage::Mesh, 0x10000u, code, 0, {}};
    request.context.waveSize = 64;
    const MeshConfiguration mesh{4u, 21u, 63u, 64u, 21u, 64u, 256u, 0u, 12u};
    request.graphics = GraphicsCompileContext{0u, {}, mesh, std::nullopt, {}};
    const auto replay = RequestSerializer{}.Deserialize(RequestSerializer{}.Serialize(request));
    require(replay.request.graphics.has_value() && replay.request.graphics->mesh.has_value() && replay.request.graphics->mesh->esgsItemSize == 12u && replay.request.graphics->mesh->primitivesPerGroup == 21u, "mesh configuration was lost in serialization");
    std::vector<std::uint64_t> key;
    RecompileCacheKey::Build(request, key);
    const auto first = key;
    auto other = request;
    auto otherMesh = mesh;
    otherMesh.esgsItemSize = 16u;
    other.graphics = GraphicsCompileContext{0u, {}, otherMesh, std::nullopt, {}};
    RecompileCacheKey::Build(other, key);
    require(key != first && RecompileCacheKey::ContextHash(request) != RecompileCacheKey::ContextHash(other), "the cache keys ignore the mesh configuration");
}

ShaderRecompiler::ShaderPixelStageInfo twoParameterPixel() {
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.interpolatorCount = 2u;
    pixel.interpolatorSettings[1] = 1u;
    pixel.wave32 = true;
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PerspectiveCenter) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::LinearCenter);
    pixel.hasPerspectiveCenterVgpr = true;
    pixel.noPerspective = true;
    pixel.targetOutputMode[0] = 9u;
    pixel.targetExportMapping[0] = 0xe4u;
    return pixel;
}

std::vector<std::uint32_t> noPerspectiveLocations(std::span<const std::uint32_t> code) {
    using namespace ShaderRecompiler;
    RecompileRequest request{};
    request.shader = {ShaderStage::Fragment, 0x30000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.pixel = twoParameterPixel();
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.layout.pushConstantSizeBytes = 128;
    request.useCache = false;
    const auto result = Recompile(request);
    const auto& words = result.spirv.Words();
    std::map<std::uint32_t, std::uint32_t> locations;
    std::vector<std::uint32_t> decorated;
    for (std::size_t at = 5; at < words.size() && (words[at] >> 16u) != 0; at += words[at] >> 16u) {
        if (static_cast<spv::Op>(words[at] & 0xffffu) != spv::OpDecorate) continue;
        if (words[at + 2] == spv::DecorationLocation) locations[words[at + 1]] = words[at + 3];
        if (words[at + 2] == spv::DecorationNoPerspective) decorated.push_back(words[at + 1]);
    }
    std::vector<std::uint32_t> result2;
    for (const auto id : decorated) result2.push_back(locations.count(id) != 0 ? locations.at(id) : 0xffffffffu);
    return result2;
}

void verifyPixelInputs() {
    using namespace ShaderRecompiler;
    require(PixelInputVgpr(0x326u, PixelInput::PerspectiveCentroid) == 2u && PixelInputVgpr(0x326u, PixelInput::LinearCenter) == 4u && PixelInputVgpr(0x326u, PixelInput::PositionX) == 6u, "the SPI_PS_INPUT_ADDR layout moved the inputs");
    require(PixelInputVgpr(0x7afu, PixelInput::PerspectiveCentroid) == 4u && PixelInputVgpr(0x7afu, PixelInput::PositionX) == 12u && PixelInputVgpr(0x7afu, PixelInput::PositionZ) == 14u, "ADDR-only inputs did not reserve their VGPRs");

    static constexpr std::array<std::uint32_t, 7> byPair{0xc8100000u, 0xc8110001u, 0xc8140402u, 0xc8150403u, 0xf800180fu, 0x05040504u, 0xbf810000u};
    const auto linear = noPerspectiveLocations(byPair);
    require(linear.size() == 1u && linear[0] == 1u, "only the parameter interpolated through the linear pair must be NoPerspective");
    static constexpr std::array<std::uint32_t, 7> bothPairs{0xc8100000u, 0xc8110001u, 0xc8140002u, 0xc8150003u, 0xf800180fu, 0x05040504u, 0xbf810000u};
    expectFailure([&] { static_cast<void>(noPerspectiveLocations(bothPairs)); }, "interpolated through both a perspective and a linear I/J pair", "a parameter read through both pairs was given one interpolation");

    static constexpr std::array<std::uint32_t, 1> code{0xbf810000u};
    RecompileRequest request{};
    request.shader = {ShaderStage::Fragment, 0x30000u, code, 0, {}};
    request.context.waveSize = 64;
    auto pixel = twoParameterPixel();
    pixel.inputAddr |= PixelInputBit(PixelInput::PerspectiveCentroid) | PixelInputBit(PixelInput::LinearCentroid);
    pixel.perspectiveCentroid = true;
    pixel.linearCentroid = true;
    request.context.pixel = pixel;
    const auto replay = RequestSerializer{}.Deserialize(RequestSerializer{}.Serialize(request));
    const auto& back = *replay.request.context.pixel;
    require(back.inputAddr == pixel.inputAddr && back.perspectiveCentroid && back.linearCentroid && back.noPerspective, "the pixel input layout did not survive serialization");
    std::vector<std::uint64_t> key;
    RecompileCacheKey::Build(request, key);
    const auto first = key;
    for (const auto change : {0, 1, 2}) {
        auto other = request;
        auto changed = pixel;
        if (change == 0) changed.inputAddr |= PixelInputBit(PixelInput::PerspectiveSample);
        if (change == 1) changed.perspectiveCentroid = false;
        if (change == 2) changed.linearCentroid = false;
        other.context.pixel = changed;
        RecompileCacheKey::Build(other, key);
        require(key != first && RecompileCacheKey::ContextHash(request) != RecompileCacheKey::ContextHash(other), "the cache keys ignore the pixel input layout");
    }
}

ShaderRecompiler::RecompileResult recompileSlots(std::initializer_list<std::uint32_t> controls, std::span<const std::uint32_t> code) {
    using namespace ShaderRecompiler;
    ShaderPixelStageInfo pixel{};
    pixel.interpolatorCount = static_cast<std::uint32_t>(controls.size());
    std::uint32_t index = 0;
    for (const auto control : controls) pixel.interpolatorSettings[index++] = control;
    pixel.inputAddr = PixelInputBit(PixelInput::PerspectiveCenter);
    pixel.hasPerspectiveCenterVgpr = true;
    pixel.targetOutputMode[0] = 9u;
    pixel.targetExportMapping[0] = 0xe4u;
    RecompileRequest request{};
    request.shader = {ShaderStage::Fragment, 0x30000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.pixel = pixel;
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.target.fragmentShaderBarycentricEnabled = true;
    request.layout.pushConstantSizeBytes = 128;
    request.useCache = false;
    return Recompile(request);
}

std::vector<std::pair<std::uint32_t, bool>> slotInputs(std::initializer_list<std::uint32_t> controls, std::span<const std::uint32_t> code) {
    const auto result = recompileSlots(controls, code);
    const auto& words = result.spirv.Words();
    std::map<std::uint32_t, std::uint32_t> locations;
    std::map<std::uint32_t, bool> perVertex;
    std::vector<std::uint32_t> inputs;
    for (std::size_t at = 5; at < words.size() && (words[at] >> 16u) != 0; at += words[at] >> 16u) {
        const auto op = static_cast<spv::Op>(words[at] & 0xffffu);
        if (op == spv::OpVariable && words[at + 3] == spv::StorageClassInput) inputs.push_back(words[at + 2]);
        if (op == spv::OpDecorate && words[at + 2] == spv::DecorationLocation) locations[words[at + 1]] = words[at + 3];
        if (op == spv::OpDecorate && words[at + 2] == spv::DecorationPerVertexKHR) perVertex[words[at + 1]] = true;
    }
    std::vector<std::pair<std::uint32_t, bool>> located;
    for (const auto id : inputs) {
        if (locations.contains(id)) located.emplace_back(locations.at(id), perVertex.contains(id));
    }
    std::sort(located.begin(), located.end());
    return located;
}

void verifyPixelParameterSlots() {
    static constexpr std::array<std::uint32_t, 7> shared{0xc8100000u, 0xc8110001u, 0xc8140500u, 0xc8150501u, 0xf800180fu, 0x05040504u, 0xbf810000u};
    auto inputs = slotInputs({0x3u, 0x3u}, shared);
    require(inputs.size() == 1u && inputs[0].first == 3u && inputs[0].second, "inputs reading one slot were not declared once at the slot");
    inputs = slotInputs({0x404u, 0x0u}, shared);
    require(inputs.size() == 2u && inputs[0].first == 0u && inputs[1].first == 4u, "inputs of different slots moved");
    require(slotInputs({0x20u, 0x2320u}, shared).empty(), "a defaulted input was declared as a parameter");
    static constexpr std::array<std::uint32_t, 8> mixed{0xc8100000u, 0xc8110001u, 0xc8160402u, 0xc81a0802u, 0xc81e0f02u, 0xf800180fu, 0x07060504u, 0xbf810000u};
    inputs = slotInputs({0x0u, 0x400u, 0x22u, 0x320u}, mixed);
    require(inputs.size() == 1u && inputs[0].first == 0u && inputs[0].second, "a slot read flat and interpolated did not become one per-vertex input");
    static constexpr std::array<std::uint32_t, 7> vertices{0xc8120002u, 0xc8160000u, 0xc81a0001u, 0xc81e0302u, 0xf800180fu, 0x07060504u, 0xbf810000u};
    const auto subtracts = [](std::initializer_list<std::uint32_t> controls) {
        const auto result = recompileSlots(controls, vertices);
        const auto& words = result.spirv.Words();
        std::size_t count = 0;
        for (std::size_t at = 5; at < words.size() && (words[at] >> 16u) != 0; at += words[at] >> 16u) count += (words[at] & 0xffffu) == spv::OpFSub;
        return count;
    };
    inputs = slotInputs({0x423u}, vertices);
    require(inputs.size() == 1u && inputs[0].first == 3u && inputs[0].second, "a pass-through input (OFFSET bit 5 with FLAT_SHADE) was not read per vertex at its slot");
    require(subtracts({0x423u}) == 0u, "v_interp_mov p10/p20 of a pass-through input subtracted vertex 0");
    inputs = slotInputs({0x403u}, vertices);
    require(inputs.size() == 1u && inputs[0].first == 3u && inputs[0].second && subtracts({0x403u}) == 2u, "v_interp_mov p10/p20 of a flat input did not read differences to vertex 0");
    require(slotInputs({0x23u}, vertices).empty(), "a defaulted input (OFFSET bit 5 without FLAT_SHADE) was declared as a parameter");
    expectFailure([] { static_cast<void>(recompileSlots({0x423u, 0x3u}, shared)); }, "passes its vertices through unchanged", "an interpolated pass-through input was accepted");
}

}

int main() {
    try {
        using namespace ShaderRecompiler;
        verifyRegisterSources();
        verifyProgramCounterRelativeData();
        verifyMeshConfiguration();
        verifyPixelInputs();
        verifyPixelParameterSlots();
#if ANYPS5_ENABLE_SPIRV_TOOLS
        const std::vector<std::uint32_t> minimalSpirv{
            0x07230203u, 0x00010000u, 0u, 5u, 0u,
            0x00020011u, 1u,
            0x0003000eu, 0u, 1u,
            0x0005000fu, 5u, 3u, 0x6e69616du, 0u,
            0x00060010u, 3u, 17u, 1u, 1u, 1u,
            0x00020013u, 1u,
            0x00030021u, 2u, 1u,
            0x00050036u, 1u, 3u, 0u, 2u,
            0x000200f8u, 4u,
            0x00010000u,
            0x000100fdu,
            0x00010038u
        };
        const auto optimizedSpirv = ValidateAndOptimizeSpirv(minimalSpirv, 0x00401001u, 0x00010000u);
        require(optimizedSpirv.size() < minimalSpirv.size(), "SPIR-V optimization did not remove the no-op");
        require(optimizedSpirv == ValidateAndOptimizeSpirv(minimalSpirv, 0x00401001u, 0x00010000u), "SPIR-V optimization is not deterministic");
#endif
        const std::array<std::uint32_t, 8> code{0xf4040004u, 0xfa000000u, 0xf4000080u, 0xfa000000u, 0x7e000202u, 0xf80008cfu, 0u, 0xbf810000u};
        std::uint32_t payload = 0x3f800000u;
        std::uint64_t table = reinterpret_cast<std::uintptr_t>(&payload);
        const auto address = reinterpret_cast<std::uintptr_t>(&table);
        const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)};
        RecompileRequest request{};
        request.shader = {ShaderStage::Vertex, 0x10000u, code, 0, {}};
        request.context.waveSize = 64;
        request.context.userDataBaseRegister = 8;
        request.context.userData = userData;
        request.context.vertex = ShaderVertexStageInfo{};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 64;
        request.target.fragmentShaderBarycentricEnabled = false;
        request.layout.pushConstantSizeBytes = 128;

        expectFailure([&] { static_cast<void>(Recompile(request)); }, "SrtWalker::EvaluateRuntimeSources", "missing snapshot unexpectedly read live memory");
        AgcDriver::ShaderMemory memory({});
        memory.Capture(request);
        auto regions = memory.Regions();
        // The table's two dwords and the payload it points to are captured (reads take whole lines).
        const auto holds = [&](std::uint64_t dword, std::uint32_t expected) {
            return std::any_of(regions.begin(), regions.end(), [&](const MemoryRegion& region) {
                if (dword < region.guestAddress || dword + 4 > region.guestAddress + region.bytes.size()) return false;
                std::uint32_t value = 0;
                std::memcpy(&value, region.bytes.data() + (dword - region.guestAddress), sizeof(value));
                return value == expected;
            });
        };
        require(holds(address, static_cast<std::uint32_t>(table)) && holds(address + 4, static_cast<std::uint32_t>(table >> 32u)) && holds(reinterpret_cast<std::uintptr_t>(&payload), payload), "nested pointer reads were not captured");
        request.context.memory = regions;
        const auto first = Recompile(request);
        require(!first.spirv.empty(), "empty compiled shader");
        require(!first.cacheHit, "first shader compilation unexpectedly hit the cache");
        const auto plan = GetResourcePlan(request);
        require(plan == GetResourcePlan(request), "resource plan was rebuilt");
        const auto cached = Recompile(request);
        require(cached.cacheHit, "unchanged shader did not hit the cache");
        verifyResult(first, cached);
        auto relocated = request;
        relocated.shader.codeAddress += 0x1000;
        require(Recompile(relocated).cacheHit, "shader relocation caused recompilation");
        auto changedTarget = request;
        changedTarget.target.subgroupSize = 32;
        require(GetResourcePlan(changedTarget) != plan, "different target reused the source entry");
        std::vector<std::uint32_t> changedCode(code.begin(), code.end());
        changedCode.insert(changedCode.begin(), 0xbf800000u);
        auto changedSource = request;
        changedSource.shader.code = changedCode;
        require(GetResourcePlan(changedSource) != plan, "changed code reused the source entry");
        auto uncached = request;
        uncached.useCache = false;
        require(GetResourcePlan(uncached) != plan, "disabled cache reused the resource plan");
        const auto fresh = Recompile(uncached);
        require(!fresh.cacheHit, "disabled cache reused the compiled variant");
        verifyResult(first, fresh);
        require(!RequestSerializer{}.Deserialize(RequestSerializer{}.Serialize(uncached)).request.useCache, "cache policy was lost in serialization");
        auto changedLayout = request;
        changedLayout.layout.pushConstantSizeBytes = 64;
        require(!Recompile(changedLayout).cacheHit, "binding layout change reused an incompatible variant");
        require(Recompile(changedLayout).cacheHit, "new binding layout variant was not cached");
        require(Recompile(request).cacheHit, "compiling a new variant evicted the original");
        auto missingMemory = request;
        missingMemory.context.memory = {};
        expectFailure([&] { static_cast<void>(Recompile(missingMemory)); }, "SrtWalker::EvaluateRuntimeSources", "cache hit bypassed resource validation");
#if ANYPS5_ENABLE_SPIRV_TOOLS
        auto invalidSpirv = first.spirv;
        invalidSpirv[0] = 0;
        expectFailure([&] { static_cast<void>(ValidateAndOptimizeSpirv(invalidSpirv, request.target.vulkanVersion, request.target.spirvVersion)); }, "SPIR-V validation before optimization failed", "invalid SPIR-V passed validation");
        expectFailure([&] { static_cast<void>(ValidateAndOptimizeSpirv(first.spirv, 0x00400000u, 0x00010600u)); }, "unsupported Vulkan/SPIR-V target", "incompatible target accepted");
        expectFailure([&] { static_cast<void>(ValidateAndOptimizeSpirv(first.spirv, 0x00405000u, 0x00010600u)); }, "unsupported Vulkan target", "unknown Vulkan target accepted");
#endif
        payload = 0x40000000u;
        AgcDriver::ShaderMemory updatedMemory({});
        updatedMemory.Capture(request);
        const auto updatedRegions = updatedMemory.Regions();
        auto updated = request;
        updated.context.memory = updatedRegions;
        const auto updatedCached = Recompile(updated);
        require(updatedCached.cacheHit, "dynamic shader data caused recompilation");
        updated.useCache = false;
        verifyResult(updatedCached, Recompile(updated));
        bool changedData = updatedCached.pushConstants != first.pushConstants;
        for (std::size_t i = 0; i < first.bindings.size(); ++i) changedData = changedData || updatedCached.bindings.at(i).guestDescriptor != first.bindings[i].guestDescriptor;
        require(changedData, "cache hit retained stale shader data");
        auto concurrent = request;
        concurrent.layout.pushConstantSizeBytes = 60;
        std::array<std::future<RecompileResult>, 4> concurrentResults;
        for (auto& future : concurrentResults) future = std::async(std::launch::async, [concurrent] { return Recompile(concurrent); });
        std::uint32_t compilations = 0;
        for (auto& future : concurrentResults) {
            const auto result = future.get();
            if (!result.cacheHit) ++compilations;
        }
        require(compilations == 1, "concurrent requests compiled the same variant repeatedly");
        const auto serialized = RequestSerializer{}.Serialize(request);
        table = 0;
        payload = 0xdeadbeefu;
        verifyResult(first, Recompile(request));
        auto replay = RequestSerializer{}.Deserialize(serialized);
        verifyResult(first, Recompile(replay.request));
        RequestMemoryView view(replay.request.context.memory);
        const auto runtime = view.MakeRuntime(userData, request.shader.codeAddress);
        std::uint32_t captured = 0;
        require(runtime.readMemory(runtime.userContext, reinterpret_cast<std::uintptr_t>(&payload), &captured) && captured == 0x3f800000u, "snapshot changed with live memory");

        request.context.userDataBaseRegister = 0x8c;
        expectFailure([&] { static_cast<void>(PrepareResourceProgram(request)); }, "shader user data exceeds the scalar register bank", "PM4 register address accepted as SGPR base");
        request.context.userDataBaseRegister = 105;
        expectFailure([&] { static_cast<void>(PrepareResourceProgram(request)); }, "shader user data exceeds the scalar register bank", "user data overran scalar register bank");
        request.context.userDataBaseRegister = 8;
        request.context.memory = {};
        AgcDriver::ShaderMemory invalid({});
        expectFailure([&] { invalid.Capture(request); }, "null or misaligned address", "null nested pointer was accepted");
        std::cout << "Shader memory capture, strict validation and deterministic replay passed\n";
        return 0;
    } catch (const std::exception& error) {
        const std::string message(error.what());
        std::cerr << message.substr(0, message.find("RecompileRequest:")) << '\n';
        return 1;
    }
}
