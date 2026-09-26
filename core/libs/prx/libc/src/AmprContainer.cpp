#include "prx/libc/include/AmprContainer.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace AmprContainer {

namespace {

constexpr char kIndexName[] = "ampr_assets.index";
constexpr std::uint32_t kIndexVersion = 4;
constexpr std::uint32_t kHeaderSize = 0x80;
constexpr std::uint32_t kFileEntrySize = 0x30;
constexpr std::uint32_t kExtentEntrySize = 0x0c;
constexpr std::uint32_t kPackEntrySize = 0x20;
constexpr std::uint32_t kFlagPacked = 0x01;
constexpr std::uint32_t kFlagUncompressed = 0x02;
constexpr std::uint32_t kCodecStored = 0;
constexpr std::uint32_t kCodecLz4 = 1;

[[noreturn]] void fail(const std::string& reason) {
    throw std::runtime_error("AMPR container: " + reason);
}

void require(bool condition, const char* reason) {
    if (!condition) fail(reason);
}

template <typename T>
T read(const std::vector<char>& data, std::uint64_t offset) {
    require(offset <= data.size() && sizeof(T) <= data.size() - offset, "index field out of bounds");
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

std::string readString(const std::vector<char>& data, std::uint64_t tableOffset, std::uint64_t tableSize, std::uint32_t offset, std::uint32_t length) {
    require(static_cast<std::uint64_t>(offset) + length < tableSize, "string outside the string table");
    const char* text = data.data() + tableOffset + offset;
    require(text[length] == '\0' && std::memchr(text, '\0', length) == nullptr, "malformed string");
    return std::string(text, length);
}

// A relative volume name must stay inside the game directory.
bool safeRelativeName(const std::string& name) {
    if (name.empty() || name.front() == '/' || name.front() == '\\') return false;
    for (const auto& component : std::filesystem::path(name))
        if (component == "." || component == "..") return false;
    return true;
}

}

std::string NormalisePath(std::string_view path) {
    std::string result(path);
    for (auto& c : result) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    while (result.size() > 1 && result.back() == '/') result.pop_back();
    return result;
}

void DecodeLz4Block(const char* source, std::size_t sourceSize, char* destination, std::size_t destinationSize) {
    const auto* in = reinterpret_cast<const unsigned char*>(source);
    std::size_t ip = 0;
    std::size_t op = 0;
    const auto length = [&](std::size_t value) {
        if (value != 15) return value;
        for (;;) {
            require(ip < sourceSize, "truncated LZ4 length");
            const auto extra = in[ip++];
            value += extra;
            if (extra != 255) return value;
        }
    };
    while (ip < sourceSize) {
        const auto token = in[ip++];
        const auto literals = length(token >> 4u);
        require(literals <= sourceSize - ip && literals <= destinationSize - op, "LZ4 literals out of bounds");
        std::memcpy(destination + op, in + ip, literals);
        ip += literals;
        op += literals;
        if (ip == sourceSize) break;
        require(sourceSize - ip >= 2, "truncated LZ4 match offset");
        const std::size_t offset = in[ip] | (static_cast<std::size_t>(in[ip + 1]) << 8u);
        ip += 2;
        require(offset != 0 && offset <= op, "invalid LZ4 match offset");
        const auto match = length(token & 15u) + 4;
        require(match <= destinationSize - op, "LZ4 match out of bounds");
        for (std::size_t i = 0; i < match; ++i, ++op) destination[op] = destination[op - offset];
    }
    require(op == destinationSize, "LZ4 block decoded to an unexpected size");
}

std::unique_ptr<Container> Container::Open(const std::filesystem::path& gameRoot, std::filesystem::path cacheRoot) {
    const auto indexPath = gameRoot / kIndexName;
    std::error_code error;
    if (!std::filesystem::is_regular_file(indexPath, error)) return nullptr;

    std::ifstream stream(indexPath, std::ios::binary);
    require(stream.good(), "cannot open the index");
    std::vector<char> data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    require(data.size() >= kHeaderSize && std::memcmp(data.data(), "AMPRPAK4", 8) == 0, "bad index magic");
    require(read<std::uint32_t>(data, 0x08) == kIndexVersion && read<std::uint32_t>(data, 0x0c) == kHeaderSize, "unsupported index version");
    require(read<std::uint32_t>(data, 0x14) == 0x01020304u, "bad index byte-order marker");
    const auto fileCount = read<std::uint64_t>(data, 0x28);
    const auto extentCount = read<std::uint64_t>(data, 0x30);
    const auto packCount = read<std::uint32_t>(data, 0x38);
    require(read<std::uint32_t>(data, 0x3c) == kFileEntrySize && read<std::uint32_t>(data, 0x40) == kExtentEntrySize && read<std::uint32_t>(data, 0x44) == kPackEntrySize, "unexpected index entry sizes");
    require(fileCount <= 2000000 && extentCount <= 16000000 && packCount <= 0x400, "index counts out of range");
    const auto filesOffset = read<std::uint64_t>(data, 0x48);
    const auto extentsOffset = read<std::uint64_t>(data, 0x50);
    const auto packsOffset = read<std::uint64_t>(data, 0x58);
    const auto stringsOffset = read<std::uint64_t>(data, 0x60);
    const auto stringsSize = read<std::uint64_t>(data, 0x68);
    require(filesOffset == kHeaderSize && extentsOffset == filesOffset + fileCount * kFileEntrySize && packsOffset == extentsOffset + extentCount * kExtentEntrySize && stringsOffset == packsOffset + std::uint64_t{packCount} * kPackEntrySize && stringsOffset + stringsSize == data.size(), "inconsistent index table layout");

    std::unique_ptr<Container> container(new Container());
    container->cacheRoot = std::move(cacheRoot);

    for (std::uint32_t i = 0; i < packCount; ++i) {
        const auto entry = packsOffset + std::uint64_t{i} * kPackEntrySize;
        const auto payloadSize = read<std::uint64_t>(data, entry);
        const auto fileSize = read<std::uint64_t>(data, entry + 0x08);
        const auto name = readString(data, stringsOffset, stringsSize, read<std::uint32_t>(data, entry + 0x10), read<std::uint32_t>(data, entry + 0x14));
        require(payloadSize <= fileSize && safeRelativeName(name), "invalid volume entry");
        container->packs.push_back(Pack{gameRoot / name, fileSize, fileSize - payloadSize, nullptr});
    }

    container->extents.resize(static_cast<std::size_t>(extentCount));
    for (std::uint64_t i = 0; i < extentCount; ++i) {
        const auto entry = extentsOffset + i * kExtentEntrySize;
        container->extents[static_cast<std::size_t>(i)] = Extent{read<std::uint64_t>(data, entry), read<std::uint32_t>(data, entry + 0x08)};
    }

    for (std::uint64_t i = 0; i < fileCount; ++i) {
        const auto entry = filesOffset + i * kFileEntrySize;
        File file{};
        file.size = read<std::uint64_t>(data, entry + 0x08);
        file.firstExtent = read<std::uint32_t>(data, entry + 0x18);
        file.extentCount = read<std::uint32_t>(data, entry + 0x1c);
        file.path = readString(data, stringsOffset, stringsSize, read<std::uint32_t>(data, entry + 0x20), read<std::uint32_t>(data, entry + 0x24));
        file.flags = read<std::uint32_t>(data, entry + 0x28);
        file.chunkLog2 = read<std::uint8_t>(data, entry + 0x2c);
        if ((file.flags & kFlagPacked) == 0) continue;
        const auto normalised = NormalisePath(file.path);
        require(normalised.rfind("/app0/", 0) == 0, "packed path outside /app0");
        require(safeRelativeName(file.path.substr(1)), "unsafe packed path");
        require(file.chunkLog2 >= 14 && file.chunkLog2 <= 20, "invalid chunk size");
        const auto chunk = std::uint64_t{1} << file.chunkLog2;
        require(file.extentCount == (file.size + chunk - 1) / chunk, "extent count does not match the file size");
        require(std::uint64_t{file.firstExtent} + file.extentCount <= extentCount, "file extents outside the extent table");
        for (auto slash = normalised.find('/', 1); slash != std::string::npos; slash = normalised.find('/', slash + 1))
            container->directories.emplace(normalised.substr(0, slash), file.path.substr(0, slash));
        container->packedFiles.emplace(normalised, std::move(file));
    }
    return container;
}

std::optional<std::filesystem::path> Container::Resolve(std::string_view guestPath) {
    const auto key = NormalisePath(guestPath);
    std::lock_guard lock(mutex);
    if (const auto file = packedFiles.find(key); file != packedFiles.end()) return extract(file->second);
    if (const auto directory = directories.find(key); directory != directories.end()) {
        const auto target = cacheRoot / directory->second.substr(1);
        std::error_code error;
        std::filesystem::create_directories(target, error);
        if (error) fail("cannot create cache directory " + target.string() + ": " + error.message());
        return target;
    }
    return std::nullopt;
}

void Container::readChunk(const File& file, std::uint32_t index, std::vector<char>& stored, char* out, std::size_t size) {
    const auto& extent = extents[file.firstExtent + index];
    const auto packId = static_cast<std::size_t>(extent.location >> 48u);
    const auto offset = extent.location & 0xffffffffffffull;
    const auto storedSize = static_cast<std::size_t>(extent.word & 0xfffffu) + 1;
    const auto codec = (extent.word >> 20u) & 3u;
    require(packId < packs.size(), "extent references a missing volume");
    auto& pack = packs[packId];
    require(offset >= pack.payloadBegin && offset <= pack.fileSize && storedSize <= pack.fileSize - offset, "extent outside its volume");
    if (!pack.stream) {
        pack.stream = std::make_unique<std::ifstream>(pack.path, std::ios::binary);
        if (!pack.stream->good()) fail("cannot open volume " + pack.path.string());
    }
    stored.resize(storedSize);
    pack.stream->seekg(static_cast<std::streamoff>(offset));
    pack.stream->read(stored.data(), static_cast<std::streamsize>(storedSize));
    if (!*pack.stream) fail("short read from volume " + pack.path.string());
    if (codec == kCodecStored) {
        require(storedSize == size, "stored chunk size mismatch");
        std::memcpy(out, stored.data(), size);
    } else if (codec == kCodecLz4) {
        require((file.flags & kFlagUncompressed) == 0, "compressed chunk in an uncompressed file");
        DecodeLz4Block(stored.data(), storedSize, out, size);
    } else {
        fail("unsupported chunk codec");
    }
}

std::filesystem::path Container::extract(const File& file) {
    const auto target = cacheRoot / file.path.substr(1);
    std::error_code error;
    if (extracted.count(file.path) != 0 || (std::filesystem::is_regular_file(target, error) && std::filesystem::file_size(target, error) == file.size && !error)) {
        extracted.insert(file.path);
        return target;
    }
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) fail("cannot create cache directory " + target.parent_path().string() + ": " + error.message());
    auto partial = target;
    partial += ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out) fail("cannot write " + partial.string());
        const auto chunk = std::uint64_t{1} << file.chunkLog2;
        std::vector<char> stored;
        std::vector<char> decoded(static_cast<std::size_t>(std::min(chunk, file.size)));
        for (std::uint32_t i = 0; i < file.extentCount; ++i) {
            const auto size = static_cast<std::size_t>(std::min(chunk, file.size - std::uint64_t{i} * chunk));
            readChunk(file, i, stored, decoded.data(), size);
            out.write(decoded.data(), static_cast<std::streamsize>(size));
        }
        if (!out.flush()) fail("cannot write " + partial.string());
    }
    std::filesystem::rename(partial, target, error);
    if (error) fail("cannot finalise " + target.string() + ": " + error.message());
    extracted.insert(file.path);
    return target;
}

}
