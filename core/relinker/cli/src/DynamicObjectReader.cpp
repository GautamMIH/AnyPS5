#include <DynamicObjectReader.hpp>
#include <domain/Types.hpp>
#include <cstring>
#include <map>

namespace Cli {

namespace {

constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPtDynamic = 2;
constexpr std::uint32_t kShtDynsym = 11;
constexpr std::int64_t kDtNull = 0;
constexpr std::int64_t kDtNeeded = 1;
constexpr std::int64_t kDtHash = 4;
constexpr std::int64_t kDtStrtab = 5;
constexpr std::int64_t kDtSymtab = 6;
constexpr std::int64_t kDtVersym = 0x6ffffff0;
constexpr std::int64_t kDtVerdef = 0x6ffffffc;
constexpr std::int64_t kDtVerdefnum = 0x6ffffffd;
constexpr std::int64_t kDtVerneed = 0x6ffffffe;
constexpr std::int64_t kDtVerneednum = 0x6fffffff;
constexpr std::size_t kSymbolEntrySize = 24;
constexpr std::uint16_t kVersionIndexMask = 0x7fff;
constexpr std::uint16_t kVersionFlagBase = 1;

template<typename TValue>
TValue readAt(const std::vector<std::uint8_t>& image, const std::uint64_t offset) {
    if (offset > image.size() || image.size() - offset < sizeof(TValue))
        throw Domain::RelinkerException("Dynamic object read out of bounds", offset);
    TValue value;
    std::memcpy(&value, image.data() + offset, sizeof(TValue));
    return value;
}

std::string readString(const std::vector<std::uint8_t>& image, const std::uint64_t offset) {
    if (offset >= image.size())
        throw Domain::RelinkerException("Dynamic string out of bounds", offset);
    std::string result;
    for (std::uint64_t pos = offset; pos < image.size() && image[pos] != 0; ++pos)
        result.push_back(static_cast<char>(image[pos]));
    return result;
}

}

DynamicObjectInfo ReadDynamicObject(const std::vector<std::uint8_t>& image) {
    if (image.size() < 0x40 || std::memcmp(image.data(), "\x7f" "ELF", 4) != 0 || image[4] != 2)
        throw Domain::RelinkerException("Not an ELF64 dynamic object");

    struct Segment {
        std::uint32_t Type;
        std::uint64_t Offset;
        std::uint64_t Vaddr;
        std::uint64_t FileSize;
    };
    std::vector<Segment> segments;
    const auto phOff = readAt<std::uint64_t>(image, 0x20);
    const auto phEntSize = readAt<std::uint16_t>(image, 0x36);
    const auto phNum = readAt<std::uint16_t>(image, 0x38);
    for (std::uint16_t index = 0; index < phNum; ++index) {
        const std::uint64_t entry = phOff + static_cast<std::uint64_t>(index) * phEntSize;
        segments.push_back({readAt<std::uint32_t>(image, entry), readAt<std::uint64_t>(image, entry + 8), readAt<std::uint64_t>(image, entry + 16), readAt<std::uint64_t>(image, entry + 32)});
    }

    const auto toOffset = [&segments](const std::uint64_t vaddr) -> std::uint64_t {
        for (const auto& segment : segments)
            if (segment.Type == kPtLoad && vaddr >= segment.Vaddr && vaddr < segment.Vaddr + segment.FileSize)
                return segment.Offset + (vaddr - segment.Vaddr);
        throw Domain::RelinkerException("Dynamic object address is not mapped", vaddr);
    };

    std::multimap<std::int64_t, std::uint64_t> tags;
    for (const auto& segment : segments) {
        if (segment.Type != kPtDynamic)
            continue;
        for (std::uint64_t pos = segment.Offset; pos + 16 <= segment.Offset + segment.FileSize; pos += 16) {
            const auto tag = readAt<std::int64_t>(image, pos);
            if (tag == kDtNull)
                break;
            tags.emplace(tag, readAt<std::uint64_t>(image, pos + 8));
        }
    }
    const auto tagValue = [&tags](const std::int64_t tag) -> std::uint64_t {
        const auto it = tags.find(tag);
        if (it == tags.end())
            throw Domain::RelinkerException("Dynamic object lacks a required dynamic tag", static_cast<std::uint64_t>(tag));
        return it->second;
    };

    DynamicObjectInfo info;
    const std::uint64_t strtab = toOffset(tagValue(kDtStrtab));
    const auto [neededBegin, neededEnd] = tags.equal_range(kDtNeeded);
    for (auto it = neededBegin; it != neededEnd; ++it)
        info.Needed.push_back(readString(image, strtab + it->second));

    std::uint64_t symbolCount = 0;
    const auto shOff = readAt<std::uint64_t>(image, 0x28);
    const auto shEntSize = readAt<std::uint16_t>(image, 0x3a);
    const auto shNum = readAt<std::uint16_t>(image, 0x3c);
    for (std::uint16_t index = 0; shOff != 0 && index < shNum; ++index) {
        const std::uint64_t entry = shOff + static_cast<std::uint64_t>(index) * shEntSize;
        if (readAt<std::uint32_t>(image, entry + 4) == kShtDynsym)
            symbolCount = readAt<std::uint64_t>(image, entry + 32) / kSymbolEntrySize;
    }
    if (symbolCount == 0 && tags.count(kDtHash) != 0)
        symbolCount = readAt<std::uint32_t>(image, toOffset(tagValue(kDtHash)) + 4);
    if (symbolCount == 0)
        throw Domain::RelinkerException("Cannot determine the dynamic symbol count");

    std::map<std::uint16_t, std::pair<std::string, std::string>> versions;
    if (tags.count(kDtVerdef) != 0) {
        info.HasVersionDefinitions = true;
        std::uint64_t entry = toOffset(tagValue(kDtVerdef));
        for (std::uint64_t index = 0; index < tagValue(kDtVerdefnum); ++index) {
            const auto flags = readAt<std::uint16_t>(image, entry + 2);
            const auto versionIndex = readAt<std::uint16_t>(image, entry + 4);
            const auto aux = readAt<std::uint32_t>(image, entry + 12);
            if ((flags & kVersionFlagBase) == 0)
                versions[versionIndex] = {readString(image, strtab + readAt<std::uint32_t>(image, entry + aux)), {}};
            const auto next = readAt<std::uint32_t>(image, entry + 16);
            if (next == 0)
                break;
            entry += next;
        }
    }
    if (tags.count(kDtVerneed) != 0) {
        std::uint64_t entry = toOffset(tagValue(kDtVerneed));
        for (std::uint64_t index = 0; index < tagValue(kDtVerneednum); ++index) {
            const auto count = readAt<std::uint16_t>(image, entry + 2);
            const std::string file = readString(image, strtab + readAt<std::uint32_t>(image, entry + 4));
            std::uint64_t aux = entry + readAt<std::uint32_t>(image, entry + 8);
            for (std::uint16_t auxIndex = 0; auxIndex < count; ++auxIndex) {
                versions[readAt<std::uint16_t>(image, aux + 6)] = {readString(image, strtab + readAt<std::uint32_t>(image, aux + 8)), file};
                const auto next = readAt<std::uint32_t>(image, aux + 12);
                if (next == 0)
                    break;
                aux += next;
            }
            const auto next = readAt<std::uint32_t>(image, entry + 12);
            if (next == 0)
                break;
            entry += next;
        }
    }

    const std::uint64_t symtab = toOffset(tagValue(kDtSymtab));
    const bool hasVersym = tags.count(kDtVersym) != 0;
    const std::uint64_t versym = hasVersym ? toOffset(tagValue(kDtVersym)) : 0;
    for (std::uint64_t index = 1; index < symbolCount; ++index) {
        const std::uint64_t entry = symtab + index * kSymbolEntrySize;
        DynamicSymbolInfo symbol{readString(image, strtab + readAt<std::uint32_t>(image, entry)), readAt<std::uint8_t>(image, entry + 4), readAt<std::uint16_t>(image, entry + 6) != 0, {}, {}};
        if (hasVersym) {
            const auto found = versions.find(static_cast<std::uint16_t>(readAt<std::uint16_t>(image, versym + index * 2) & kVersionIndexMask));
            if (found != versions.end()) {
                symbol.Version = found->second.first;
                symbol.VersionFile = found->second.second;
            }
        }
        info.Symbols.push_back(std::move(symbol));
    }
    return info;
}

}
