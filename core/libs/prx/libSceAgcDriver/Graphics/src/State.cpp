#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/General.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <sstream>

namespace AgcDriver::Graphics {
namespace {

std::uint32_t read(const Registers& registers, std::uint32_t offset, const char* bank = "context") {
    const auto it = registers.find(offset);
    if (it == registers.end()) {
        std::ostringstream message;
        message << "missing register in " << bank << " bank at DWORD 0x" << std::hex << offset << " (" << std::dec << offset << ')';
        throw std::runtime_error("AGC graphics: " + message.str());
    }
    return it->second;
}

float readFloat(const Registers& registers, std::uint32_t offset) {
    const auto value = std::bit_cast<float>(read(registers, offset));
    Require(std::isfinite(value), "non-finite register at DWORD " + std::to_string(offset));
    return value;
}

void zero(const Registers& registers, std::uint32_t offset, std::uint32_t mask, const char* name, const char* bank = "context") {
    const auto value = read(registers, offset, bank);
    if ((value & mask) == 0) return;
    std::ostringstream message;
    message << "AGC graphics: " << name << " is unsupported (" << bank << " register 0x" << std::hex << offset << " = 0x" << value << ")";
    throw std::runtime_error(message.str());
}

VkBlendFactor blendFactor(std::uint32_t value) {
    switch (value) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 2: return VK_BLEND_FACTOR_SRC_COLOR;
        case 3: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 6: return VK_BLEND_FACTOR_DST_ALPHA;
        case 7: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 8: return VK_BLEND_FACTOR_DST_COLOR;
        case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 13: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 14: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 19: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 20: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: throw std::runtime_error("AGC graphics: unsupported blend factor " + std::to_string(value));
    }
}

VkBlendOp blendOp(std::uint32_t value) {
    switch (value) {
        case 0: return VK_BLEND_OP_ADD;
        case 1: return VK_BLEND_OP_SUBTRACT;
        case 2: return VK_BLEND_OP_MIN;
        case 3: return VK_BLEND_OP_MAX;
        case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: throw std::runtime_error("AGC graphics: unsupported blend operation " + std::to_string(value));
    }
}

void intersect(VkRect2D& result, const Registers& registers, std::uint32_t offset, bool screen) {
    const auto tl = read(registers, offset);
    const auto br = read(registers, offset + 1);
    // WINDOW_OFFSET_DISABLE (bit 31) is irrelevant because PA_SC_WINDOW_OFFSET must be zero.
    if (!screen) Require((tl & 0x00008000u) == 0 && (br & 0x80008000u) == 0, "reserved scissor bits are set");
    const auto x = tl & 0xffffu;
    const auto y = (tl >> 16u) & (screen ? 0xffffu : 0x7fffu);
    const auto right = br & 0xffffu;
    const auto bottom = br >> 16u;
    Require(x <= right && y <= bottom, "inverted scissor rectangle");
    const auto oldRight = static_cast<std::uint32_t>(result.offset.x) + result.extent.width;
    const auto oldBottom = static_cast<std::uint32_t>(result.offset.y) + result.extent.height;
    const auto left = std::max(static_cast<std::uint32_t>(result.offset.x), x);
    const auto top = std::max(static_cast<std::uint32_t>(result.offset.y), y);
    result.offset = {static_cast<std::int32_t>(left), static_cast<std::int32_t>(top)};
    result.extent = {std::min(oldRight, right) > left ? std::min(oldRight, right) - left : 0, std::min(oldBottom, bottom) > top ? std::min(oldBottom, bottom) - top : 0};
}

std::uint64_t depthAddress(const Registers& registers, std::uint32_t low, std::uint32_t high) {
    const auto extension = read(registers, high);
    Require((extension & ~0xffu) == 0, "invalid depth address extension");
    return (static_cast<std::uint64_t>(extension) << 40u) | (static_cast<std::uint64_t>(read(registers, low)) << 8u);
}

VkStencilOp stencilOp(std::uint32_t value, std::uint32_t writeMask, std::uint32_t opValue) {
    if (writeMask == 0) return VK_STENCIL_OP_KEEP;
    switch (value) {
        case 0: return VK_STENCIL_OP_KEEP;
        case 1: return VK_STENCIL_OP_ZERO;
        case 3: return VK_STENCIL_OP_REPLACE;
        case 4: return (opValue & writeMask) == 0 ? VK_STENCIL_OP_ZERO : VK_STENCIL_OP_REPLACE;
        case 5: Require(opValue <= 1, "stencil add with an operand above one is unsupported"); return opValue == 0 ? VK_STENCIL_OP_KEEP : VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 6: Require(opValue <= 1, "stencil subtract with an operand above one is unsupported"); return opValue == 0 ? VK_STENCIL_OP_KEEP : VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 7: return VK_STENCIL_OP_INVERT;
        case 8: Require(opValue <= 1, "stencil add with an operand above one is unsupported"); return opValue == 0 ? VK_STENCIL_OP_KEEP : VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 9: Require(opValue <= 1, "stencil subtract with an operand above one is unsupported"); return opValue == 0 ? VK_STENCIL_OP_KEEP : VK_STENCIL_OP_DECREMENT_AND_WRAP;
        case 12:
            if ((writeMask & opValue) == 0) return VK_STENCIL_OP_KEEP;
            Require((writeMask & ~opValue) == 0, "stencil XOR with a partial operand is unsupported");
            return VK_STENCIL_OP_INVERT;
        default: throw std::runtime_error("AGC graphics: unsupported stencil operation " + std::to_string(value));
    }
}

// Vulkan has one reference per face for both the comparison and every replacement, so the
// PS5 test value and operation value (REPLACE_OP) must agree on every bit that matters.
VkStencilOpState stencilState(std::uint32_t compare, std::uint32_t operations, std::uint32_t refMask, bool writable) {
    const auto testValue = refMask & 0xffu;
    const auto compareMask = (refMask >> 8u) & 0xffu;
    const auto writeMask = writable ? (refMask >> 16u) & 0xffu : 0u;
    const auto opValue = refMask >> 24u;
    VkStencilOpState result{};
    result.compareOp = static_cast<VkCompareOp>(compare);
    result.compareMask = compareMask;
    result.writeMask = writeMask;
    auto reference = testValue;
    auto requiredBits = compare == 0 || compare == 7 ? 0u : compareMask;
    VkStencilOp* targets[] = {&result.failOp, &result.passOp, &result.depthFailOp};
    for (std::uint32_t i = 0; i < 3; ++i) {
        const auto operation = (operations >> (4u * i)) & 0xfu;
        *targets[i] = stencilOp(operation, writeMask, opValue);
        if (*targets[i] != VK_STENCIL_OP_REPLACE) continue;
        const auto replacement = operation == 4 ? opValue : testValue;
        Require(((reference ^ replacement) & requiredBits & writeMask) == 0, "stencil replacement value conflicts with the stencil test value");
        reference = (reference & ~writeMask) | (replacement & writeMask);
        requiredBits |= writeMask;
    }
    result.reference = reference;
    return result;
}

void decodeDepth(const Registers& cx, State& result) {
    const auto render = read(cx, 0x000);
    const auto control = read(cx, 0x200);
    const auto zInfo = read(cx, 0x010);
    const auto stencilInfo = read(cx, 0x011);
    const auto view = read(cx, 0x002);
    const bool depthFormat = (zInfo & 3u) != 0;
    const bool hasStencil = (stencilInfo & 1u) != 0;
    const bool depthActive = (control & 0xau) != 0 || (render & 0x5u) != 0;
    const bool stencilActive = hasStencil && ((control & 1u) != 0 || (render & 0xau) != 0);
    Require((control & ~0xc07007ffu) == 0, "reserved DB_DEPTH_CONTROL bits");
    if (!depthActive && !stencilActive) return;
    const auto readBase = read(cx, 0x012);
    const auto stencilReadBase = read(cx, 0x013);
    if (!depthFormat && !hasStencil && readBase == 0 && stencilReadBase == 0 && read(cx, 0x014) == 0 && read(cx, 0x015) == 0) {
        // Enabled depth state with no attachment bound has no effect on the hardware.
        return;
    }
    Require((control & 0xc0000000u) == 0, "color writes conditioned on the depth result are unsupported");
    Require((render & 0xcu) == 0 && (render & 0x1f80u) == 0, "depth/stencil copy or decompress passes are unsupported");
    Require((render & 0x2000u) == 0, "draws without pixel shader invocation are unsupported");
    Require((zInfo & 3u) != 2, "24-bit depth is not a PS5 depth format");
    Require(((zInfo >> 2u) & 3u) == 0, "multisampled depth targets are unsupported");
    Require((zInfo & 0x1000u) == 0 && (stencilInfo & 0x1000u) == 0, "partially resident depth targets are unsupported");
    Require(((zInfo >> 16u) & 0xfu) == 0 && ((view >> 26u) & 0xfu) == 0, "mipmapped depth targets are unsupported");
    Require((view & 0x7ffu) == 0 && ((view >> 11u) & 3u) == 0 && ((view >> 13u) & 0x7ffu) == 0 && (view >> 30u) == 0, "layered depth targets are unsupported");
    const auto size = read(cx, 0x007);
    DepthTarget& target = result.depth;
    target.extent = {(size & 0x3fffu) + 1u, ((size >> 16u) & 0x3fffu) + 1u};
    target.depthElementBytes = depthFormat ? ((zInfo & 3u) == 1 ? 2u : 4u) : 0u;
    target.hasStencil = hasStencil;
    target.format = hasStencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : target.depthElementBytes == 2 ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT;
    const bool depthReadOnly = (view & 0x01000000u) != 0;
    const bool stencilReadOnly = (view & 0x02000000u) != 0;
    if (depthFormat) {
        target.depthAddress = depthAddress(cx, 0x012, 0x01a);
        Require(depthReadOnly || target.depthAddress == depthAddress(cx, 0x014, 0x01c), "depth read and write bases differ");
        Require(target.depthAddress != 0 && (target.depthAddress & 0xffffu) == 0, "depth base is not 64 KiB aligned");
        const DepthTargetLayout layout(target.extent.width, target.extent.height, target.depthElementBytes);
        target.depthBytes = layout.Bytes();
        GuestMemory::CheckGpuRange(reinterpret_cast<const void*>(target.depthAddress), target.depthBytes, layout.Alignment(), true);
    }
    if (hasStencil) {
        target.stencilAddress = depthAddress(cx, 0x013, 0x01b);
        Require(stencilReadOnly || target.stencilAddress == depthAddress(cx, 0x015, 0x01d), "stencil read and write bases differ");
        Require(target.stencilAddress != 0 && (target.stencilAddress & 0xffffu) == 0, "stencil base is not 64 KiB aligned");
        const DepthTargetLayout layout(target.extent.width, target.extent.height, 1);
        target.stencilBytes = layout.Bytes();
        GuestMemory::CheckGpuRange(reinterpret_cast<const void*>(target.stencilAddress), target.stencilBytes, layout.Alignment(), true);
    }
    result.hasDepthTarget = true;
    auto& state = result.depthState;
    state.clearDepth = depthFormat && (render & 1u) != 0;
    state.clearStencil = hasStencil && (render & 2u) != 0 && !stencilReadOnly;
    state.depthClearValue = std::bit_cast<float>(read(cx, 0x00b));
    state.stencilClearValue = read(cx, 0x00a) & 0xffu;
    Require(!state.clearDepth || (state.depthClearValue >= 0 && state.depthClearValue <= 1), "depth clear value outside [0, 1]");
    state.depthTest = depthFormat && (control & 2u) != 0;
    state.depthWrite = state.depthTest && (control & 4u) != 0 && !depthReadOnly && !state.clearDepth;
    state.depthCompare = static_cast<VkCompareOp>((control >> 4u) & 7u);
    state.depthBounds = depthFormat && (control & 8u) != 0;
    if (state.depthBounds) {
        state.minDepthBounds = readFloat(cx, 0x008);
        state.maxDepthBounds = readFloat(cx, 0x009);
    }
    state.stencilTest = hasStencil && (control & 1u) != 0;
    if (state.stencilTest) {
        const bool writable = !state.clearStencil && !stencilReadOnly;
        const auto operations = read(cx, 0x10b);
        state.front = stencilState((control >> 8u) & 7u, operations & 0xfffu, read(cx, 0x10c), writable);
        state.back = (control & 0x80u) != 0 ? stencilState((control >> 20u) & 7u, (operations >> 12u) & 0xfffu, read(cx, 0x10d), writable) : state.front;
    }
}

}

ShaderStages DecodeShaderStages(const QueueState& queue) {
    const auto value = read(queue.context, 0x2d5);
    std::ostringstream prefix;
    prefix << "VGT_SHADER_STAGES_EN=0x" << std::hex << value << ": ";
    const auto validate = [&](bool condition, const char* reason) { Require(condition, prefix.str() + reason); };
    validate((value & 0xfc000000u) == 0, "reserved stage bits are set");
    validate((value & 3u) != 3u && ((value >> 3u) & 3u) != 3u && ((value >> 6u) & 3u) != 3u, "reserved LS_EN, ES_EN or VS_EN encoding");
    const auto primitive = read(queue.userConfig, 0x242, "user-config");
    const bool tessellation = primitive == 9;
    const bool geometry = (value & 0x20u) != 0;
    validate(tessellation == ((value & 4u) != 0), "Patch topology and HS_EN disagree");
    validate(!tessellation || !geometry, "combined tessellation and geometry is unsupported by the reference path");
    const auto path = tessellation ? ShaderPath::Tessellation : geometry ? ShaderPath::Geometry : ShaderPath::Vertex;
    ShaderStages result{path, value, (value & 0x00400000u) != 0 ? 32u : 64u, (read(queue.context, 0x1b6) & 0x8000u) != 0 ? 32u : 64u, {}, {}};
    if (path == ShaderPath::Vertex) {
        validate((value & 0x2000u) != 0, "legacy vertex routing without PRIMGEN_EN is unsupported");
        validate((value & ~0x02402010u) == 0, "unsupported vertex routing, scheduling or wave-ID state");
    } else if (path == ShaderPath::Tessellation) {
        validate((value & 0x00600020u) == 0, "wave32 tessellation or geometry amplification is unsupported");
        validate((value & ~0x0007ed0du) == 0 && (value & 3u) == 1u && ((value >> 3u) & 3u) == 1u, "unsupported tessellation routing");
        const auto config = read(queue.context, 0x2d6);
        const auto parameters = read(queue.context, 0x2db);
        ShaderRecompiler::TessellationConfiguration tess{(config >> 8u) & 0x3fu, (config >> 14u) & 0x3fu, parameters & 3u, (parameters >> 2u) & 3u, (parameters >> 5u) & 3u};
        validate(tess.inputControlPoints != 0 && tess.inputControlPoints <= 32 && tess.outputControlPoints != 0 && tess.outputControlPoints <= 32, "invalid tessellation control-point counts");
        validate(tess.domain == 1 && tess.partitioning == 2 && tess.outputTopology == 2, "only triangular, fractional-odd, clockwise tessellation is supported by the reference path");
        result.tessellation = tess;
    } else {
        validate((value & ~0x0047ec30u) == 0, "unsupported geometry routing, fast launch or wave-ID state");
        const auto group = read(queue.userConfig, 0x25b, "user-config");
        const auto vertices = (group >> 9u) & 0x1ffu;
        const auto primitives = group & 0x1ffu;
        const auto maxVertices = read(queue.context, 0x1ff);
        const auto verticesPerPrimitive = read(queue.context, 0x2ce);
        validate((primitive == 1 || primitive == 2 || primitive == 4 || primitive == 6) && read(queue.context, 0x29b) == 2 && verticesPerPrimitive >= 3, "unsupported geometry input or output assembly");
        const auto inputSize = primitive == 1 ? 1u : primitive == 2 ? 2u : 3u;
        validate(vertices >= inputSize && maxVertices != 0 && maxVertices <= 256 && verticesPerPrimitive <= 256, "invalid geometry subgroup output");
        const auto inputStep = primitive == 6 ? 1u : inputSize;
        const auto groupPrimitives = std::min({primitives, (vertices - inputSize) / inputStep + 1u, maxVertices / verticesPerPrimitive});
        validate(groupPrimitives != 0, "geometry subgroup contains no primitives");
        const auto resources = read(queue.shader, 0x8b, "shader");
        validate(((read(queue.shader, 0x8a, "shader") >> 29u) & 3u) == 3 && ((resources >> 16u) & 3u) == 3, "unsupported geometry VGPR allocation");
        result.mesh = ShaderRecompiler::MeshConfiguration{primitive, groupPrimitives, (groupPrimitives - 1u) * inputStep + inputSize, maxVertices, primitives * (verticesPerPrimitive - 2u), ((maxVertices + result.vertexWaveSize - 1u) / result.vertexWaveSize) * result.vertexWaveSize, ((resources >> 19u) & 0xffu) * 128u, 0};
    }
    return result;
}

State DecodeState(const QueueState& queue) {
    const auto& cx = queue.context;
    State result{};
    result.stages = DecodeShaderStages(queue);
    const auto primitive = read(queue.userConfig, 0x242, "user-config");
    switch (primitive) {
        case 1: Require(result.stages.mesh.has_value(), "point-list vertex rendering requires point-size output support"); result.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case 2: result.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case 7:
        case 17:
            Require(result.stages.path == ShaderPath::Vertex, "rect-list requires vertex routing");
            result.rectList = true;
            result.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
            break;
        case 9: result.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST; break;
        case 4: result.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        case 5: result.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
        case 6: result.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
        default: throw std::runtime_error("AGC graphics: unsupported primitive type " + std::to_string(primitive));
    }
    zero(queue.userConfig, 0x24b, ~0u, "primitive restart (GE_MULTI_PRIM_IB_RESET_EN)", "user-config");
    zero(cx, 0x207, ~0u, "clip distances, layer, viewport or auxiliary vertex exports");
    // Z_EXPORT_ENABLE (bit 0): the pixel shader writes depth (recompiled to gl_FragDepth).
    zero(cx, 0x203, ~0x00009871u, "stencil or mask export, shader coverage or ordered fragment execution");
    zero(cx, 0x2dc, ~0x0001ff00u, "alpha-to-coverage");
    zero(cx, 0x2f8, ~0u, "multisampling or coverage conversion");
    zero(cx, 0x292, ~2u, "scan conversion mode");
    // PA_SC_MODE_CNTL_1: walk order, hierarchical-Z kill, primitive discard and multi-GPU fields only
    // affect performance; in-order rasterization is a valid result of out-of-order mode, and sample
    // iteration equals pixel shading at the single sample count enforced above.
    zero(cx, 0x293, 0x80000000u, "reserved PA_SC_MODE_CNTL_1 bit");
    zero(cx, 0x80, ~0u, "window offset");
    zero(cx, 0x8d, ~0x01ff01ffu, "reserved PA_SU_HARDWARE_SCREEN_OFFSET bits");
    Require(read(cx, 0x83) == 0xffffu, "clip rectangles are unsupported");
    Require((read(cx, 0x8c) & 0xfu) == 0xau, "nonstandard triangle edge rules are unsupported");
    Require(read(cx, 0x2f9) == 0x2du, "nonstandard pixel center or vertex quantization is unsupported");
    Require(read(cx, 0x313) == 0x6000u, "conservative rasterization is unsupported");
    Require(read(cx, 0x30e) == 0xffffffffu && read(cx, 0x30f) == 0xffffffffu, "sample masks are unsupported");
    const auto viewportControl = read(cx, 0x206);
    if (viewportControl != 0x43fu) {
        std::ostringstream message;
        message << "AGC graphics: PA_CL_VTE_CNTL=0x" << std::hex << viewportControl << ": expected 0x43f for homogeneous positions and all viewport transforms; pre-divided coordinates, reciprocal W or disabled transforms are unsupported";
        throw std::runtime_error(message.str());
    }
    zero(cx, 0x204, ~0x80000u, "unsupported PA_CL_CLIP_CNTL flags");
    result.negativeOneToOne = (read(cx, 0x204) & 0x80000u) == 0;
    const auto raster = read(cx, 0x205);
    Require((raster & ~0x7u) == 0 || (raster & ~0x7u) == 0x240u, "polygon mode, depth bias, provoking vertex or nonstandard rasterization is unsupported");
    result.cullMode = ((raster & 1u) != 0 ? VK_CULL_MODE_FRONT_BIT : 0u) | ((raster & 2u) != 0 ? VK_CULL_MODE_BACK_BIT : 0u);
    if (result.rectList) result.cullMode = VK_CULL_MODE_NONE;
    result.frontFace = (raster & 4u) != 0 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    const auto targetMask = read(cx, 0x8e);
    const auto shaderMask = read(cx, 0x8f);
    if ((targetMask & ~0xfu) != 0 || (shaderMask & ~0xfu) != 0) {
        std::ostringstream message;
        message << "AGC graphics: only color target zero is supported: CB_TARGET_MASK=0x" << std::hex << targetMask << ", CB_SHADER_MASK=0x" << shaderMask;
        throw std::runtime_error(message.str());
    }
    result.hasColorTarget = targetMask != 0;
    Require(!result.hasColorTarget || shaderMask == 0xfu, "partial shader color exports are unsupported");
    // CB_COLOR_CONTROL: MODE (bits 4-6) is NORMAL, or DISABLE for draws without color targets; copy ROP.
    const auto colorControl = read(cx, 0x202);
    if (!(colorControl == 0xcc0010u || (colorControl == 0xcc0000u && !result.hasColorTarget))) {
        std::ostringstream message;
        message << "AGC graphics: CB_COLOR_CONTROL=0x" << std::hex << colorControl << ": only normal color rendering (or disabled color without targets) with copy ROP is supported";
        throw std::runtime_error(message.str());
    }
    // SPI_SHADER_Z_FORMAT: 32_R (depth only) with Z export, otherwise nothing.
    const auto depthExport = (read(cx, 0x203) & 1u) != 0;
    zero(cx, 0x1c4, depthExport ? ~1u : ~0u, "stencil or sample-mask export, or a depth export format other than 32_R");
    Require(!depthExport || read(cx, 0x1c4) == 1u, "depth export without the 32_R export format");
    const auto exportFormat = read(cx, 0x1c5);
    // SPI_SHADER_ZERO discards color exports, as a draw without color attachments does.
    if (!(exportFormat == 4 || exportFormat == 9 || (exportFormat == 0 && !result.hasColorTarget))) {
        std::ostringstream message;
        message << "AGC graphics: SPI_SHADER_COL_FORMAT=0x" << std::hex << exportFormat << ": only FP16_ABGR or 32_ABGR color export, or no export without a color target, is supported";
        throw std::runtime_error(message.str());
    }
    Require(read(cx, 0x1c3) == 4, "additional position exports are unsupported");
    if (result.hasColorTarget) {
        const auto info = read(cx, 0x31c);
        const auto number = (info >> 8u) & 7u;
        const auto swap = (info >> 11u) & 3u;
        Require(((info >> 2u) & 0x1fu) == 10 && (number == 0 || number == 6) && swap <= 1, "unsupported color format or component order");
        Require((info & ~0x00029f7cu) == 0, "color compression, DCC, endian conversion, nonstandard rounding or color optimization is unsupported");
        Require((info & 0x8000u) != 0, "unclamped normalized color is unsupported");
        zero(cx, 0x31b, ~0u, "color mip or array view");
        zero(cx, 0x31d, ~0u, "color samples, fragments or destination alpha override");
        const auto attrib2 = read(cx, 0x3b0);
        Require((attrib2 >> 28u) == 0, "mipmapped render targets are unsupported");
        const auto attrib3 = read(cx, 0x3b8);
        result.color.tileMode = DecodeColorTileMode(attrib3);
        result.color.extent = {((attrib2 >> 14u) & 0x3fffu) + 1u, (attrib2 & 0x3fffu) + 1u};
        const ColorTargetLayout colorLayout(result.color.extent.width, result.color.extent.height, result.color.tileMode);
        const auto high = read(cx, 0x390);
        Require((high & ~0xffu) == 0, "invalid color address extension");
        result.color.address = (static_cast<std::uint64_t>(high) << 40u) | (static_cast<std::uint64_t>(read(cx, 0x318)) << 8u);
        result.color.bytes = colorLayout.Bytes();
        GuestMemory::CheckGpuRange(reinterpret_cast<const void*>(result.color.address), result.color.bytes, colorLayout.Alignment(), true);
        result.color.format = swap == 0 ? (number == 0 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB) : (number == 0 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_B8G8R8A8_SRGB);
        result.color.componentMapping = 0xe4u;
        result.renderExtent = result.color.extent;
    }
    decodeDepth(cx, result);
    const auto psLow = queue.shader.find(0x8);
    const auto psHigh = queue.shader.find(0x9);
    result.hasFragmentShader = (psLow != queue.shader.end() && psLow->second != 0) || (psHigh != queue.shader.end() && psHigh->second != 0);
    Require(result.hasFragmentShader || !result.hasColorTarget, "color writes without a pixel shader");
    if (result.hasColorTarget) {
        if (result.hasDepthTarget) result.renderExtent = {std::min(result.color.extent.width, result.depth.extent.width), std::min(result.color.extent.height, result.depth.extent.height)};
    } else if (result.hasDepthTarget) {
        result.renderExtent = result.depth.extent;
    } else {
        const auto screenBottomRight = read(cx, 0xd);
        result.renderExtent = {screenBottomRight & 0xffffu, screenBottomRight >> 16u};
        Require(result.renderExtent.width != 0 && result.renderExtent.height != 0, "empty framebuffer extent for a draw without color writes");
    }
    const auto xs = readFloat(cx, 0x10f);
    const auto xo = readFloat(cx, 0x110);
    const auto ys = readFloat(cx, 0x111);
    const auto yo = readFloat(cx, 0x112);
    const auto zs = readFloat(cx, 0x113);
    const auto zo = readFloat(cx, 0x114);
    const auto minDepth = result.negativeOneToOne ? zo - zs : zo;
    const auto maxDepth = zo + zs;
    if (!(xs > 0 && ys != 0 && std::isfinite(minDepth) && std::isfinite(maxDepth))) {
        std::ostringstream message;
        message << "AGC graphics: unsupported viewport transform: scale=(" << xs << ", " << ys << ", " << zs << "), offset=(" << xo << ", " << yo << ", " << zo << "), depth=(" << minDepth << ", " << maxDepth << "), negativeOneToOne=" << result.negativeOneToOne;
        throw std::runtime_error(message.str());
    }
    Require(readFloat(cx, 0xb4) <= readFloat(cx, 0xb5), "inverted viewport depth clamp bounds");
    result.viewport = {xo - xs, yo - ys, 2 * xs, 2 * ys, minDepth, maxDepth};
    const auto& depthState = result.depthState;
    if (!result.hasDepthTarget || (!depthState.depthTest && !depthState.depthWrite && !depthState.depthBounds)) {
        // Window depth reaches no attachment and clipping happens in clip space, so the range
        // only has to be valid for the host API.
        result.viewport.minDepth = std::clamp(result.viewport.minDepth, 0.0f, 1.0f);
        result.viewport.maxDepth = std::clamp(result.viewport.maxDepth, 0.0f, 1.0f);
    }
    result.scissor = {{0, 0}, result.renderExtent};
    intersect(result.scissor, cx, 0xc, true);
    intersect(result.scissor, cx, 0x81, false);
    intersect(result.scissor, cx, 0x90, false);
    if ((read(cx, 0x292) & 2u) != 0) intersect(result.scissor, cx, 0x94, false);
    if (result.hasColorTarget) {
        const auto blend = read(cx, 0x1e0);
        Require((blend & 0x0000e000u) == 0, "reserved blend control bits");
        result.blend.colorWriteMask = targetMask;
        result.blend.blendEnable = (blend >> 30u) & 1u;
        if (result.blend.blendEnable) {
            Require((read(cx, 0x31c) & 0x10000u) == 0, "blend bypass conflicts with enabled blending");
            result.blend.srcColorBlendFactor = blendFactor(blend & 0x1fu);
            result.blend.dstColorBlendFactor = blendFactor((blend >> 8u) & 0x1fu);
            result.blend.colorBlendOp = blendOp((blend >> 5u) & 7u);
            const auto alpha = (blend & 0x20000000u) != 0 ? blend >> 16u : blend;
            result.blend.srcAlphaBlendFactor = blendFactor(alpha & 0x1fu);
            result.blend.dstAlphaBlendFactor = blendFactor((alpha >> 8u) & 0x1fu);
            result.blend.alphaBlendOp = blendOp((alpha >> 5u) & 7u);
            for (std::uint32_t i = 0; i < 4; ++i) result.blendConstants[i] = readFloat(cx, 0x105 + i);
        }
    }
    return result;
}

}
