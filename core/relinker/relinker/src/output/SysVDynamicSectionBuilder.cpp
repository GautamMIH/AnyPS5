#include <relinker/output/SysVDynamicSectionBuilder.hpp>
#include <algorithm>
#include <elfpatcher/general/ElfConstants.hpp>
#include <cstring>
#include <map>

namespace Relinker {

using namespace Elfpatcher;

void SysVDynamicSectionBuilder::_appendU64(std::vector<std::uint8_t>& buf, std::uint64_t v) const {
    std::size_t pos = buf.size();
    buf.resize(pos + 8);
    std::memcpy(buf.data() + pos, &v, 8);
}

void SysVDynamicSectionBuilder::_appendI64(std::vector<std::uint8_t>& buf, std::int64_t v) const {
    _appendU64(buf, static_cast<std::uint64_t>(v));
}

void SysVDynamicSectionBuilder::_appendDynEntry(std::vector<std::uint8_t>& buf, std::int64_t tag, std::uint64_t val) const {
    _appendI64(buf, tag);
    _appendU64(buf, val);
}

std::uint32_t SysVDynamicSectionBuilder::_appendStr(std::vector<std::uint8_t>& strtab, const std::string& s) const {
    auto offset = static_cast<std::uint32_t>(strtab.size());
    for (char c : s)
        strtab.push_back(static_cast<std::uint8_t>(c));
    strtab.push_back(0);
    return offset;
}

void SysVDynamicSectionBuilder::_appendElfSym(
    std::vector<std::uint8_t>& dynsym,
    std::uint32_t nameOff,
    std::uint8_t info,
    std::uint8_t other,
    std::uint16_t shndx,
    std::uint64_t value,
    std::uint64_t size) const
{
    dynsym.push_back(nameOff & 0xFF);
    dynsym.push_back((nameOff >> 8) & 0xFF);
    dynsym.push_back((nameOff >> 16) & 0xFF);
    dynsym.push_back((nameOff >> 24) & 0xFF);
    dynsym.push_back(info);
    dynsym.push_back(other);
    dynsym.push_back(shndx & 0xFF);
    dynsym.push_back((shndx >> 8) & 0xFF);
    _appendU64(dynsym, value);
    _appendU64(dynsym, size);
}

void SysVDynamicSectionBuilder::_appendRela(
    std::vector<std::uint8_t>& rela,
    std::uint64_t offset,
    std::uint64_t info,
    std::int64_t addend) const
{
    _appendU64(rela, offset);
    _appendU64(rela, info);
    _appendI64(rela, addend);
}

SysVDynamicSection SysVDynamicSectionBuilder::BuildDynamicSection(
    const std::vector<NidReference>& nidReferences,
    const std::vector<std::string>& neededLibraries,
    FileByteOffset originalJmprelOffset,
    std::uint32_t originalJmprelCount)
{
    SysVDynamicSection result;
    result.DynStrData.push_back(0);

    std::vector<std::uint32_t> neededOffsets;
    for (const auto& lib : neededLibraries)
        neededOffsets.push_back(_appendStr(result.DynStrData, lib));

    _appendElfSym(result.DynSymData, 0, 0, 0, 0, 0, 0);
    result.SymbolVersions.push_back({});

    auto importVersion = [](const NidReference& ref) -> SymbolVersion {
        if (ref.Library.empty() || ref.LibraryFile.empty()) return {};
        return {ref.Library, ref.LibraryFile};
    };

    auto stripHashSuffix = [](const std::string& value) -> std::string {
        const auto hashPos = value.find('#');
        if (hashPos == std::string::npos) return value;
        return value.substr(0, hashPos);
    };

    static constexpr std::size_t kRelaEntSize = 24;

    std::vector<const NidReference*> pltSlots(originalJmprelCount, nullptr);
    std::vector<const NidReference*> nonPltRefs;

    for (const auto& ref : nidReferences) {
        // Library is the SCE import library (the symbol version); LibraryFile is the module file it
        // comes from, which Windows import binding needs.
        if (!ref.LibraryFile.empty()) result.ImportModules.emplace(ref.RelocationAddress, ref.LibraryFile);
        std::uint32_t relType = ref.RelocationTypeValue;
        if (relType == 0) relType = R_X86_64_JUMP_SLOT;

        if (relType != R_X86_64_JUMP_SLOT) {
            nonPltRefs.push_back(&ref);
            continue;
        }

        if (ref.RelocationTableOffset < originalJmprelOffset)
            throw RelinkerException(
                "JUMP_SLOT relocation lies before the original .rela.plt table",
                ref.RelocationTableOffset);

        const std::uint64_t byteDelta = ref.RelocationTableOffset - originalJmprelOffset;
        if (byteDelta % kRelaEntSize != 0)
            throw RelinkerException(
                "JUMP_SLOT relocation is not aligned to the original .rela.plt entry size",
                ref.RelocationTableOffset);

        const std::uint64_t slotIndex = byteDelta / kRelaEntSize;
        if (slotIndex >= originalJmprelCount)
            throw RelinkerException(
                "JUMP_SLOT relocation index exceeds the original .rela.plt table size",
                ref.RelocationTableOffset);

        if (pltSlots[slotIndex] != nullptr)
            throw RelinkerException(
                "Duplicate JUMP_SLOT relocation for the same original .rela.plt slot",
                ref.RelocationTableOffset);

        pltSlots[slotIndex] = &ref;
    }

    std::uint32_t symIdx = 1;

    for (const NidReference* slot : pltSlots) {
        if (slot == nullptr)
            throw RelinkerException(
                "Original .rela.plt slot has no corresponding JUMP_SLOT relocation; "
                "PLT thunks cannot be filtered without patching their hard-coded reloc index");

        const NidReference& ref = *slot;
        const std::uint32_t nameOff = _appendStr(result.DynStrData, stripHashSuffix(ref.Nid));
        _appendElfSym(result.DynSymData, nameOff, ref.SymbolInfo, STV_DEFAULT, 0, 0, 0);
        result.SymbolVersions.push_back(importVersion(ref));

        const std::uint64_t relaInfo = (static_cast<std::uint64_t>(symIdx) << 32) | R_X86_64_JUMP_SLOT;
        _appendRela(result.RelaPltData, ref.RelocationAddress, relaInfo, ref.Addend);
        ++symIdx;
    }

    for (const NidReference* slotPtr : nonPltRefs) {
        const NidReference& ref = *slotPtr;
        const std::uint32_t nameOff = _appendStr(result.DynStrData, stripHashSuffix(ref.Nid));
        _appendElfSym(result.DynSymData, nameOff, ref.SymbolInfo, STV_DEFAULT, 0, 0, 0);
        result.SymbolVersions.push_back(importVersion(ref));

        const std::uint64_t relaInfo = (static_cast<std::uint64_t>(symIdx) << 32) | ref.RelocationTypeValue;
        _appendRela(result.RelaData, ref.RelocationAddress, relaInfo, ref.Addend);
        ++symIdx;
    }

    for (const std::uint32_t off : neededOffsets)
        _appendDynEntry(result.DynamicSegmentData, DT_NEEDED, off);

    return result;
}

std::uint32_t SysVDynamicSectionBuilder::_sysvHash(const std::string& name) {
    std::uint32_t hash = 0;
    for (const char c : name) {
        hash = (hash << 4) + static_cast<std::uint8_t>(c);
        const std::uint32_t high = hash & 0xf0000000u;
        if (high != 0)
            hash ^= high >> 24;
        hash &= ~high;
    }
    return hash;
}

std::string SysVDynamicSectionBuilder::_readName(const std::vector<std::uint8_t>& strtab, const std::uint32_t offset) {
    if (offset >= strtab.size())
        throw RelinkerException("Dynamic symbol name offset is out of bounds", offset);
    std::string name;
    for (std::size_t pos = offset; pos < strtab.size() && strtab[pos] != 0; ++pos)
        name.push_back(static_cast<char>(strtab[pos]));
    return name;
}

void SysVDynamicSectionBuilder::AppendExportsAndHash(
    SysVDynamicSection& section,
    const std::vector<ExportedSymbol>& exports)
{
    if (section.DynSymData.empty() || section.DynSymData.size() % kSymbolEntrySize != 0)
        throw RelinkerException("Dynamic symbol table must contain the null symbol before exports are appended");

    for (const auto& symbol : exports) {
        if (symbol.Name.empty())
            throw RelinkerException("Exported symbol has an empty name");
        const std::uint32_t nameOff = _appendStr(section.DynStrData, symbol.Name);
        _appendElfSym(section.DynSymData, nameOff, symbol.Info, STV_DEFAULT, kDefinedSymbolSectionIndex, symbol.Value, symbol.Size);
        section.SymbolVersions.push_back({symbol.Version, {}});
    }

    const auto symbolCount = static_cast<std::uint32_t>(section.DynSymData.size() / kSymbolEntrySize);
    std::uint32_t bucketCount = kHashBucketPrimes[0];
    for (const std::uint32_t prime : kHashBucketPrimes)
        if (prime <= symbolCount / 2 + 1)
            bucketCount = prime;

    std::vector<std::uint32_t> buckets(bucketCount, 0);
    std::vector<std::uint32_t> chains(symbolCount, 0);
    for (std::uint32_t index = 1; index < symbolCount; ++index) {
        std::uint32_t nameOff = 0;
        std::memcpy(&nameOff, section.DynSymData.data() + static_cast<std::size_t>(index) * kSymbolEntrySize, 4);
        const std::uint32_t bucket = _sysvHash(_readName(section.DynStrData, nameOff)) % bucketCount;
        chains[index] = buckets[bucket];
        buckets[bucket] = index;
    }

    section.HashData.clear();
    const auto appendU32 = [&section](const std::uint32_t value) {
        const std::size_t pos = section.HashData.size();
        section.HashData.resize(pos + 4);
        std::memcpy(section.HashData.data() + pos, &value, 4);
    };
    appendU32(bucketCount);
    appendU32(symbolCount);
    for (const std::uint32_t value : buckets) appendU32(value);
    for (const std::uint32_t value : chains) appendU32(value);
}

void SysVDynamicSectionBuilder::BuildVersionTables(
    SysVDynamicSection& section,
    const std::string& baseVersionName)
{
    const std::size_t symbolCount = section.DynSymData.size() / kSymbolEntrySize;
    if (section.SymbolVersions.size() != symbolCount)
        throw RelinkerException("Symbol version list does not match the dynamic symbol table");

    const auto appendU16 = [](std::vector<std::uint8_t>& buf, const std::uint16_t value) {
        const std::size_t pos = buf.size();
        buf.resize(pos + 2);
        std::memcpy(buf.data() + pos, &value, 2);
    };
    const auto appendU32 = [](std::vector<std::uint8_t>& buf, const std::uint32_t value) {
        const std::size_t pos = buf.size();
        buf.resize(pos + 4);
        std::memcpy(buf.data() + pos, &value, 4);
    };

    std::vector<std::string> definedNames;
    std::vector<std::string> neededFiles;
    std::vector<std::vector<std::string>> neededNames;
    for (const auto& version : section.SymbolVersions) {
        if (version.Name.empty())
            continue;
        if (version.File.empty()) {
            if (std::find(definedNames.begin(), definedNames.end(), version.Name) == definedNames.end())
                definedNames.push_back(version.Name);
            continue;
        }
        auto file = std::find(neededFiles.begin(), neededFiles.end(), version.File);
        if (file == neededFiles.end()) {
            neededFiles.push_back(version.File);
            neededNames.emplace_back();
            file = neededFiles.end() - 1;
        }
        auto& names = neededNames[static_cast<std::size_t>(file - neededFiles.begin())];
        if (std::find(names.begin(), names.end(), version.Name) == names.end())
            names.push_back(version.Name);
    }

    section.VersymData.clear();
    section.VerdefData.clear();
    section.VerneedData.clear();
    section.VerdefCount = 0;
    section.VerneedCount = 0;
    if (definedNames.empty() && neededFiles.empty())
        return;

    std::map<std::string, std::uint16_t> definedIndex;
    std::map<std::pair<std::string, std::string>, std::uint16_t> neededIndex;
    std::uint16_t nextIndex = static_cast<std::uint16_t>(kVersionIndexGlobal + 1);

    if (!definedNames.empty()) {
        std::vector<std::string> entries = {baseVersionName};
        entries.insert(entries.end(), definedNames.begin(), definedNames.end());
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const bool isBase = index == 0;
            const std::uint16_t versionIndex = isBase ? kVersionIndexGlobal : nextIndex++;
            if (!isBase)
                definedIndex[entries[index]] = versionIndex;
            const bool isLast = index + 1 == entries.size();
            appendU16(section.VerdefData, kVersionRevision);
            appendU16(section.VerdefData, isBase ? kVersionFlagBase : 0);
            appendU16(section.VerdefData, versionIndex);
            appendU16(section.VerdefData, 1);
            appendU32(section.VerdefData, _sysvHash(entries[index]));
            appendU32(section.VerdefData, kVerdefSize);
            appendU32(section.VerdefData, isLast ? 0 : kVerdefSize + kVerdauxSize);
            appendU32(section.VerdefData, _appendStr(section.DynStrData, entries[index]));
            appendU32(section.VerdefData, 0);
        }
        section.VerdefCount = entries.size();
    }

    for (std::size_t fileIndex = 0; fileIndex < neededFiles.size(); ++fileIndex) {
        const auto& names = neededNames[fileIndex];
        const bool isLastFile = fileIndex + 1 == neededFiles.size();
        appendU16(section.VerneedData, kVersionRevision);
        appendU16(section.VerneedData, static_cast<std::uint16_t>(names.size()));
        appendU32(section.VerneedData, _appendStr(section.DynStrData, neededFiles[fileIndex]));
        appendU32(section.VerneedData, kVerneedSize);
        appendU32(section.VerneedData, isLastFile ? 0 : kVerneedSize + kVernauxSize * static_cast<std::uint32_t>(names.size()));
        for (std::size_t nameIndex = 0; nameIndex < names.size(); ++nameIndex) {
            const std::uint16_t versionIndex = nextIndex++;
            neededIndex[{neededFiles[fileIndex], names[nameIndex]}] = versionIndex;
            appendU32(section.VerneedData, _sysvHash(names[nameIndex]));
            appendU16(section.VerneedData, kVersionFlagWeak);
            appendU16(section.VerneedData, versionIndex);
            appendU32(section.VerneedData, _appendStr(section.DynStrData, names[nameIndex]));
            appendU32(section.VerneedData, nameIndex + 1 == names.size() ? 0 : kVernauxSize);
        }
    }
    section.VerneedCount = neededFiles.size();

    for (std::size_t index = 0; index < symbolCount; ++index) {
        const auto& version = section.SymbolVersions[index];
        std::uint16_t value = index == 0 ? 0 : kVersionIndexGlobal;
        if (!version.Name.empty())
            value = version.File.empty() ? definedIndex.at(version.Name) : neededIndex.at({version.File, version.Name});
        appendU16(section.VersymData, value);
    }
}

}
