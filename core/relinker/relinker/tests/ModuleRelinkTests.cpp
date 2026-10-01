#include <relinker/parsing/SelfUnwrapper.hpp>
#include <relinker/output/SysVDynamicSectionBuilder.hpp>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void requireFailure(const std::function<void()>& operation, const char* message) {
    try {
        operation();
    } catch (const Relinker::RelinkerException&) {
        return;
    }
    throw std::runtime_error(message);
}

template<typename TValue>
void write(std::vector<std::uint8_t>& bytes, std::size_t offset, TValue value) {
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) throw std::runtime_error("Test fixture write is out of bounds");
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template<typename TValue>
TValue read(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || sizeof(TValue) > bytes.size() - offset) throw std::runtime_error("Test fixture read is out of bounds");
    TValue value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

constexpr std::size_t kLoadOffset = 0x1000;
constexpr std::size_t kVersionOffset = 0x1010;
constexpr std::uint8_t kLoadBytes[] = {0x55, 0x48, 0x89, 0xe5, 0x5d, 0xc3};
constexpr std::uint8_t kVersionBytes[] = {0x01, 0x02, 0x03, 0x04};

std::vector<std::uint8_t> embeddedElfHeader() {
    std::vector<std::uint8_t> elf(0x40 + 2 * 0x38, 0);
    elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    write<std::uint16_t>(elf, 0x10, 0xfe18);
    write<std::uint64_t>(elf, 0x20, 0x40);
    write<std::uint64_t>(elf, 0x28, 0x1234);
    write<std::uint16_t>(elf, 0x36, 0x38);
    write<std::uint16_t>(elf, 0x38, 2);
    write<std::uint16_t>(elf, 0x3c, 7);
    write<std::uint32_t>(elf, 0x40, 1);
    write<std::uint64_t>(elf, 0x40 + 0x08, kLoadOffset);
    write<std::uint64_t>(elf, 0x40 + 0x20, sizeof(kLoadBytes));
    write<std::uint32_t>(elf, 0x78, 0x6fffff01);
    write<std::uint64_t>(elf, 0x78 + 0x08, kVersionOffset);
    write<std::uint64_t>(elf, 0x78 + 0x20, sizeof(kVersionBytes));
    return elf;
}

std::vector<std::uint8_t> selfFile(std::uint64_t segmentFlags, std::uint8_t magic0 = 0x54) {
    const auto elf = embeddedElfHeader();
    std::vector<std::uint8_t> self(0x20 + 0x20, 0);
    self[0] = magic0;
    self[1] = magic0 == 0x54 ? 0x14 : 0x15;
    self[2] = magic0 == 0x54 ? 0xf5 : 0x3d;
    self[3] = magic0 == 0x54 ? 0xee : 0x1d;
    write<std::uint16_t>(self, 0x18, 1);
    self.insert(self.end(), elf.begin(), elf.end());
    const std::size_t dataOffset = self.size();
    self.insert(self.end(), std::begin(kLoadBytes), std::end(kLoadBytes));
    self.insert(self.end(), std::begin(kVersionBytes), std::end(kVersionBytes));
    write<std::uint64_t>(self, 0x20, segmentFlags);
    write<std::uint64_t>(self, 0x28, dataOffset);
    write<std::uint64_t>(self, 0x30, sizeof(kLoadBytes));
    write<std::uint64_t>(self, 0x38, sizeof(kLoadBytes));
    return self;
}

void plainElfPassesThrough() {
    const auto elf = embeddedElfHeader();
    const Relinker::SelfUnwrapper unwrapper;
    require(!unwrapper.IsSelf(elf), "Plain ELF was detected as SELF");
    require(unwrapper.Unwrap(elf) == elf, "Plain ELF was modified");
}

void unwrapsPs4AndPs5Selfs() {
    const Relinker::SelfUnwrapper unwrapper;
    for (const std::uint8_t magic0 : {std::uint8_t{0x54}, std::uint8_t{0x4f}}) {
        const auto self = selfFile(0x800, magic0);
        require(unwrapper.IsSelf(self), "SELF magic was not detected");
        const auto image = unwrapper.Unwrap(self);
        require(image.size() == kVersionOffset + sizeof(kVersionBytes), "Unwrapped image has the wrong size");
        require(image[0] == 0x7f && image[1] == 'E', "Unwrapped image lacks the ELF header");
        require(std::memcmp(image.data() + kLoadOffset, kLoadBytes, sizeof(kLoadBytes)) == 0, "Segment data was not placed at its program header offset");
        require(std::memcmp(image.data() + kVersionOffset, kVersionBytes, sizeof(kVersionBytes)) == 0, "Version segment was not restored from the SELF tail");
        require(read<std::uint64_t>(image, 0x28) == 0 && read<std::uint16_t>(image, 0x3c) == 0, "Section header references were not cleared");
    }
}

void rejectsUnsupportedSelfs() {
    const Relinker::SelfUnwrapper unwrapper;
    requireFailure([&] { (void)unwrapper.Unwrap(selfFile(0x800 | 0x2)); }, "Encrypted SELF segment was accepted");
    requireFailure([&] { (void)unwrapper.Unwrap(selfFile(0x800 | 0x8)); }, "Compressed SELF segment was accepted");
    requireFailure([&] { (void)unwrapper.Unwrap(selfFile(0x800 | (5ull << 20))); }, "SELF segment with a missing program header was accepted");
    // Inputs that are not SELFs pass through unchanged; the ELF parser rejects what is not an ELF.
    const std::vector<std::uint8_t> unknown(64, 0xab);
    require(unwrapper.Unwrap(unknown) == unknown, "A non-SELF input was changed by the SELF unwrapper");
}

std::string symbolName(const Domain::SysVDynamicSection& section, std::uint32_t index) {
    const auto nameOffset = read<std::uint32_t>(section.DynSymData, static_cast<std::size_t>(index) * 24);
    return std::string(reinterpret_cast<const char*>(section.DynStrData.data() + nameOffset));
}

std::uint32_t sysvHash(const std::string& name) {
    std::uint32_t hash = 0;
    for (const char c : name) {
        hash = (hash << 4) + static_cast<std::uint8_t>(c);
        const std::uint32_t high = hash & 0xf0000000u;
        if (high != 0) hash ^= high >> 24;
        hash &= ~high;
    }
    return hash;
}

std::uint32_t lookup(const Domain::SysVDynamicSection& section, const std::string& name) {
    const auto bucketCount = read<std::uint32_t>(section.HashData, 0);
    const auto chainCount = read<std::uint32_t>(section.HashData, 4);
    auto index = read<std::uint32_t>(section.HashData, 8 + 4 * (sysvHash(name) % bucketCount));
    while (index != 0) {
        if (symbolName(section, index) == name) return index;
        index = read<std::uint32_t>(section.HashData, 8 + 4 * bucketCount + 4 * index);
        require(index < chainCount, "Hash chain index is out of bounds");
    }
    return 0;
}

void exportsAreHashedAndImportsKeepBinding() {
    Relinker::SysVDynamicSectionBuilder builder;
    constexpr std::uint8_t kWeakFunction = 0x22;
    const std::vector<Domain::NidReference> references = {
        {"aaaaaaaaaaa#A#A", {}, 7, 0x100, 0x2000, 0, kWeakFunction},
        {"bbbbbbbbbbb#B#B", {}, 6, 0x200, 0x2008, 0, 0x11},
    };
    auto section = builder.BuildDynamicSection(references, {"libkernel.prx"}, 0x100, 1);
    builder.AppendExportsAndHash(section, {{"ccccccccccc", 0x12, 0x4000, 16}, {"ddddddddddd", 0x11, 0x5000, 8}});

    require(section.DynSymData.size() == 5 * 24, "Unexpected dynamic symbol count");
    require(section.DynSymData[24 + 4] == kWeakFunction, "Weak import lost its binding");
    const auto exportIndex = lookup(section, "ccccccccccc");
    require(exportIndex == 3, "Export is not reachable through DT_HASH");
    require(read<std::uint16_t>(section.DynSymData, exportIndex * 24 + 6) != 0, "Export is not a defined symbol");
    require(read<std::uint64_t>(section.DynSymData, exportIndex * 24 + 8) == 0x4000, "Export has the wrong value");
    require(lookup(section, "ddddddddddd") == 4, "Object export is not reachable through DT_HASH");
    require(lookup(section, "aaaaaaaaaaa") == 1 && lookup(section, "bbbbbbbbbbb") == 2, "Imports are not reachable through DT_HASH");
    require(lookup(section, "eeeeeeeeeee") == 0, "Unknown name was found in DT_HASH");
    requireFailure([&] { builder.AppendExportsAndHash(section, {{"", 0x12, 0, 0}}); }, "Empty export name was accepted");
}

}

int main() {
    try {
        plainElfPassesThrough();
        unwrapsPs4AndPs5Selfs();
        rejectsUnsupportedSelfs();
        exportsAreHashedAndImportsKeepBinding();
        std::cout << "Module relink tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
