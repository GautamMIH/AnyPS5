#include "prx/libSceAgcDriver/Graphics/include/GuestGpuMemory.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace AgcDriver::Graphics {
namespace {

// Imports cover whole chunks of a segment so neighbouring ranges share them; a range crossing chunk
// boundaries gets one import spanning them (imports may overlap).
constexpr std::uint64_t kChunkBytes = 32ull << 20;

}

std::unique_ptr<GuestGpuMemory> GuestGpuMemory::Create(const Context& context) {
    if (!context.externalMemoryHost || !context.bufferDeviceAddress) return nullptr;
    // Debug aid: ANYPS5_GPU_MEMORY_COPY=1 keeps the per-draw copies instead.
    if (std::getenv("ANYPS5_GPU_MEMORY_COPY") != nullptr) return nullptr;
    std::fprintf(stderr, "[AnyPS5] guest memory is shared with the GPU (host memory import)\n");
    return std::unique_ptr<GuestGpuMemory>(new GuestGpuMemory(context, 0));
}

GuestGpuMemory::GuestGpuMemory(const Context& context, std::uint32_t memoryTypeMask) : context(context), memoryTypeMask(memoryTypeMask) {}

GuestGpuMemory::~GuestGpuMemory() {
    for (auto& [key, import] : imports) release(import);
    for (auto& [segment, import] : retired) release(import);
}

std::optional<GuestGpuMemory::View> GuestGpuMemory::Resolve(std::uint64_t address, std::uint64_t bytes) {
    GuestMemoryBacking::Translation translation{};
    if (!GuestMemoryBacking::GuestVirtualTranslate_nid_postfix(address, bytes, &translation)) return std::nullopt;
    const auto begin = translation.offset;
    const auto end = translation.offset + translation.bytes;
    std::lock_guard lock(mutex);
    const auto first = begin & ~(kChunkBytes - 1u);
    const auto last = std::min((end + kChunkBytes - 1u) & ~(kChunkBytes - 1u), translation.segmentBytes);
    const auto& import = importRange(translation.segment, translation.segmentAlias, translation.segmentBytes, first, last);
    return View{import.buffer, begin - first, import.address + (begin - first), translation.bytes};
}

const GuestGpuMemory::Import& GuestGpuMemory::importRange(std::uint64_t segment, void* alias, std::uint64_t segmentBytes, std::uint64_t first, std::uint64_t last) {
    const auto found = imports.find({segment, first});
    if (found != imports.end() && found->second.end >= last) return found->second;
    // A longer range from the same chunk replaces the key's import; the shorter one stays alive, as
    // in-flight GPU work may use it, until its segment is gone.
    if (found != imports.end()) {
        retired.emplace_back(segment, found->second);
        imports.erase(found);
    }
    Import import;
    import.end = last;
    auto* pointer = static_cast<std::byte*>(alias) + first;
    const auto bytes = last - first;
    try {
        VkMemoryHostPointerPropertiesEXT properties{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        Check(context.Function<PFN_vkGetMemoryHostPointerPropertiesEXT>("vkGetMemoryHostPointerPropertiesEXT")(context.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, pointer, &properties), "vkGetMemoryHostPointerPropertiesEXT");
        VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &bufferInfo, nullptr, &import.buffer), "vkCreateBuffer guest import");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, import.buffer, &requirements);
        VkImportMemoryHostPointerInfoEXT host{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        host.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        host.pHostPointer = pointer;
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &host};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
        allocate.allocationSize = bytes;
        // Coherent memory needs no flushes between CPU writes and GPU reads.
        allocate.memoryTypeIndex = context.MemoryType(properties.memoryTypeBits & requirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocate, nullptr, &import.memory), "vkAllocateMemory guest import");
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, import.buffer, import.memory, 0), "vkBindBufferMemory guest import");
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = import.buffer;
        import.address = context.Function<PFN_vkGetBufferDeviceAddressKHR>("vkGetBufferDeviceAddressKHR")(context.device, &addressInfo);
    } catch (...) {
        release(import);
        throw;
    }
    return imports.insert_or_assign({segment, first}, import).first->second;
}

void GuestGpuMemory::Collect() {
    std::lock_guard lock(mutex);
    for (auto it = imports.begin(); it != imports.end();) {
        if (GuestMemoryBacking::GuestSegmentAlive_nid_postfix(it->first.first)) {
            ++it;
            continue;
        }
        release(it->second);
        it = imports.erase(it);
    }
    std::erase_if(retired, [this](auto& entry) {
        if (GuestMemoryBacking::GuestSegmentAlive_nid_postfix(entry.first)) return false;
        release(entry.second);
        return true;
    });
}

void GuestGpuMemory::release(Import& import) noexcept {
    if (import.buffer != VK_NULL_HANDLE) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, import.buffer, nullptr);
    if (import.memory != VK_NULL_HANDLE) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, import.memory, nullptr);
    import.buffer = VK_NULL_HANDLE;
    import.memory = VK_NULL_HANDLE;
}

}
