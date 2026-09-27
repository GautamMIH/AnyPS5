#include <relinker/output/ShimLibraryBuilder.hpp>
#include <relinker/output/SysVDynamicSectionBuilder.hpp>
#include <algorithm>
#include <cstring>

namespace Relinker {

namespace {

constexpr std::int64_t kDtNull = 0;
constexpr std::int64_t kDtHash = 4;
constexpr std::int64_t kDtStrtab = 5;
constexpr std::int64_t kDtSymtab = 6;
constexpr std::int64_t kDtRela = 7;
constexpr std::int64_t kDtRelasz = 8;
constexpr std::int64_t kDtRelaent = 9;
constexpr std::int64_t kDtStrsz = 10;
constexpr std::int64_t kDtSyment = 11;
constexpr std::int64_t kDtSoname = 14;
constexpr std::int64_t kDtRunpath = 29;
constexpr std::int64_t kDtFlags = 30;
constexpr char kRunPath[] = "$ORIGIN";
constexpr std::int64_t kDtVersym = 0x6ffffff0;
constexpr std::int64_t kDtVerdef = 0x6ffffffc;
constexpr std::int64_t kDtVerdefnum = 0x6ffffffd;
constexpr std::int64_t kDtVerneed = 0x6ffffffe;
constexpr std::int64_t kDtVerneednum = 0x6fffffff;
constexpr std::uint64_t kDfSymbolic = 0x2;
constexpr std::uint64_t kDfBindNow = 0x8;
constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPtDynamic = 2;
constexpr std::uint32_t kPtGnuStack = 0x6474e551;
constexpr std::uint32_t kPfX = 1;
constexpr std::uint32_t kPfW = 2;
constexpr std::uint32_t kPfR = 4;
constexpr std::uint64_t kSymbolEntrySize = 24;
constexpr std::uint64_t kRelaEntrySize = 24;
constexpr std::uint8_t kInt3 = 0xcc;

std::uint64_t alignUp(const std::uint64_t value, const std::uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void writeU16(std::vector<std::uint8_t>& buf, const std::size_t offset, const std::uint16_t value) {
    std::memcpy(buf.data() + offset, &value, 2);
}

void writeU32(std::vector<std::uint8_t>& buf, const std::size_t offset, const std::uint32_t value) {
    std::memcpy(buf.data() + offset, &value, 4);
}

void writeU64(std::vector<std::uint8_t>& buf, const std::size_t offset, const std::uint64_t value) {
    std::memcpy(buf.data() + offset, &value, 8);
}

void writeBytes(std::vector<std::uint8_t>& buf, const std::size_t offset, const std::vector<std::uint8_t>& bytes) {
    std::copy(bytes.begin(), bytes.end(), buf.begin() + static_cast<std::ptrdiff_t>(offset));
}

void writeRipRelative(std::vector<std::uint8_t>& buf, const std::size_t offset, const std::uint64_t instructionEnd, const std::uint64_t target) {
    const auto delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(instructionEnd);
    if (delta < INT32_MIN || delta > INT32_MAX)
        throw RelinkerException("Shim stub target is out of rel32 range", target);
    writeU32(buf, offset, static_cast<std::uint32_t>(static_cast<std::int32_t>(delta)));
}

std::vector<std::uint8_t> buildDynamic(const SysVDynamicSection& section, const std::uint64_t soNameOffset, const std::uint64_t dynStrSize, const std::vector<std::uint64_t>& addresses) {
    const std::uint64_t runPathOffset = soNameOffset + std::strlen(reinterpret_cast<const char*>(section.DynStrData.data() + soNameOffset)) + 1;
    std::vector<std::uint8_t> dynamic = section.DynamicSegmentData;
    const auto append = [&dynamic](const std::int64_t tag, const std::uint64_t value) {
        const std::size_t pos = dynamic.size();
        dynamic.resize(pos + 16);
        std::memcpy(dynamic.data() + pos, &tag, 8);
        std::memcpy(dynamic.data() + pos + 8, &value, 8);
    };
    append(kDtSoname, soNameOffset);
    append(kDtHash, addresses[0]);
    append(kDtStrtab, addresses[1]);
    append(kDtStrsz, dynStrSize);
    append(kDtSymtab, addresses[2]);
    append(kDtSyment, kSymbolEntrySize);
    if (!section.RelaData.empty()) {
        append(kDtRela, addresses[3]);
        append(kDtRelasz, section.RelaData.size());
        append(kDtRelaent, kRelaEntrySize);
    }
    if (!section.VersymData.empty())
        append(kDtVersym, addresses[4]);
    if (!section.VerdefData.empty()) {
        append(kDtVerdef, addresses[5]);
        append(kDtVerdefnum, section.VerdefCount);
    }
    if (!section.VerneedData.empty()) {
        append(kDtVerneed, addresses[6]);
        append(kDtVerneednum, section.VerneedCount);
    }
    append(kDtRunpath, runPathOffset);
    append(kDtFlags, kDfBindNow | kDfSymbolic);
    append(kDtNull, 0);
    return dynamic;
}

}

SysVDynamicSection ShimLibraryBuilder::_buildSection(const ShimLibraryRequest& request, const Layout& layout, const std::vector<std::uint64_t>& symbolAddresses, std::uint64_t& soNameOffset) {
    std::vector<NidReference> references;
    const bool needsReporter = std::any_of(request.Symbols.begin(), request.Symbols.end(), [](const ShimSymbol& symbol) { return symbol.Kind == ShimSymbolKind::Report; });
    if (needsReporter) {
        NidReference reporter{request.ReporterName, request.ReporterVersion, kRelocationGlobDat, 0, layout.Got, 0, kGlobalFunction};
        reporter.LibraryFile = request.ReporterFile;
        references.push_back(std::move(reporter));
    }

    SysVDynamicSectionBuilder builder;
    auto section = builder.BuildDynamicSection(references, request.Needed, 0, 0);

    std::vector<ExportedSymbol> exports;
    for (std::size_t index = 0; index < request.Symbols.size(); ++index) {
        const auto& symbol = request.Symbols[index];
        const bool isObject = symbol.Kind == ShimSymbolKind::ZeroObject;
        exports.push_back({symbol.Name, isObject ? kGlobalObject : kGlobalFunction, symbolAddresses[index], isObject ? kZeroObjectSize : 0, symbol.Version});
    }
    builder.AppendExportsAndHash(section, exports);
    builder.BuildVersionTables(section, request.SoName);

    soNameOffset = section.DynStrData.size();
    for (const char c : request.SoName)
        section.DynStrData.push_back(static_cast<std::uint8_t>(c));
    section.DynStrData.push_back(0);
    for (const char c : std::string(kRunPath))
        section.DynStrData.push_back(static_cast<std::uint8_t>(c));
    section.DynStrData.push_back(0);
    return section;
}

std::vector<std::uint8_t> ShimLibraryBuilder::Build(const ShimLibraryRequest& request) const {
    if (request.SoName.empty())
        throw RelinkerException("Shim library requires a name");
    std::size_t reportCount = 0;
    std::size_t objectCount = 0;
    std::uint64_t stringBytes = 0;
    for (const auto& symbol : request.Symbols) {
        if (symbol.Name.empty())
            throw RelinkerException("Shim symbol has an empty name");
        if (symbol.Kind == ShimSymbolKind::Report) {
            ++reportCount;
            stringBytes += symbol.Name.size() + symbol.Library.size() + 2;
        } else {
            ++objectCount;
        }
    }
    if (reportCount != 0 && (request.ReporterName.empty() || request.ReporterFile.empty()))
        throw RelinkerException("Shim reporter stubs require a reporter import");

    std::vector<std::uint64_t> addresses(request.Symbols.size(), 0);
    Layout layout{};
    std::uint64_t soNameOffset = 0;
    const auto sizing = _buildSection(request, layout, addresses, soNameOffset);

    layout.DynStr = kElfHeaderSize + kProgramHeaderCount * kProgramHeaderSize;
    layout.DynSym = alignUp(layout.DynStr + sizing.DynStrData.size(), 8);
    layout.Hash = alignUp(layout.DynSym + sizing.DynSymData.size(), 8);
    layout.Versym = alignUp(layout.Hash + sizing.HashData.size(), 8);
    layout.Verdef = alignUp(layout.Versym + sizing.VersymData.size(), 8);
    layout.Verneed = alignUp(layout.Verdef + sizing.VerdefData.size(), 8);
    layout.Rela = alignUp(layout.Verneed + sizing.VerneedData.size(), 8);
    layout.Text = alignUp(layout.Rela + sizing.RelaData.size(), 16);
    layout.Strings = layout.Text + reportCount * kReportStubSize;
    layout.ReadOnlyEnd = layout.Strings + stringBytes;
    layout.Got = alignUp(layout.ReadOnlyEnd, kPageSize);
    const std::uint64_t gotCount = reportCount != 0 ? 1 : 0;
    layout.Objects = alignUp(layout.Got + gotCount * kGotSlotSize, 16);
    layout.Dynamic = alignUp(layout.Objects + objectCount * kZeroObjectSize, 8);

    std::uint64_t textCursor = layout.Text;
    std::uint64_t objectCursor = layout.Objects;
    for (std::size_t index = 0; index < request.Symbols.size(); ++index) {
        const auto kind = request.Symbols[index].Kind;
        if (kind == ShimSymbolKind::ZeroObject) {
            addresses[index] = objectCursor;
            objectCursor += kZeroObjectSize;
        } else {
            addresses[index] = textCursor;
            textCursor += kReportStubSize;
        }
    }

    const auto section = _buildSection(request, layout, addresses, soNameOffset);
    if (section.DynStrData.size() != sizing.DynStrData.size() || section.DynSymData.size() != sizing.DynSymData.size() || section.RelaData.size() != sizing.RelaData.size())
        throw RelinkerException("Shim library layout changed between passes");

    const std::vector<std::uint64_t> tableAddresses = {layout.Hash, layout.DynStr, layout.DynSym, layout.Rela, layout.Versym, layout.Verdef, layout.Verneed};
    const auto dynamic = buildDynamic(section, soNameOffset, section.DynStrData.size(), tableAddresses);
    layout.End = layout.Dynamic + dynamic.size();

    std::vector<std::uint8_t> image(static_cast<std::size_t>(layout.End), 0);
    const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
    std::copy(std::begin(ident), std::end(ident), image.begin());
    writeU16(image, 0x10, 3);
    writeU16(image, 0x12, 0x3e);
    writeU32(image, 0x14, 1);
    writeU64(image, 0x20, kElfHeaderSize);
    writeU16(image, 0x34, kElfHeaderSize);
    writeU16(image, 0x36, kProgramHeaderSize);
    writeU16(image, 0x38, kProgramHeaderCount);
    writeU16(image, 0x3a, 0x40);

    const auto writeProgramHeader = [&image](const std::size_t index, const std::uint32_t type, const std::uint32_t flags, const std::uint64_t offset, const std::uint64_t size, const std::uint64_t alignment) {
        const std::size_t entry = kElfHeaderSize + index * kProgramHeaderSize;
        writeU32(image, entry, type);
        writeU32(image, entry + 4, flags);
        writeU64(image, entry + 8, offset);
        writeU64(image, entry + 16, offset);
        writeU64(image, entry + 24, offset);
        writeU64(image, entry + 32, size);
        writeU64(image, entry + 40, size);
        writeU64(image, entry + 48, alignment);
    };
    writeProgramHeader(0, kPtLoad, kPfR | kPfX, 0, layout.ReadOnlyEnd, kPageSize);
    writeProgramHeader(1, kPtLoad, kPfR | kPfW, layout.Got, layout.End - layout.Got, kPageSize);
    writeProgramHeader(2, kPtDynamic, kPfR | kPfW, layout.Dynamic, dynamic.size(), 8);
    writeProgramHeader(3, kPtGnuStack, kPfR | kPfW, 0, 0, 16);

    writeBytes(image, layout.DynStr, section.DynStrData);
    writeBytes(image, layout.DynSym, section.DynSymData);
    writeBytes(image, layout.Hash, section.HashData);
    writeBytes(image, layout.Versym, section.VersymData);
    writeBytes(image, layout.Verdef, section.VerdefData);
    writeBytes(image, layout.Verneed, section.VerneedData);
    writeBytes(image, layout.Rela, section.RelaData);
    writeBytes(image, layout.Dynamic, dynamic);

    std::fill(image.begin() + static_cast<std::ptrdiff_t>(layout.Text), image.begin() + static_cast<std::ptrdiff_t>(layout.Strings), kInt3);
    const std::uint64_t reporterSlot = layout.Got;
    std::uint64_t stringCursor = layout.Strings;
    const auto appendString = [&](const std::string& text) {
        const std::uint64_t address = stringCursor;
        std::copy(text.begin(), text.end(), image.begin() + static_cast<std::ptrdiff_t>(address));
        stringCursor += text.size() + 1;
        return address;
    };

    for (std::size_t index = 0; index < request.Symbols.size(); ++index) {
        const auto& symbol = request.Symbols[index];
        const std::uint64_t stub = addresses[index];
        if (symbol.Kind == ShimSymbolKind::Report) {
            const std::uint64_t nameString = appendString(symbol.Name);
            const std::uint64_t libraryString = appendString(symbol.Library);
            const std::uint8_t leaRdi[] = {0x48, 0x8d, 0x3d};
            const std::uint8_t leaRsi[] = {0x48, 0x8d, 0x35};
            std::copy(std::begin(leaRdi), std::end(leaRdi), image.begin() + static_cast<std::ptrdiff_t>(stub));
            writeRipRelative(image, stub + 3, stub + 7, nameString);
            std::copy(std::begin(leaRsi), std::end(leaRsi), image.begin() + static_cast<std::ptrdiff_t>(stub + 7));
            writeRipRelative(image, stub + 10, stub + 14, libraryString);
            image[stub + 14] = 0xff;
            image[stub + 15] = 0x25;
            writeRipRelative(image, stub + 16, stub + 20, reporterSlot);
        }
    }

    return image;
}

}
