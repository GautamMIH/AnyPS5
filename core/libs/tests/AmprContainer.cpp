#include "prx/libc/include/AmprContainer.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename TAction>
void expectFailure(TAction action, const char* message) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error(message);
}

template <typename T>
void put(std::vector<char>& data, std::size_t offset, T value) {
    if (data.size() < offset + sizeof(T)) data.resize(offset + sizeof(T));
    std::memcpy(data.data() + offset, &value, sizeof(T));
}

std::vector<char> literalBlock(const std::vector<char>& literals) {
    std::vector<char> block;
    auto length = literals.size();
    block.push_back(static_cast<char>(length >= 15 ? 0xf0 : length << 4u));
    if (length >= 15) {
        for (length -= 15; length >= 255; length -= 255) block.push_back(static_cast<char>(255));
        block.push_back(static_cast<char>(length));
    }
    block.insert(block.end(), literals.begin(), literals.end());
    return block;
}

std::vector<char> readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

void testLz4() {
    const char matchBlock[] = {0x35, 'a', 'b', 'c', 0x03, 0x00};
    char out[12]{};
    AmprContainer::DecodeLz4Block(matchBlock, sizeof(matchBlock), out, sizeof(out));
    check(std::string(out, sizeof(out)) == "abcabcabcabc", "overlapping LZ4 match decoded incorrectly");
    char small[8]{};
    expectFailure([&] { AmprContainer::DecodeLz4Block(matchBlock, sizeof(matchBlock), small, sizeof(small)); }, "LZ4 overflow accepted");
    const char badOffset[] = {0x35, 'a', 'b', 'c', 0x09, 0x00};
    expectFailure([&] { AmprContainer::DecodeLz4Block(badOffset, sizeof(badOffset), out, sizeof(out)); }, "LZ4 offset before output start accepted");
    expectFailure([&] { AmprContainer::DecodeLz4Block(matchBlock, 3, out, 3); }, "truncated LZ4 literals accepted");
}

void testContainer(const std::filesystem::path& root) {
    std::filesystem::remove_all(root);
    const auto game = root / "app0";
    std::filesystem::create_directories(game);

    // Packed file: two 16 KiB chunks, the first stored raw, the second an LZ4 block; the third
    // (partial) chunk is shared with the first file's data to model deduplication.
    std::vector<char> content(0x4000 * 2 + 100);
    for (std::size_t i = 0; i < content.size(); ++i) content[i] = static_cast<char>((i * 7 + 3) & 0xff);
    const std::vector<char> chunk0(content.begin(), content.begin() + 0x4000);
    const auto chunk1 = literalBlock(std::vector<char>(content.begin() + 0x4000, content.begin() + 0x8000));
    const auto chunk2 = literalBlock(std::vector<char>(content.begin() + 0x8000, content.end()));

    std::vector<char> pak(0x10000, 0);
    std::memcpy(pak.data(), "AMPRDAT3", 8);
    const auto place = [&](const std::vector<char>& bytes) {
        while (pak.size() % 64) pak.push_back(0);
        const auto offset = pak.size();
        pak.insert(pak.end(), bytes.begin(), bytes.end());
        return static_cast<std::uint64_t>(offset);
    };
    const auto off0 = place(chunk0);
    const auto off1 = place(chunk1);
    const auto off2 = place(chunk2);
    while (pak.size() % 0x10000) pak.push_back(0);
    std::ofstream(game / "ampr_assets-bulk-lane00-vol00-000.pak", std::ios::binary).write(pak.data(), static_cast<std::streamsize>(pak.size()));

    const std::string names[] = {"/app0/Data/Blob.bin", "/app0/loose.txt", "ampr_assets-bulk-lane00-vol00-000.pak"};
    std::vector<char> strings;
    std::vector<std::uint32_t> nameOffsets;
    for (const auto& name : names) {
        nameOffsets.push_back(static_cast<std::uint32_t>(strings.size()));
        strings.insert(strings.end(), name.begin(), name.end());
        strings.push_back('\0');
    }

    const std::uint64_t fileCount = 2, extentCount = 3, packCount = 1;
    const std::uint64_t filesOffset = 0x80, extentsOffset = filesOffset + fileCount * 0x30;
    const std::uint64_t packsOffset = extentsOffset + extentCount * 0x0c, stringsOffset = packsOffset + packCount * 0x20;
    std::vector<char> index(stringsOffset, 0);
    std::memcpy(index.data(), "AMPRPAK4", 8);
    put<std::uint32_t>(index, 0x08, 4);
    put<std::uint32_t>(index, 0x0c, 0x80);
    put<std::uint32_t>(index, 0x14, 0x01020304);
    put<std::uint64_t>(index, 0x28, fileCount);
    put<std::uint64_t>(index, 0x30, extentCount);
    put<std::uint32_t>(index, 0x38, static_cast<std::uint32_t>(packCount));
    put<std::uint32_t>(index, 0x3c, 0x30);
    put<std::uint32_t>(index, 0x40, 0x0c);
    put<std::uint32_t>(index, 0x44, 0x20);
    put<std::uint64_t>(index, 0x48, filesOffset);
    put<std::uint64_t>(index, 0x50, extentsOffset);
    put<std::uint64_t>(index, 0x58, packsOffset);
    put<std::uint64_t>(index, 0x60, stringsOffset);
    put<std::uint64_t>(index, 0x68, strings.size());

    put<std::uint64_t>(index, filesOffset + 0x08, content.size());
    put<std::uint32_t>(index, filesOffset + 0x18, 0);
    put<std::uint32_t>(index, filesOffset + 0x1c, 3);
    put<std::uint32_t>(index, filesOffset + 0x20, nameOffsets[0]);
    put<std::uint32_t>(index, filesOffset + 0x24, static_cast<std::uint32_t>(names[0].size()));
    put<std::uint32_t>(index, filesOffset + 0x28, 1);
    put<std::uint8_t>(index, filesOffset + 0x2c, 14);
    put<std::uint64_t>(index, filesOffset + 0x30 + 0x08, 5);
    put<std::uint32_t>(index, filesOffset + 0x30 + 0x20, nameOffsets[1]);
    put<std::uint32_t>(index, filesOffset + 0x30 + 0x24, static_cast<std::uint32_t>(names[1].size()));

    const auto extent = [&](std::size_t i, std::uint64_t offset, std::size_t stored, std::uint32_t codec) {
        put<std::uint64_t>(index, extentsOffset + i * 0x0c, offset);
        put<std::uint32_t>(index, extentsOffset + i * 0x0c + 8, static_cast<std::uint32_t>(stored - 1) | (codec << 20u));
    };
    extent(0, off0, chunk0.size(), 0);
    extent(1, off1, chunk1.size(), 1);
    extent(2, off2, chunk2.size(), 1);

    put<std::uint64_t>(index, packsOffset, pak.size() - 0x10000);
    put<std::uint64_t>(index, packsOffset + 0x08, pak.size());
    put<std::uint32_t>(index, packsOffset + 0x10, nameOffsets[2]);
    put<std::uint32_t>(index, packsOffset + 0x14, static_cast<std::uint32_t>(names[2].size()));
    put<std::uint32_t>(index, packsOffset + 0x18, 2);
    put<std::uint32_t>(index, packsOffset + 0x1c, 0x10000);
    index.insert(index.end(), strings.begin(), strings.end());
    std::ofstream(game / "ampr_assets.index", std::ios::binary).write(index.data(), static_cast<std::streamsize>(index.size()));

    auto container = AmprContainer::Container::Open(game, root / "cache");
    check(container != nullptr && container->PackedFileCount() == 1, "container did not load its packed file");
    const auto resolved = container->Resolve("/APP0\\data/blob.BIN");
    check(resolved.has_value() && *resolved == root / "cache" / "app0/Data/Blob.bin", "case-insensitive lookup failed");
    check(readFile(*resolved) == content, "extracted file differs from its chunks");
    check(container->Resolve("/app0/Data").value() == root / "cache" / "app0/Data" && std::filesystem::is_directory(root / "cache" / "app0/Data"), "container-only directory was not resolved");
    check(!container->Resolve("/app0/loose.txt").has_value(), "loose entry was served from the container");
    check(!container->Resolve("/app0/missing.bin").has_value(), "unknown path resolved");

    // A second container instance reuses the extracted file instead of rewriting it.
    std::filesystem::last_write_time(*resolved, std::filesystem::file_time_type{});
    auto reopened = AmprContainer::Container::Open(game, root / "cache");
    reopened->Resolve("/app0/Data/Blob.bin");
    check(std::filesystem::last_write_time(*resolved) == std::filesystem::file_time_type{}, "cached extraction was rewritten");

    // Corrupt data fails loudly instead of producing a short file.
    extent(1, off1, chunk1.size() - 1, 1);
    std::ofstream(game / "ampr_assets.index", std::ios::binary).write(index.data(), static_cast<std::streamsize>(index.size()));
    std::filesystem::remove(*resolved);
    auto corrupt = AmprContainer::Container::Open(game, root / "cache");
    expectFailure([&] { corrupt->Resolve("/app0/Data/Blob.bin"); }, "corrupt chunk was accepted");
    check(!std::filesystem::exists(*resolved), "corrupt extraction left a final file behind");

    check(AmprContainer::Container::Open(root / "cache", root / "other") == nullptr, "directory without an index produced a container");
    std::filesystem::remove_all(root);
}

}

int main() {
    try {
        testLz4();
        testContainer(std::filesystem::temp_directory_path() / "anyps5-ampr-container-test");
        std::puts("AMPR container tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
