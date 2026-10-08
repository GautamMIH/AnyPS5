#include <algorithm>
#include "prx/libSceAgcDriver/Graphics/include/GuestGpuMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastClear.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetTransfer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GpuColorTransfer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RenderCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GraphicsPipelineCache.hpp"
#include <cstring>
#include <limits>
#include <memory>

namespace AgcDriver::Graphics {
namespace {

struct DrawStorage {
    std::unique_ptr<Buffer> indices;
    std::vector<std::unique_ptr<Buffer>> vertices;
    std::array<std::shared_ptr<ResidentColor>, MaxColorTargets> colors;
    std::shared_ptr<ResidentDepth> depth;
    std::shared_ptr<Pipeline> pipeline;
};

}

std::array<std::uint32_t, 4> MeshIndexBufferDescriptor(const Pm4::DrawParameters& draw) {
    // Raw buffer: 32-bit format, stride 0, no swizzle (as the SDK's index V#s).
    constexpr std::uint32_t RawWord3 = 0x31016facu;
    // A non-indexed draw never loads through it: a null V# binds the empty buffer (upstream 5822c26b;
    // a range at the program's code address overlapped the program's own snapshot).
    if (!draw.indexed) return {0u, 0u, 0u, RawWord3};
    const auto address = draw.indexAddress;
    const auto bytes = (static_cast<std::uint64_t>(draw.indexCount) * draw.indexSize + 3u) & ~std::uint64_t{3};
    Require(address != 0 && bytes != 0 && bytes <= 0xffffffffu && (address >> 48u) == 0, "invalid mesh index buffer range");
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) & 0xffffu, static_cast<std::uint32_t>(bytes), RawWord3};
}

void Draw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots) {
    PerformanceTimer timing("Graphics.Draw");
    ApplyFastClears(state);
    const bool depthFastClear = TakeDepthFastClear(state);
    // With surfaces kept uncompressed, resolving the fast clear is all an elimination pass does.
    if (state.eliminateFastClear) return;
    Require(draw.indexed ? draw.flags == 0 : (draw.flags & ~0x20u) == 0, "draw modifiers are unsupported");
    if (draw.indexed) {
        Require(draw.indexSize == 2 || draw.indexSize == 4, "only uint16 and uint32 index buffers are supported");
        // firstVertex is the signed base vertex added to every index.
        Require(draw.firstInstance <= std::numeric_limits<std::uint32_t>::max() - (draw.instanceCount - 1u), "indexed draw instance range overflow");
    } else {
        Require(draw.indexAddress == 0 && draw.indexSize == 0, "auto draw must not reference an index buffer");
        if (draw.indexCount == 0 || draw.instanceCount == 0) return;
        Require(draw.firstVertex <= std::numeric_limits<std::uint32_t>::max() - (draw.indexCount - 1u), "auto draw vertex range overflow");
        Require(draw.firstInstance <= std::numeric_limits<std::uint32_t>::max() - (draw.instanceCount - 1u), "auto draw instance range overflow");
    }
    Require(draw.indexCount != 0 && draw.instanceCount != 0, "zero-count indexed draws are unsupported");
    const auto indexBytes = static_cast<std::uint64_t>(draw.indexCount) * draw.indexSize;
    Require(indexBytes <= std::numeric_limits<std::size_t>::max(), "index buffer size overflow");
    if (draw.indexed) GuestMemory::CheckRange(reinterpret_cast<const void*>(draw.indexAddress), static_cast<std::size_t>(indexBytes), draw.indexSize);
    std::vector<ColorTarget> colorTargets;
    for (std::uint32_t slot = 0; slot < MaxColorTargets; ++slot) {
        if ((state.colorTargetMask & (1u << slot)) != 0) colorTargets.push_back(state.colors[slot]);
    }
    const auto aliasesTargets = [&](std::uint64_t address, std::uint64_t bytes) {
        const auto overlaps = [&](std::uint64_t base, std::uint64_t size) { return size != 0 && address < base + size && base < address + bytes; };
        for (const auto& color : colorTargets) {
            if (overlaps(color.address, color.bytes)) return true;
        }
        return (state.hasDepthTarget && (overlaps(state.depth.depthAddress, state.depth.depthBytes) || overlaps(state.depth.stencilAddress, state.depth.stencilBytes)));
    };
    Require(!draw.indexed || !aliasesTargets(draw.indexAddress, indexBytes), "index buffer aliases a render target");
    if (state.rectList) Require(draw.indexCount % 3 == 0, "incomplete rect-list primitive");
    ValidateShaders(shaders, state, context.subgroup, context.fragmentShaderBarycentric, ShaderDeviceFeatures::Of(context));
    const auto shaderStages = PipelineStages(shaders);
    std::uint32_t meshGroups = 0;
    if (state.stages.mesh) {
        // Auto draws pass their offsets in the mesh draw parameters; indexed ones have none.
        Require(!draw.indexed || (draw.firstVertex == 0 && draw.firstInstance == 0), "indexed mesh draw offsets are unsupported");
        Require(context.meshShader, "device does not support mesh shaders");
        const auto& mesh = *state.stages.mesh;
        const auto inputSize = mesh.inputPrimitive == 1 ? 1u : mesh.inputPrimitive == 2 ? 2u : 3u;
        Require(draw.indexCount >= inputSize && mesh.primitivesPerGroup != 0, "mesh draw contains no complete primitive");
        const auto step = mesh.inputPrimitive == 6 ? 1u : inputSize;
        const auto primitives = (draw.indexCount - inputSize) / step + 1u;
        meshGroups = (primitives - 1u) / mesh.primitivesPerGroup + 1u;
        Require(meshGroups <= context.meshLimits.maxMeshWorkGroupCount[0] && draw.instanceCount <= context.meshLimits.maxMeshWorkGroupCount[1] && static_cast<std::uint64_t>(meshGroups) * draw.instanceCount <= context.meshLimits.maxMeshWorkGroupTotalCount, "mesh draw exceeds workgroup count limits");
    }
    if (state.stages.tessellation) Require(draw.indexCount % state.stages.tessellation->inputControlPoints == 0, "incomplete tessellation patch");
    timing.Mark("validate");
    Require(context.renderCache != nullptr && context.drawQueue != nullptr && context.graphicsPipelines != nullptr, "device graphics execution caches are unavailable");
    auto storage = std::make_shared<DrawStorage>();
    auto& indices = storage->indices;
    std::uint32_t maxIndex = draw.indexed ? 0u : draw.firstVertex + draw.indexCount - 1u;
    VkBuffer indexHandle = VK_NULL_HANDLE;
    VkDeviceSize indexOffset = 0;
    if (draw.indexed) {
        // The indices are scanned where they are (the range check above made pending writes land).
        // With imported guest memory the GPU reads them in place too, as it does vertices; otherwise
        // they are copied. ANYPS5_COPY_INDICES=1 always copies.
        static const bool copyIndices = std::getenv("ANYPS5_COPY_INDICES") != nullptr;
        std::span<const std::byte> source;
        if (!copyIndices && context.guestGpuMemory != nullptr) {
            {
                const GuestMemory::GpuAccessScope gpuAccess;
                GuestMemory::CheckRange(reinterpret_cast<const void*>(draw.indexAddress), static_cast<std::size_t>(indexBytes), draw.indexSize);
            }
            const auto view = context.guestGpuMemory->Mirrors() ? context.guestGpuMemory->ResolveRead(draw.indexAddress, indexBytes, context.drawQueue->Begin(context)) : context.guestGpuMemory->Resolve(draw.indexAddress, indexBytes);
            if (view && view->bytes >= indexBytes && view->offset % draw.indexSize == 0) {
                indexHandle = view->buffer;
                indexOffset = view->offset;
                source = std::span(reinterpret_cast<const std::byte*>(draw.indexAddress), static_cast<std::size_t>(indexBytes));
            }
        }
        if (indexHandle == VK_NULL_HANDLE) {
            indices = std::make_unique<Buffer>(context, static_cast<std::size_t>(indexBytes), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            {
                const GuestMemory::AccessSite site("cpu_wait_index");
                GuestMemory::Read(draw.indexAddress, indices->Bytes(), draw.indexSize);
            }
            indexHandle = indices->Handle();
            source = indices->Bytes();
        }
        if (draw.indexSize == 2) {
            for (std::size_t offset = 0; offset < indexBytes; offset += 2) {
                std::uint16_t value = 0;
                std::memcpy(&value, source.data() + offset, sizeof(value));
                maxIndex = std::max<std::uint32_t>(maxIndex, value);
            }
        } else {
            for (std::size_t offset = 0; offset < indexBytes; offset += 4) {
                std::uint32_t value = 0;
                std::memcpy(&value, source.data() + offset, sizeof(value));
                maxIndex = std::max(maxIndex, value);
            }
        }
        Require(maxIndex <= context.limits.maxDrawIndexedIndexValue, "index exceeds the device's indexed draw limit");
        const auto highest = static_cast<std::int64_t>(maxIndex) + static_cast<std::int32_t>(draw.firstVertex);
        Require(highest >= 0 && highest <= std::numeric_limits<std::uint32_t>::max(), "base vertex moves indices out of range");
        maxIndex = static_cast<std::uint32_t>(highest);
    }
    timing.Mark("index_upload");
    const auto& attributes = shaders.front().program->vertexAttributes;
    static_cast<void>(BuildVertexInputLayout(context, attributes));
    auto& vertexBuffers = storage->vertices;
    std::vector<VkBuffer> vertexHandles;
    std::vector<VkDeviceSize> vertexOffsets(attributes.size(), 0);
    for (std::size_t index = 0; index < attributes.size(); ++index) {
        const auto& attribute = attributes[index];
        const auto bytes = VertexBufferReadSize(attribute, maxIndex, draw.instanceCount, draw.firstInstance);
        const auto& fields = attribute.resource.fields;
        const auto address = fields[0] | (static_cast<std::uint64_t>(fields[1] & 0xffffu) << 32u);
        Require(!aliasesTargets(address, bytes), "vertex buffer aliases a render target");
        // The GPU fetches vertices from guest memory itself when it is imported: earlier draws' writes
        // then need a barrier, and nothing is copied.
        if (context.guestGpuMemory != nullptr) {
            {
                const GuestMemory::GpuAccessScope gpuAccess;
                GuestMemory::CheckRange(reinterpret_cast<const void*>(address), bytes, 1);
            }
            const auto view = context.guestGpuMemory->Mirrors() ? context.guestGpuMemory->ResolveRead(address, bytes, context.drawQueue->Begin(context)) : context.guestGpuMemory->Resolve(address, bytes);
            if (view && view->bytes >= bytes) {
                vertexHandles.push_back(view->buffer);
                vertexOffsets[index] = view->offset;
                continue;
            }
        }
        const GuestMemory::AccessSite site("cpu_wait_vertex");
        GuestMemory::CheckRange(reinterpret_cast<const void*>(address), bytes, 1);
        auto buffer = std::make_unique<Buffer>(context, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        GuestMemory::Read(address, buffer->Bytes(), 1);
        vertexHandles.push_back(buffer->Handle());
        vertexBuffers.push_back(std::move(buffer));
    }
    timing.Mark("vertex_upload");
    // The work leaves the depth plane unwritten (no depth writes, depth clears or fast clear): it may
    // sample it in its pass (TextureCache, Texture::FeedbackView).
    const bool depthReadOnly = state.hasDepthTarget && !state.depthState.depthWrite && !state.depthState.clearDepth && !depthFastClear;
    if (context.textureCache) context.textureCache->SetRenderedDepth(state.hasDepthTarget && state.depth.depthBytes != 0 ? state.depth.depthAddress : 0, depthReadOnly);
    const auto renderedCopiesBefore = context.textureCache ? context.textureCache->RenderedDepthCopies() : 0;
    auto resources = std::make_shared<ShaderResources>(context, shaders, colorTargets, draw.indexAddress, static_cast<std::size_t>(indexBytes), snapshots);
    // Debug aid: APS5_TRACE_DEPTH_FEEDBACK=1 logs the state of draws that sample their own depth target.
    static const bool traceFeedback = std::getenv("APS5_TRACE_DEPTH_FEEDBACK") != nullptr;
    if (traceFeedback && context.textureCache && context.textureCache->RenderedDepthCopies() != renderedCopiesBefore) {
        const auto& depthState = state.depthState;
        std::fprintf(stderr, "[depth-feedback] depth 0x%llx test=%d write=%d compare=%d stencil=%d stencilWrite=%x/%x clearDepth=%d clearStencil=%d colors=%x\n", static_cast<unsigned long long>(state.depth.depthAddress), depthState.depthTest, depthState.depthWrite, static_cast<int>(depthState.depthCompare), depthState.stencilTest, depthState.front.writeMask, depthState.back.writeMask, depthState.clearDepth, depthState.clearStencil, state.colorTargetMask);
    }
    if (context.textureCache) context.textureCache->SetRenderedDepth(0);
    timing.Mark("shader_resources");
    for (std::uint32_t slot = 0; slot < MaxColorTargets; ++slot) {
        if ((state.colorTargetMask & (1u << slot)) == 0) continue;
        const auto& color = state.colors[slot];
        const ColorTargetLayout colorLayout(color.extent.width, color.extent.height, color.tileMode, color.elementBytes, color.tail);
        Require(color.bytes == (color.Layered() ? color.LayeredBytes(colorLayout.Bytes()) : colorLayout.Bytes()), "color target transfer size mismatch");
        // The render cache evicts residents that overlap a new target, so the targets of one draw
        // must be disjoint.
        for (std::uint32_t other = 0; other < slot; ++other) Require(!storage->colors[other] || !storage->colors[other]->Overlaps(color), "color targets of one draw overlap");
        storage->colors[slot] = context.renderCache->Get(color, state.blends[slot].blendEnable != 0);
    }
    if (state.hasDepthTarget) storage->depth = context.renderCache->GetDepth(state.depth);
    timing.Mark("render_target_cache");
    storage->pipeline = context.graphicsPipelines->Get(state, storage->colors, storage->depth, *resources, shaders, resources->DepthFeedback());
    auto& pipeline = *storage->pipeline;
    timing.Mark("pipeline_cache");
    // Render-pass merging: a draw whose targets are resident attachments and that records nothing
    // outside a pass continues the pass the previous draw left open on the same attachments.
    // ANYPS5_NO_PASS_MERGE=1 begins a pass per draw. (The GPU draw profiler resets queries, which a
    // pass forbids: it disables merging.)
    static const bool noMerge = std::getenv("ANYPS5_NO_PASS_MERGE") != nullptr;
    bool mergeable = !noMerge && !context.drawProfiler && !resources->RecordsOutsidePass();
    for (const auto& color : storage->colors) mergeable = mergeable && (!color || !color->BeginRecords());
    mergeable = mergeable && (!storage->depth || !storage->depth->BeginRecords());
    const auto continued = mergeable ? context.drawQueue->ContinuePass(pipeline.PassKey()) : VkCommandBuffer{VK_NULL_HANDLE};
    const auto commands = continued != VK_NULL_HANDLE ? continued : context.drawQueue->Begin(context);
    std::uint32_t profile = UINT32_MAX;
    if (context.drawProfiler) {
        std::string key = "draw";
        char part[64];
        for (const auto& shader : shaders) {
            std::snprintf(part, sizeof(part), " %u:%016llx", static_cast<unsigned>(shader.stage), static_cast<unsigned long long>(shader.program->variantId));
            key += part;
        }
        std::snprintf(part, sizeof(part), " %ux%u rt%x%s", state.renderExtent.width, state.renderExtent.height, state.colorTargetMask, state.hasDepthTarget ? " depth" : "");
        key += part;
        profile = context.drawProfiler->Begin(context, commands, std::move(key));
    }
    if (continued != VK_NULL_HANDLE) {
        // Begin records nothing here (BeginRecords): it only marks the targets rendered.
        for (const auto& color : storage->colors) {
            if (color) color->Begin(commands);
        }
        if (storage->depth) storage->depth->Begin(commands);
        pipeline.Bind(commands, state);
    } else {
        // Host writes made before the batch is submitted are visible to it (submission orders them);
        // this barrier covers the transfer stages of the uploads below.
        VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        upload.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        upload.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | shaderStages, 0, 1, &upload, 0, nullptr, 0, nullptr);
        resources->RecordUploads(commands);
        for (const auto& color : storage->colors) {
            if (color) color->Begin(commands);
        }
        if (storage->depth) storage->depth->Begin(commands);
        if (context.drawProfiler) context.drawProfiler->Mark(context, commands, profile, 1);
        pipeline.Begin(commands, state);
    }
    if (depthFastClear) {
        // An HTILE fast clear stands for the whole surface holding the clear values.
        VkClearAttachment clear{};
        clear.aspectMask = (state.depth.depthElementBytes != 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) | (state.depth.hasStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
        clear.clearValue.depthStencil = {std::clamp(state.depthState.depthClearValue, 0.0f, 1.0f), state.depthState.stencilClearValue};
        const VkClearRect rect{{{0, 0}, state.renderExtent}, 0, 1};
        context.Function<PFN_vkCmdClearAttachments>("vkCmdClearAttachments")(commands, 1, &clear, 1, &rect);
    }
    if (state.hasDepthTarget && (state.depthState.clearDepth || state.depthState.clearStencil) && state.scissor.extent.width != 0 && state.scissor.extent.height != 0) {
        // DB_RENDER_CONTROL clears replace the draw's depth/stencil results with the clear values
        // wherever it rasterizes; clear draws cover the scissor rectangle.
        VkClearAttachment clear{};
        clear.aspectMask = (state.depthState.clearDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) | (state.depthState.clearStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
        clear.clearValue.depthStencil = {state.depthState.depthClearValue, state.depthState.stencilClearValue};
        const VkClearRect rect{state.scissor, 0, 1};
        context.Function<PFN_vkCmdClearAttachments>("vkCmdClearAttachments")(commands, 1, &clear, 1, &rect);
    }
    resources->Bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Layout());
    pipeline.PushConstants(commands, shaders);
    if (state.stages.mesh) {
        // Mesh programs read the draw's parameters from the end of the push block (upstream's NGG
        // translation ABI); the index buffer is a V# in the front program's user words 4-7.
        const std::array<std::uint32_t, ShaderRecompiler::MeshDrawPushBytes / sizeof(std::uint32_t)> parameters{draw.indexCount, draw.firstVertex, draw.firstInstance, draw.indexed ? draw.indexSize : 0u, 0u, 0u};
        static_assert(ShaderRecompiler::MeshDrawPushOffsetBytes + ShaderRecompiler::MeshDrawPushBytes == PipelinePushConstantBytes);
        context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, pipeline.Layout(), PushConstantStages(shaders), ShaderRecompiler::MeshDrawPushOffsetBytes, ShaderRecompiler::MeshDrawPushBytes, parameters.data());
    }
    if (state.stages.mesh) {
        context.Function<PFN_vkCmdDrawMeshTasksEXT>("vkCmdDrawMeshTasksEXT")(commands, meshGroups, draw.instanceCount, 1);
    } else {
        if (!vertexHandles.empty()) context.Function<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers")(commands, 0, static_cast<std::uint32_t>(vertexHandles.size()), vertexHandles.data(), vertexOffsets.data());
        if (draw.indexed) {
            context.Function<PFN_vkCmdBindIndexBuffer>("vkCmdBindIndexBuffer")(commands, indexHandle, indexOffset, draw.indexSize == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            context.Function<PFN_vkCmdDrawIndexed>("vkCmdDrawIndexed")(commands, draw.indexCount, draw.instanceCount, 0, static_cast<std::int32_t>(draw.firstVertex), draw.firstInstance);
        } else {
            context.Function<PFN_vkCmdDraw>("vkCmdDraw")(commands, draw.indexCount, draw.instanceCount, draw.firstVertex, draw.firstInstance);
        }
    }
    // The pass stays open for the next draw on the same attachments; the batch's end makes the
    // writes visible to the host (DrawQueue::Flush).
    context.drawQueue->KeepPassOpen(context, pipeline.PassKey());
    if (!mergeable) {
        context.drawQueue->EndPass();
        if (context.drawProfiler) context.drawProfiler->Mark(context, commands, profile, 2);
        resources->RecordDownloads(commands);
        if (context.drawProfiler) context.drawProfiler->Mark(context, commands, profile, 3);
    }
    timing.Mark("command_record");
    context.drawQueue->Enqueue(std::move(resources), std::move(storage));
    timing.Mark("enqueue");
}

}
