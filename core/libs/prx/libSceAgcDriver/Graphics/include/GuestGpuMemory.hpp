#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTGPUMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTGPUMEMORY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

class Buffer;

// Guest memory as the GPU sees it, without copies: the host memory behind guest mappings (the
// segments of GuestMemoryBacking, through their always-writable aliases) is imported into Vulkan
// with VK_EXT_external_memory_host, so shaders read and write the same pages as the CPU. Imports are
// made on demand in fixed chunks (drivers pin imported pages) and kept while their segment lives.
class GuestGpuMemory {
public:
    struct View {
        VkBuffer buffer;
        VkDeviceSize offset;
        VkDeviceAddress address;
        // Bytes of the requested range the view covers; less when the guest range continues in
        // another segment (or not in guest memory at all).
        std::uint64_t bytes;
    };

    // Returns null when the device cannot import host memory.
    static std::unique_ptr<GuestGpuMemory> Create(const Context& context);
    ~GuestGpuMemory();
    GuestGpuMemory(const GuestGpuMemory&) = delete;
    GuestGpuMemory& operator=(const GuestGpuMemory&) = delete;

    // The GPU view of guest memory starting at address, or nothing when it is not segment memory.
    std::optional<View> Resolve(std::uint64_t address, std::uint64_t bytes);
    // A view for GPU reads only, in device-local memory when the mirror applies: pages written since
    // they were last copied (by the CPU, the GPU through Resolve's view, or the driver) are copied
    // from guest memory first, recorded into commands in queue order. Falls back to Resolve's view.
    // The GPU must not write through it.
    std::optional<View> ResolveRead(std::uint64_t address, std::uint64_t bytes, VkCommandBuffer commands);
    // Whether ResolveRead may record copies (ANYPS5_GPU_MIRROR); otherwise it is Resolve.
    bool Mirrors() const { return mirrorEnabled; }
    // Releases the imports of segments that no longer exist; the GPU must not be using them.
    void Collect();

private:
    struct Import {
        std::uint64_t end;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
        // The device-local mirror (made on first read) and which of its pages are current.
        std::shared_ptr<Buffer> mirror;
        std::vector<bool> current;
    };
    // Imports by (segment, first segment offset).
    using Key = std::pair<std::uint64_t, std::uint64_t>;

    GuestGpuMemory(const Context& context, std::uint32_t memoryTypeMask);
    static void invalidateWrites(void* self, std::uint64_t address, std::uint64_t bytes);
    void invalidate(std::uint64_t address, std::uint64_t bytes);
    const Import& importRange(std::uint64_t segment, void* alias, std::uint64_t segmentBytes, std::uint64_t first, std::uint64_t last);
    void release(Import& import) noexcept;

    Context context;
    std::uint32_t memoryTypeMask;
    std::mutex mutex;
    std::map<Key, Import> imports;
    std::vector<std::pair<std::uint64_t, Import>> retired;
    // Device-local bytes the mirrors may use; past it reads use guest memory in place.
    std::uint64_t mirrorBudget = 0;
    std::uint64_t mirrorBytes = 0;
    bool mirrorEnabled = false;
};

}

#endif
