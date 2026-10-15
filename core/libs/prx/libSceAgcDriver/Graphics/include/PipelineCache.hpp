#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECACHE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace AgcDriver::Graphics {

// The device's VkPipelineCache, kept on disk between runs (upstream cf721a27): loaded at creation
// when the file's checksum and Vulkan header match the device (vendor, device, pipelineCacheUUID;
// the driver version is in the file name), saved every SaveIntervalSeconds while it grew and at
// teardown. The file lives in ShaderCacheDirectory(). A process can hold several caches for one file
// (the driver keeps a replaced headless device next to the windowed one), so a save merges this
// cache into the file's (upstream 8c8cd86c) instead of overwriting the other caches' pipelines.
class PipelineCache {
public:
    static constexpr int SaveIntervalSeconds = 10;

    PipelineCache(const Context& context, const VkPhysicalDeviceProperties& properties);
    ~PipelineCache();

    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;
    VkPipelineCache Handle() const { return cache; }

private:
    struct FileHeader {
        std::uint32_t magic;
        std::uint32_t format;
        std::uint64_t dataBytes;
        std::uint64_t dataHash;
        bool operator==(const FileHeader&) const = default;
    };
    enum class FileState { Missing, Unreadable, Invalid, Valid };
    struct File {
        FileState state = FileState::Missing;
        const char* invalidReason = nullptr;
        FileHeader header{};
        std::vector<std::byte> contents;
        std::span<const std::byte> Payload() const;
    };

    File readFile() const;
    std::optional<FileHeader> readHeader() const;
    void load(std::vector<std::byte>& initialData);
    void save(bool final);
    void run();

    Context context;
    VkPhysicalDeviceProperties properties{};
    VkPipelineCache cache = VK_NULL_HANDLE;
    std::filesystem::path path;
    // Bytes loaded at creation, and the live cache size and file header at the last save (or load)
    // that left the file settled: a save with neither changed does nothing.
    std::size_t loadedBytes = 0;
    std::size_t settledBytes = 0;
    std::optional<FileHeader> settledFile;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread saver;
};

}

#endif
