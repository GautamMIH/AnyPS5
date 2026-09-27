#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTGPUMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTGPUMEMORY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

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
    // Releases the imports of segments that no longer exist; the GPU must not be using them.
    void Collect();

private:
    struct Import {
        std::uint64_t end;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
    };
    // Imports by (segment, first segment offset).
    using Key = std::pair<std::uint64_t, std::uint64_t>;

    GuestGpuMemory(const Context& context, std::uint32_t memoryTypeMask);
    const Import& importRange(std::uint64_t segment, void* alias, std::uint64_t segmentBytes, std::uint64_t first, std::uint64_t last);
    void release(Import& import) noexcept;

    Context context;
    std::uint32_t memoryTypeMask;
    std::mutex mutex;
    std::map<Key, Import> imports;
    std::vector<std::pair<std::uint64_t, Import>> retired;
};

}

#endif
