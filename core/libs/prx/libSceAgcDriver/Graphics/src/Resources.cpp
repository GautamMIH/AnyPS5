#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <algorithm>
#include <mutex>
#include <vector>

namespace AgcDriver::Graphics {

Buffer::Buffer(const Context& context, std::size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) : context(context), size(size), usage(usage), properties(properties) {
    Require(size != 0, "zero-sized GPU buffer");
    const bool addressable = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
    Require(!addressable || context.bufferDeviceAddress, "buffer device address is not enabled");
    cache = GetBufferPool(context);
    if (const auto allocation = cache->Take(size, usage, properties)) {
        buffer = allocation->buffer;
        memory = allocation->memory;
        mapping = allocation->mapping;
        deviceAddress = allocation->address;
        allocationBytes = allocation->allocationBytes;
        reusable = true;
        return;
    }
    try {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
        if (addressable) allocation.pNext = &flags;
        allocation.allocationSize = requirements.size;
        allocationBytes = requirements.size;
        // Host-visible buffers are also read by the CPU (write-back, readbacks). On discrete GPUs
        // the first host-visible type is usually uncached (write-combined), where CPU reads are
        // an order of magnitude slower, so a cached type is preferred.
        const auto preferred = (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0u;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, properties, preferred);
        Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory), "vkAllocateMemory buffer");
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0), "vkBindBufferMemory");
        initializeAddress(usage);
        if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) Check(context.Function<PFN_vkMapMemory>("vkMapMemory")(context.device, memory, 0, VK_WHOLE_SIZE, 0, &mapping), "vkMapMemory");
        reusable = true;
    } catch (...) {
        release();
        throw;
    }
}

Buffer::~Buffer() {
    release();
}

void Buffer::release() noexcept {
    if (reusable && buffer && memory && cache) {
        cache->Put({buffer, memory, mapping, deviceAddress, allocationBytes, size, usage, properties});
        return;
    }
    if (mapping) context.Function<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, memory);
    if (buffer) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
    if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
}

VkBuffer Buffer::Handle() const {
    return buffer;
}

std::span<std::byte> Buffer::Bytes() {
    Require(mapping != nullptr, "GPU-only buffer has no CPU mapping");
    return {static_cast<std::byte*>(mapping), size};
}

void Buffer::Invalidate() {
    Require(mapping != nullptr, "cannot invalidate an unmapped GPU buffer");
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = memory;
    range.size = VK_WHOLE_SIZE;
    Check(context.Function<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(context.device, 1, &range), "vkInvalidateMappedMemoryRanges");
}

RenderTarget::RenderTarget(const Context& context, const ColorTarget& target, bool blending) : context(context) {
    VkFormatProperties properties{};
    context.formatProperties(context.physical, target.format, &properties);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | (blending ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT : 0u);
    Require((properties.optimalTilingFeatures & required) == required, "render-target format does not support required operations");
    create(target.format, target.extent, target.bytes, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, target.Layered() ? target.layers : 1u);
}

RenderTarget::RenderTarget(const Context& context, const DepthTarget& target) : context(context) {
    VkFormatProperties properties{};
    context.formatProperties(context.physical, target.format, &properties);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    Require((properties.optimalTilingFeatures & required) == required, "depth/stencil format does not support required operations");
    const auto aspect = target.hasStencil ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    // Sampled directly by textures over the depth plane (no copy) where the format allows it.
    sampled = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    create(target.format, target.extent, target.depthBytes + target.stencilBytes, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0u), aspect);
}

void RenderTarget::create(VkFormat format, VkExtent2D extent, std::size_t bytes, VkImageUsageFlags attachment, VkImageAspectFlags aspect, std::uint32_t layers) {
    const auto usage = attachment | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageFormatProperties supported{};
    Check(context.imageFormatProperties(context.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &supported), "vkGetPhysicalDeviceImageFormatProperties");
    Require(extent.width <= supported.maxExtent.width && extent.height <= supported.maxExtent.height && (supported.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0 && bytes <= supported.maxResourceSize, "render target exceeds device image limits");
    Require(extent.width <= context.limits.maxFramebufferWidth && extent.height <= context.limits.maxFramebufferHeight, "render target exceeds framebuffer limits");
    Require(layers != 0 && layers <= supported.maxArrayLayers && layers <= context.limits.maxFramebufferLayers, "render target layers exceed device limits");
    try {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = layers;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory), "vkAllocateMemory render target");
        Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {aspect, 0, 1, 0, layers};
        Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView");
    } catch (...) {
        release();
        throw;
    }
}

RenderTarget::~RenderTarget() {
    release();
}

void RenderTarget::release() noexcept {
    if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
    if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
    if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
}

VkImage RenderTarget::Image() const {
    return image;
}

VkImageView RenderTarget::View() const {
    return view;
}

namespace {

// A destroyed batch's command buffer and fence are kept for the next batch: allocating them costs
// hundreds of microseconds on some drivers, and textures and copies create batches every frame.
struct RecycledBatch {
    VkDevice device;
    VkCommandPool pool;
    VkCommandBuffer commands;
    VkFence fence;
};
constexpr std::size_t MaxRecycledBatches = 256;
std::mutex recycledMutex;
std::vector<RecycledBatch> recycledBatches;

bool takeRecycled(VkDevice device, VkCommandPool pool, VkCommandBuffer& commands, VkFence& fence) {
    std::lock_guard lock(recycledMutex);
    for (auto it = recycledBatches.rbegin(); it != recycledBatches.rend(); ++it) {
        if (it->device != device || it->pool != pool) continue;
        commands = it->commands;
        fence = it->fence;
        recycledBatches.erase(std::next(it).base());
        return true;
    }
    return false;
}

bool keepRecycled(VkDevice device, VkCommandPool pool, VkCommandBuffer commands, VkFence fence) {
    std::lock_guard lock(recycledMutex);
    if (recycledBatches.size() >= MaxRecycledBatches) return false;
    recycledBatches.push_back({device, pool, commands, fence});
    return true;
}

}

void DropRecycledCommandBatches(VkDevice device, VkCommandPool pool, PFN_vkGetDeviceProcAddr deviceProc) {
    std::lock_guard lock(recycledMutex);
    for (auto it = recycledBatches.begin(); it != recycledBatches.end();) {
        if (it->device != device || it->pool != pool) {
            ++it;
            continue;
        }
        reinterpret_cast<PFN_vkFreeCommandBuffers>(deviceProc(device, "vkFreeCommandBuffers"))(device, pool, 1, &it->commands);
        reinterpret_cast<PFN_vkDestroyFence>(deviceProc(device, "vkDestroyFence"))(device, it->fence, nullptr);
        it = recycledBatches.erase(it);
    }
}

CommandBatch::CommandBatch(const Context& context) : context(context) {
    try {
        if (takeRecycled(context.device, context.pool, commands, fence)) {
            Check(context.Function<PFN_vkResetFences>("vkResetFences")(context.device, 1, &fence), "vkResetFences recycled batch");
            Check(context.Function<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(commands, 0), "vkResetCommandBuffer recycled batch");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            Check(context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin), "vkBeginCommandBuffer recycled batch");
            beginTiming();
            return;
        }
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = context.pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        Check(context.Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(context.device, &allocation, &commands), "vkAllocateCommandBuffers");
        VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(context.Function<PFN_vkCreateFence>("vkCreateFence")(context.device, &info, nullptr, &fence), "vkCreateFence");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin), "vkBeginCommandBuffer");
        beginTiming();
    } catch (...) {
        release();
        throw;
    }
}

CommandBatch::~CommandBatch() {
    release();
}

void CommandBatch::release() noexcept {
    if (pending) {
        auto result = context.Function<PFN_vkGetFenceStatus>("vkGetFenceStatus")(context.device, fence);
        if (result == VK_NOT_READY) result = context.Function<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(context.queue);
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) std::terminate();
    }
    reportTiming();
    // Completed (or never submitted): the command buffer and fence serve the next batch.
    if (commands && fence && keepRecycled(context.device, context.pool, commands, fence)) return;
    if (commands) context.Function<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(context.device, context.pool, 1, &commands);
    if (fence) context.Function<PFN_vkDestroyFence>("vkDestroyFence")(context.device, fence, nullptr);
}

VkCommandBuffer CommandBatch::Handle() const {
    return commands;
}

void CommandBatch::SubmitAndWait() {
    Submit();
    Wait();
}

void CommandBatch::Reset() {
    Require(submitted && !pending, "command batch must complete before reuse");
    reportTiming();
    Check(context.Function<PFN_vkResetFences>("vkResetFences")(context.device, 1, &fence), "vkResetFences graphics");
    Check(context.Function<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(commands, 0), "vkResetCommandBuffer graphics");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin), "vkBeginCommandBuffer graphics");
    submitted = false;
    beginTiming();
}

void CommandBatch::Submit() {
    PerformanceTimer timing("Graphics.Submit");
    Require(!submitted, "command batch has already been submitted");
    if (timestamp != UINT32_MAX) context.Function<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, context.gpuTimestamps->Pool(), timestamp + 1);
    Check(context.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands), "vkEndCommandBuffer");
    VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &commands;
    timing.Mark("command_end");
    Check(context.Function<PFN_vkQueueSubmit>("vkQueueSubmit")(context.queue, 1, &submission, fence), "vkQueueSubmit graphics");
    timing.Mark("queue_submit");
    pending = true;
    submitted = true;
}

void CommandBatch::Wait() {
    Require(submitted, "command batch has not been submitted");
    if (!pending) return;
    PerformanceTimer timing("Graphics.Wait");
    const auto result = context.Function<PFN_vkWaitForFences>("vkWaitForFences")(context.device, 1, &fence, VK_TRUE, 5'000'000'000ULL);
    timing.Mark("fence_wait");
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST) pending = false;
    Check(result, "vkWaitForFences graphics");
    reportTiming();
}

ImageMemory::ImageMemory(VkDevice device, PFN_vkGetDeviceProcAddr deviceProc, const VkPhysicalDeviceMemoryProperties& properties) : device(device), deviceProc(deviceProc), properties(properties) {}

ImageMemory::~ImageMemory() {
    const auto freeMemory = reinterpret_cast<PFN_vkFreeMemory>(deviceProc(device, "vkFreeMemory"));
    for (const auto& block : blocks)
        if (block.memory != VK_NULL_HANDLE) freeMemory(device, block.memory, nullptr);
}

VkDeviceMemory ImageMemory::allocate(std::uint32_t type, VkDeviceSize size) {
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = size;
    info.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    Check(reinterpret_cast<PFN_vkAllocateMemory>(deviceProc(device, "vkAllocateMemory"))(device, &info, nullptr, &memory), "vkAllocateMemory image block");
    return memory;
}

ImageMemory::Allocation ImageMemory::Allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags flags) {
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < properties.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) != 0 && (properties.memoryTypes[i].propertyFlags & flags) == flags) type = i;
    Require(type != UINT32_MAX, "required Vulkan memory type is unavailable");
    const auto size = requirements.size;
    const auto alignment = std::max<VkDeviceSize>(requirements.alignment, 1);
    if (size > BlockBytes / 4) return {allocate(type, size), 0, size, SIZE_MAX};
    std::lock_guard lock(mutex);
    const auto carve = [&](std::size_t index) -> std::optional<Allocation> {
        auto& block = blocks[index];
        for (auto it = block.free.begin(); it != block.free.end(); ++it) {
            const auto [begin, bytes] = *it;
            const auto aligned = (begin + alignment - 1) / alignment * alignment;
            if (aligned + size > begin + bytes) continue;
            const auto end = begin + bytes;
            block.free.erase(it);
            if (aligned > begin) block.free.emplace(begin, aligned - begin);
            if (aligned + size < end) block.free.emplace(aligned + size, end - aligned - size);
            block.used += size;
            return Allocation{block.memory, aligned, size, index};
        }
        return std::nullopt;
    };
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        if (blocks[index].memory == VK_NULL_HANDLE || blocks[index].type != type) continue;
        if (auto found = carve(index)) return *found;
    }
    // A new block, in a released slot when there is one.
    std::size_t index = blocks.size();
    for (std::size_t i = 0; i < blocks.size(); ++i)
        if (blocks[i].memory == VK_NULL_HANDLE) index = i;
    if (index == blocks.size()) blocks.emplace_back();
    auto& block = blocks[index];
    block.memory = allocate(type, BlockBytes);
    block.type = type;
    block.used = 0;
    block.free.clear();
    block.free.emplace(0, BlockBytes);
    return *carve(index);
}

void ImageMemory::Free(const Allocation& allocation) noexcept {
    if (allocation.memory == VK_NULL_HANDLE) return;
    const auto freeMemory = reinterpret_cast<PFN_vkFreeMemory>(deviceProc(device, "vkFreeMemory"));
    if (allocation.block == SIZE_MAX) {
        freeMemory(device, allocation.memory, nullptr);
        return;
    }
    std::lock_guard lock(mutex);
    auto& block = blocks[allocation.block];
    auto begin = allocation.offset;
    auto end = allocation.offset + allocation.size;
    auto next = block.free.lower_bound(begin);
    if (next != block.free.end() && next->first == end) {
        end += next->second;
        next = block.free.erase(next);
    }
    if (next != block.free.begin()) {
        const auto previous = std::prev(next);
        if (previous->first + previous->second == begin) {
            begin = previous->first;
            block.free.erase(previous);
        }
    }
    block.free.emplace(begin, end - begin);
    block.used -= allocation.size;
    if (block.used != 0) return;
    // An empty block is released unless it is the only one of its type (kept for the next level).
    const auto others = std::count_if(blocks.begin(), blocks.end(), [&](const Block& other) { return &other != &block && other.memory != VK_NULL_HANDLE && other.type == block.type; });
    if (others == 0) return;
    freeMemory(device, block.memory, nullptr);
    block.memory = VK_NULL_HANDLE;
    block.free.clear();
}

GpuTimestamps::GpuTimestamps(VkDevice device, PFN_vkGetDeviceProcAddr deviceProc, float period) : device(device), deviceProc(deviceProc), period(period) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = Pairs * 2;
    Check(reinterpret_cast<PFN_vkCreateQueryPool>(deviceProc(device, "vkCreateQueryPool"))(device, &info, nullptr, &pool), "vkCreateQueryPool timestamps");
}

GpuTimestamps::~GpuTimestamps() {
    if (pool != VK_NULL_HANDLE) reinterpret_cast<PFN_vkDestroyQueryPool>(deviceProc(device, "vkDestroyQueryPool"))(device, pool, nullptr);
}

std::uint32_t GpuTimestamps::Acquire() {
    return (next.fetch_add(1, std::memory_order_relaxed) % Pairs) * 2;
}

void GpuTimestamps::Report(std::uint32_t first, const char* label) {
    std::array<std::uint64_t, 2> values{};
    const auto result = reinterpret_cast<PFN_vkGetQueryPoolResults>(deviceProc(device, "vkGetQueryPoolResults"))(device, pool, first, 2, sizeof(values), values.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS || values[1] < values[0]) return;
    const auto nanoseconds = static_cast<std::uint64_t>(static_cast<double>(values[1] - values[0]) * period);
    const auto start = static_cast<std::uint64_t>(static_cast<double>(values[0]) * period);
    std::lock_guard lock(mutex);
    auto& total = totals[label];
    total.first += nanoseconds;
    ++total.second;
    if (intervals.size() < 65536) intervals.emplace_back(start, start + nanoseconds);
}

std::vector<std::tuple<const char*, std::uint64_t, std::uint64_t>> GpuTimestamps::Drain(std::uint64_t& busy) {
    std::lock_guard lock(mutex);
    std::vector<std::tuple<const char*, std::uint64_t, std::uint64_t>> result;
    for (const auto& [label, total] : totals) result.emplace_back(label, total.first, total.second);
    totals.clear();
    std::sort(intervals.begin(), intervals.end());
    busy = 0;
    std::uint64_t coveredEnd = 0;
    for (const auto& [start, end] : intervals) {
        const auto from = std::max(start, coveredEnd);
        if (end > from) busy += end - from;
        coveredEnd = std::max(coveredEnd, end);
    }
    intervals.clear();
    return result;
}

DrawProfiler::DrawProfiler(VkDevice device, PFN_vkGetDeviceProcAddr deviceProc, float period) : device(device), deviceProc(deviceProc), period(period) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = Records * Marks;
    Check(reinterpret_cast<PFN_vkCreateQueryPool>(deviceProc(device, "vkCreateQueryPool"))(device, &info, nullptr, &pool), "vkCreateQueryPool draw profile");
    if (const char* frames = std::getenv("APS5_PROFILE_GPU_FRAMES")) interval = std::max(1, std::atoi(frames));
}

DrawProfiler::~DrawProfiler() {
    if (pool != VK_NULL_HANDLE) reinterpret_cast<PFN_vkDestroyQueryPool>(deviceProc(device, "vkDestroyQueryPool"))(device, pool, nullptr);
}

std::uint32_t DrawProfiler::Begin(const Context& context, VkCommandBuffer commands, std::string key) {
    std::uint32_t record = 0;
    {
        std::lock_guard lock(mutex);
        if (pending.size() >= Records) return UINT32_MAX;
        record = next;
        next = (next + 1) % Records;
        pending.push_back({record, std::move(key)});
    }
    context.Function<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(commands, pool, record * Marks, Marks);
    context.Function<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, record * Marks);
    return record;
}

void DrawProfiler::Mark(const Context& context, VkCommandBuffer commands, std::uint32_t record, std::uint32_t mark) const {
    if (record == UINT32_MAX) return;
    context.Function<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, record * Marks + mark);
}

void DrawProfiler::EndFrame() {
    std::lock_guard lock(mutex);
    const auto results = reinterpret_cast<PFN_vkGetQueryPoolResults>(deviceProc(device, "vkGetQueryPoolResults"));
    // The queue runs in order: the first record not yet finished ends the read.
    while (!pending.empty()) {
        std::array<std::uint64_t, Marks> values{};
        if (results(device, pool, pending.front().record * Marks, Marks, sizeof(values), values.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) break;
        auto& total = totals[pending.front().key];
        for (std::uint32_t phase = 0; phase + 1 < Marks; ++phase)
            if (values[phase + 1] >= values[phase]) total.phases[phase] += static_cast<std::uint64_t>(static_cast<double>(values[phase + 1] - values[phase]) * period);
        ++total.count;
        pending.pop_front();
    }
    if (++frames < interval) return;
    std::vector<std::pair<std::string, Total>> sorted(totals.begin(), totals.end());
    const auto sum = [](const Total& total) { return total.phases[0] + total.phases[1] + total.phases[2]; };
    std::sort(sorted.begin(), sorted.end(), [&](const auto& a, const auto& b) { return sum(a.second) > sum(b.second); });
    Total all;
    for (const auto& [key, total] : sorted) {
        for (std::uint32_t phase = 0; phase + 1 < Marks; ++phase) all.phases[phase] += total.phases[phase];
        all.count += total.count;
    }
    const auto perFrame = [&](std::uint64_t nanoseconds) { return static_cast<double>(nanoseconds) / 1e6 / frames; };
    std::fprintf(stderr, "[gpu-profile] %u frames: %.1f draws+dispatches/frame, ms/frame setup %.2f work %.2f teardown %.2f\n", frames, static_cast<double>(all.count) / frames, perFrame(all.phases[0]), perFrame(all.phases[1]), perFrame(all.phases[2]));
    for (std::size_t index = 0; index < sorted.size() && index < 30; ++index) {
        const auto& [key, total] = sorted[index];
        std::fprintf(stderr, "[gpu-profile]   %7.2f ms/frame (setup %.2f work %.2f teardown %.2f) %6.1f/frame %7.1f us each  %s\n", perFrame(sum(total)), perFrame(total.phases[0]), perFrame(total.phases[1]), perFrame(total.phases[2]), static_cast<double>(total.count) / frames, static_cast<double>(sum(total)) / 1e3 / static_cast<double>(total.count), key.c_str());
    }
    totals.clear();
    frames = 0;
}

void CommandBatch::beginTiming() {
    if (!context.gpuTimestamps) return;
    timestamp = context.gpuTimestamps->Acquire();
    const auto pool = context.gpuTimestamps->Pool();
    context.Function<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(commands, pool, timestamp, 2);
    context.Function<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, timestamp);
}

void CommandBatch::reportTiming() noexcept {
    if (timestamp == UINT32_MAX) return;
    const auto first = timestamp;
    timestamp = UINT32_MAX;
    if (!submitted || pending || !context.gpuTimestamps) return;
    try {
        context.gpuTimestamps->Report(first, label);
    } catch (...) {
    }
}

}
