#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "SceShaders.hpp"
#include "prx/libSceAgc/Shader/include/ShaderConstants.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace AgcDriver::Graphics {
namespace {

constexpr std::uint32_t computeNumThreadX = 0x207;
constexpr std::uint32_t computeNumThreadY = 0x208;
constexpr std::uint32_t computeNumThreadZ = 0x209;
constexpr std::uint32_t computePgmRsrc2 = 0x213;

constexpr std::uint32_t spiPsInputCntl0 = 0x191;
constexpr std::uint32_t spiPsInputEna = 0x1B3;
constexpr std::uint32_t spiPsInputAddr = 0x1B4;
constexpr std::uint32_t spiPsInControl = 0x1B6;
constexpr std::uint32_t dbShaderControl = 0x203;
constexpr std::uint32_t spiShaderColFormat = 0x1C5;

std::uint32_t read(const Registers& registers, std::uint32_t offset) {
    const auto it = registers.find(offset);
    if (it == registers.end()) {
        char message[64];
        std::snprintf(message, sizeof(message), "AGC graphics: missing register at DWORD 0x%x", offset);
        throw std::runtime_error(message);
    }
    return it->second;
}

// `bytes`: the leading bytes of T the header holds (a struct's tail padding may lie past the header;
// upstream 50f1f1aa); the rest is value-initialized.
template <typename T> T _readHeaderPod(std::span<const std::byte> header, std::uint64_t headerAddress, const void* pointer, std::size_t bytes = sizeof(T)) {
    if (pointer == nullptr) throw std::runtime_error("AGC graphics: null AGC header pointer");
    const auto address = reinterpret_cast<std::uint64_t>(pointer);
    if (address < headerAddress) throw std::runtime_error("AGC graphics: AGC header pointer precedes the shader header");
    const auto offset = address - headerAddress;
    if (offset > header.size() || bytes > header.size() - offset) throw std::runtime_error("AGC graphics: AGC header pointer is outside the registered shader header");
    T value{};
    std::memcpy(&value, header.data() + offset, bytes);
    return value;
}

template <typename T> void _readHeaderArray(std::span<const std::byte> header, std::uint64_t headerAddress, const void* pointer, std::uint32_t count, T* destination) {
    if (count == 0) return;
    if (pointer == nullptr) throw std::runtime_error("AGC graphics: null AGC header array pointer");
    const auto address = reinterpret_cast<std::uint64_t>(pointer);
    if (address < headerAddress) throw std::runtime_error("AGC graphics: AGC header array pointer precedes the shader header");
    const auto offset = address - headerAddress;
    const auto bytes = static_cast<std::uint64_t>(count) * sizeof(T);
    if (offset + bytes > header.size()) throw std::runtime_error("AGC graphics: AGC header array is outside the registered shader header");
    std::memcpy(destination, header.data() + offset, bytes);
}

}

ShaderRecompiler::ShaderComputeStageInfo DecodeComputeStageInfo(const Registers& shader, std::span<const std::byte> header) {
    const auto numThreadX = read(shader, computeNumThreadX);
    const auto numThreadY = read(shader, computeNumThreadY);
    const auto numThreadZ = read(shader, computeNumThreadZ);
    if (numThreadX == 0 || numThreadY == 0 || numThreadZ == 0) {
        throw std::runtime_error("AGC graphics: COMPUTE_NUM_THREAD_X/Y/Z must be nonzero");
    }
    const auto rsrc2 = read(shader, computePgmRsrc2);
    // SCRATCH_EN: the per-lane private size comes from the AGC shader header; the translator lowers
    // scratch accesses to a per-invocation array of that many dwords (upstream 20e055c2).
    std::uint32_t scratchDwords = 0;
    if ((rsrc2 & 0x1u) != 0) {
        if (header.size() < sizeof(Shader)) throw std::runtime_error("AGC graphics: COMPUTE_PGM_RSRC2.SCRATCH_EN without an AGC shader header");
        Shader agcShader;
        std::memcpy(&agcShader, header.data(), sizeof(Shader));
        scratchDwords = agcShader.scratch_size_dw_per_thread;
        if (scratchDwords == 0) throw std::runtime_error("AGC graphics: COMPUTE_PGM_RSRC2.SCRATCH_EN with a zero scratch size");
    }
    return ShaderRecompiler::ShaderComputeStageInfo{
        {numThreadX, numThreadY, numThreadZ},
        ((rsrc2 >> 15u) & 0x1FFu) * 128u,
        {((rsrc2 >> 7u) & 0x1u) != 0, ((rsrc2 >> 8u) & 0x1u) != 0, ((rsrc2 >> 9u) & 0x1u) != 0},
        ((rsrc2 >> 10u) & 0x1u) != 0,
        ((rsrc2 >> 11u) & 0x3u) + 1u,
        {},
        scratchDwords
    };
}

ShaderRecompiler::ShaderPixelStageInfo DecodePixelStageInfo(const Registers& context, const std::array<std::uint8_t, 8>& colorComponentMappings) {
    const auto inControl = read(context, spiPsInControl);
    const auto inputNum = inControl & 0x3Fu;
    if (inputNum > 32u) {
        throw std::runtime_error("AGC graphics: SPI_PS_IN_CONTROL input count exceeds 32");
    }
    const auto ena = read(context, spiPsInputEna);
    const auto addr = read(context, spiPsInputAddr);
    const auto activeInputs = ena & addr;
    // Perspective and linear sample, center and centroid pairs (rendering is single-sampled, so a
    // centroid is the center), POS_X..W, FRONT_FACE, ANCILLARY.
    constexpr std::uint32_t knownMask = 0x1u | 0x2u | 0x4u | 0x10u | 0x20u | 0x40u | 0x100u | 0x200u | 0x400u | 0x800u | 0x1000u | 0x2000u | 0x4000u | 0x8000u;
    if ((activeInputs & ~knownMask) != 0) {
        char message[128];
        std::snprintf(message, sizeof(message), "AGC graphics: unsupported SPI_PS_INPUT_ENA 0x%x / ADDR 0x%x", ena, addr);
        throw std::runtime_error(message);
    }
    std::array<std::uint32_t, 32> interpolatorSettings{};
    for (std::uint32_t i = 0; i < inputNum; ++i) {
        interpolatorSettings[i] = read(context, spiPsInputCntl0 + i);
    }
    const auto shaderControl = read(context, dbShaderControl);
    if (((shaderControl >> 9u) & 0x1u) != 0) {
        throw std::runtime_error("AGC graphics: DB_SHADER_CONTROL.DUAL_EXPORT_ENABLE is unsupported");
    }
    if (((shaderControl >> 11u) & 0x1u) != 0) {
        throw std::runtime_error("AGC graphics: DB_SHADER_CONTROL.ALPHA_TO_MASK_DISABLE is unsupported");
    }
    if (((shaderControl >> 13u) & 0x3u) != 0) {
        throw std::runtime_error("AGC graphics: DB_SHADER_CONTROL.CONSERVATIVE_Z_EXPORT is unsupported");
    }
    const auto colFormat = read(context, spiShaderColFormat);
    std::array<std::uint8_t, 8> targetOutputMode{};
    for (std::uint32_t i = 0; i < 8u; ++i) {
        targetOutputMode[i] = static_cast<std::uint8_t>((colFormat >> (4u * i)) & 0xFu);
    }
    const bool pixelKillEnable = ((shaderControl >> 6u) & 0x1u) != 0;
    const bool depthExportEnable = (shaderControl & 0x1u) != 0;
    const bool sampleMaskExportEnable = ((shaderControl >> 8u) & 0x1u) != 0;
    const auto zOrder = (shaderControl >> 4u) & 0x3u;
    // SPI_PS_INPUT_ENA/ADDR bits: 0 PERSP_SAMPLE, 1 PERSP_CENTER, 2 PERSP_CENTROID, 4 LINEAR_SAMPLE,
    // 5 LINEAR_CENTER, 6 LINEAR_CENTROID, 8-11 POS_X..W, 12 FRONT_FACE, 13 ANCILLARY. The flags are
    // the loaded inputs; inputAddr places every named input's VGPRs, loaded or not.
    const auto loaded = [&](std::uint32_t bit) { return (activeInputs & (1u << bit)) != 0; };
    return ShaderRecompiler::ShaderPixelStageInfo{
        .interpolatorCount = inputNum,
        .interpolatorSettings = interpolatorSettings,
        .wave32 = (inControl & 0x8000u) != 0,
        .inputAddr = addr,
        .hasPerspectiveCenterVgpr = loaded(1),
        .perspectiveCentroid = loaded(2),
        .posX = loaded(8),
        .posY = loaded(9),
        .posZ = loaded(10),
        .posW = loaded(11),
        .frontFace = loaded(12),
        .ancillary = loaded(13),
        .sampleShading = loaded(0) && loaded(4),
        .noPerspective = loaded(5),
        .linearCentroid = loaded(6),
        .pixelKillEnable = pixelKillEnable,
        .depthExportEnable = depthExportEnable,
        .sampleMaskExportEnable = sampleMaskExportEnable,
        .earlyZ = zOrder == 1u && !pixelKillEnable && !depthExportEnable && !sampleMaskExportEnable,
        .executeOnNoop = ((shaderControl >> 10u) & 0x1u) != 0,
        // PRIMITIVE_ORDERED_PIXEL_SHADER: the body runs under fragment shader interlock (e1d4e699).
        .orderedPixelShader = ((shaderControl >> 16u) & 0x1u) != 0,
        .targetOutputMode = targetOutputMode,
        .targetExportMapping = colorComponentMappings,
        // Packed 10_11_11 targets are not supported here; Driver sets dualSourceBlend from the
        // draw state.
        .targetExportPacking = {},
        .dualSourceBlend = false
    };
}

// userSgprBase: the SGPR where user data starts. Merged stages (ES+GS, LS+HS) begin with eight
// system SGPRs; the header's direct-resource offsets count from the first user SGPR.
PreparedVertexHeader PrepareVertexHeader(std::span<const std::byte> header, std::uint64_t headerAddress, std::uint32_t userSgprBase) {
    if (header.size() < sizeof(Shader)) throw std::runtime_error("AGC graphics: shader header is smaller than the fixed AGC header");
    Shader shader;
    std::memcpy(&shader, header.data(), sizeof(Shader));
    PreparedVertexHeader prepared;
    prepared.shaderType = shader.type;
    if (shader.user_data == nullptr) throw std::runtime_error("AGC graphics: missing AGC user-data header");
    constexpr auto userDataBytes = offsetof(ShaderUserData, sharp_resource_count) + sizeof(ShaderUserData{}.sharp_resource_count);
    const auto userDataHeader = _readHeaderPod<ShaderUserData>(header, headerAddress, shader.user_data, userDataBytes);
    if (userDataHeader.direct_resource_count > ShaderRegs::AGC_DIRECT_RESOURCE_TYPE_COUNT) throw std::runtime_error("AGC graphics: AGC direct-resource count exceeds the known resource domain");
    std::array<std::uint16_t, ShaderRegs::AGC_DIRECT_RESOURCE_TYPE_COUNT> directOffsets{};
    directOffsets.fill(ShaderRegs::AGC_ILLEGAL_DIRECT_OFFSET);
    if (userDataHeader.direct_resource_count != 0) _readHeaderArray(header, headerAddress, userDataHeader.direct_resource_offset, userDataHeader.direct_resource_count, directOffsets.data());
    for (std::uint32_t type = 0; type < userDataHeader.direct_resource_count; ++type) {
        const auto reg = directOffsets[type];
        if (reg == ShaderRegs::AGC_ILLEGAL_DIRECT_OFFSET) continue;
        if (type == static_cast<std::uint32_t>(ShaderRegs::AgcDirectResourceType::PtrVertexBufferTable)) prepared.bufferReg = static_cast<std::int32_t>(reg + userSgprBase);
        if (type == static_cast<std::uint32_t>(ShaderRegs::AgcDirectResourceType::PtrVertexAttribDescTable)) prepared.attribReg = static_cast<std::int32_t>(reg + userSgprBase);
    }
    if (prepared.attribReg < 0) return prepared;
    if (prepared.bufferReg < 0) throw std::runtime_error("AGC graphics: vertex attribute table requires a vertex buffer table");
    if (shader.num_input_semantics == 0 || shader.num_input_semantics > ShaderRecompiler::ShaderVertexStageInfo::MaxResources) throw std::runtime_error("AGC graphics: vertex semantic count is outside the supported domain");
    if (shader.input_semantics == nullptr) throw std::runtime_error("AGC graphics: missing vertex input semantics");
    std::array<ShaderSemantic, ShaderRecompiler::ShaderVertexStageInfo::MaxResources> semantics{};
    _readHeaderArray(header, headerAddress, shader.input_semantics, shader.num_input_semantics, semantics.data());
    for (std::uint32_t i = 0; i < shader.num_input_semantics; ++i) {
        const auto& semantic = semantics[i];
        if (semantic.static_vb_index == 1 || semantic.static_attribute == 1) throw std::runtime_error("AGC graphics: statically bound vertex attributes are not implemented");
        prepared.semantics.push_back({static_cast<std::uint32_t>(semantic.semantic), static_cast<std::int32_t>(semantic.hardware_mapping), static_cast<std::int32_t>(semantic.size_in_elements)});
    }
    prepared.fetch = true;
    return prepared;
}

ShaderRecompiler::ShaderVertexStageInfo DecodeVertexStageInfo(const PreparedVertexHeader& prepared, std::span<const std::uint32_t> userData) {
    ShaderRecompiler::ShaderVertexStageInfo info{};
    if (!prepared.fetch) return info;
    const auto vertexBufferReg = static_cast<std::uint32_t>(prepared.bufferReg);
    const auto vertexAttribReg = static_cast<std::uint32_t>(prepared.attribReg);
    if (vertexBufferReg + 1u >= userData.size()) throw std::runtime_error("AGC graphics: vertex buffer table pointer exceeds the user-SGPR domain");
    if (vertexAttribReg + 1u >= userData.size()) throw std::runtime_error("AGC graphics: vertex attribute table pointer exceeds the user-SGPR domain");
    const auto attribTableAddr = static_cast<std::uint64_t>(userData[vertexAttribReg]) | (static_cast<std::uint64_t>(userData[vertexAttribReg + 1u]) << 32u);
    const auto bufferTableAddr = static_cast<std::uint64_t>(userData[vertexBufferReg]) | (static_cast<std::uint64_t>(userData[vertexBufferReg + 1u]) << 32u);
    if (attribTableAddr == 0) {
        std::string message = "AGC graphics: null vertex attribute table address (shader type " + std::to_string(prepared.shaderType) + ", attribute table s" + std::to_string(prepared.attribReg) + ", buffer table s" + std::to_string(prepared.bufferReg) + ", user data";
        for (const auto value : userData) {
            char word[12];
            std::snprintf(word, sizeof(word), " %08x", value);
            message += word;
        }
        throw std::runtime_error(message + ")");
    }
    if (bufferTableAddr == 0) throw std::runtime_error("AGC graphics: null vertex buffer table address");
    info.fetchEmbedded = true;
    info.fetchAttribReg = vertexAttribReg;
    info.fetchBufferReg = vertexBufferReg;
    // The attribute words of every semantic, then the V#s they name, each table read as one range
    // (the tables are arrays indexed by semantic and by buffer index).
    std::uint32_t firstSemantic = ~0u, lastSemantic = 0;
    for (const auto& semantic : prepared.semantics) {
        firstSemantic = std::min(firstSemantic, semantic.semantic);
        lastSemantic = std::max(lastSemantic, semantic.semantic);
    }
    std::array<std::uint32_t, ShaderRecompiler::ShaderVertexStageInfo::MaxResources> attribWords{};
    {
        std::vector<std::uint32_t> words(lastSemantic - firstSemantic + 1u);
        AgcDriver::GuestMemory::Read(attribTableAddr + static_cast<std::uint64_t>(firstSemantic) * 4u, std::as_writable_bytes(std::span(words)), 4);
        for (std::size_t i = 0; i < prepared.semantics.size(); ++i) attribWords[i] = words[prepared.semantics[i].semantic - firstSemantic];
    }
    std::uint32_t firstIndex = ~0u, lastIndex = 0;
    for (std::size_t i = 0; i < prepared.semantics.size(); ++i) {
        const auto index = attribWords[i] & 0x1fu;
        if (index >= ShaderRecompiler::ShaderVertexStageInfo::MaxResources) throw std::runtime_error("AGC graphics: vertex buffer index exceeds the supported domain");
        firstIndex = std::min(firstIndex, index);
        lastIndex = std::max(lastIndex, index);
    }
    std::vector<std::uint32_t> sharps((lastIndex - firstIndex + 1u) * 4u);
    AgcDriver::GuestMemory::Read(bufferTableAddr + static_cast<std::uint64_t>(firstIndex) * 16u, std::as_writable_bytes(std::span(sharps)), 4);
    for (std::size_t i = 0; i < prepared.semantics.size(); ++i) {
        const auto& semantic = prepared.semantics[i];
        const auto attribWord = attribWords[i];
        const auto index = attribWord & 0x1fu;
        const auto format = (attribWord >> 5u) & 0x1ffu;
        const auto offset = (attribWord >> 14u) & 0xfffu;
        const auto fetchIndex = (attribWord >> 26u) & 0x1u;
        std::array<std::uint32_t, 4> sharp{};
        std::copy_n(sharps.begin() + (index - firstIndex) * 4u, 4, sharp.begin());
        if (info.resourcesNum >= ShaderRecompiler::ShaderVertexStageInfo::MaxResources) throw std::runtime_error("AGC graphics: vertex resource count exceeds the supported domain");
        auto& resource = info.resources[info.resourcesNum];
        auto& destination = info.resourcesDst[info.resourcesNum];
        resource.fields = sharp;
        destination.registerStart = semantic.hardwareMapping;
        destination.registersNum = semantic.sizeInElements;
        destination.attrId = static_cast<std::int32_t>(semantic.semantic);
        destination.fetchIndex = fetchIndex;
        if (format != 0u) {
            const auto bufferFormat = format >> 2u;
            const auto channels = (format & 0x3u) + 1u;
            const auto dstSelY = channels > 1u ? 5u : 0u;
            const auto dstSelZ = channels > 2u ? 6u : 0u;
            const auto dstSelW = channels > 3u ? 7u : 1u;
            const auto dstSel = 4u | (dstSelY << 3u) | (dstSelZ << 6u) | (dstSelW << 9u);
            resource.fields[3] = (resource.fields[3] & ~((0x7fu << 12u) | 0xfffu)) | (bufferFormat << 12u) | dstSel;
        }
        if (offset != 0u) {
            const auto base = ((static_cast<std::uint64_t>(resource.fields[0]) | (static_cast<std::uint64_t>(resource.fields[1]) << 32u)) & 0xffffffffffffull) + offset;
            resource.fields[0] = static_cast<std::uint32_t>(base & 0xffffffffu);
            resource.fields[1] = (resource.fields[1] & 0xffff0000u) | static_cast<std::uint32_t>((base >> 32u) & 0xffffu);
        }
        ++info.resourcesNum;
    }
    return info;
}

ShaderRecompiler::ShaderVertexStageInfo DecodeVertexStageInfo(std::span<const std::byte> header, std::uint64_t headerAddress, std::span<const std::uint32_t> userData, std::uint32_t userSgprBase) {
    return DecodeVertexStageInfo(PrepareVertexHeader(header, headerAddress, userSgprBase), userData);
}

}
