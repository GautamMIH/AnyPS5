#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_AMPRCONTAINER_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_AMPRCONTAINER_HPP

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Reader for the AMPR asset container some title dumps use to store part of /app0:
// ampr_assets.index (AMPRPAK4) describes files as chunk lists inside ampr_assets-*.pak volumes
// (AMPRDAT3); chunks are stored raw or as LZ4 blocks and may be shared between files. Files the
// index marks as loose live on the real filesystem and are not served from here.
//
// Packed files are materialised on first use into a cache directory, so every file API (open,
// stat, stdio, mmap, APR) sees an ordinary host file.
namespace AmprContainer {

class Container {
public:
    // Loads <gameRoot>/ampr_assets.index; returns null when the title has no container. Throws on a
    // malformed container.
    static std::unique_ptr<Container> Open(const std::filesystem::path& gameRoot, std::filesystem::path cacheRoot);

    // Maps a guest path ("/app0/..."; case-insensitive, '\\' accepted) to a host path when it names a
    // packed file (extracted on first use) or a directory that only exists inside the container.
    std::optional<std::filesystem::path> Resolve(std::string_view guestPath);

    std::size_t PackedFileCount() const { return packedFiles.size(); }

private:
    struct Extent {
        std::uint64_t location;
        std::uint32_t word;
    };
    struct File {
        std::string path;
        std::uint64_t size;
        std::uint32_t firstExtent;
        std::uint32_t extentCount;
        std::uint32_t flags;
        std::uint8_t chunkLog2;
    };
    struct Pack {
        std::filesystem::path path;
        std::uint64_t fileSize;
        std::uint64_t payloadBegin;
        std::unique_ptr<std::ifstream> stream;
    };

    Container() = default;
    std::filesystem::path extract(const File& file);
    void readChunk(const File& file, std::uint32_t index, std::vector<char>& stored, char* out, std::size_t size);

    std::filesystem::path cacheRoot;
    std::vector<Extent> extents;
    std::vector<Pack> packs;
    std::map<std::string, File> packedFiles;        // key: normalised path
    std::map<std::string, std::string> directories;  // key: normalised path, value: stored-case path
    std::set<std::string> extracted;
    std::mutex mutex;
};

// Lower-cases ASCII and converts '\\' to '/'; the container's lookup rule.
std::string NormalisePath(std::string_view path);

// Decodes one LZ4 block of exactly source.size() bytes into exactly destinationSize bytes.
void DecodeLz4Block(const char* source, std::size_t sourceSize, char* destination, std::size_t destinationSize);

}

#endif
