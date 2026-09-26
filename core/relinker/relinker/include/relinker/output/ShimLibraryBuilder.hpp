#ifndef RELINKER_OUTPUT_SHIMLIBRARYBUILDER_HPP
#define RELINKER_OUTPUT_SHIMLIBRARYBUILDER_HPP

#include <relinker/domain/Types.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Relinker {

enum class ShimSymbolKind {
    Report,
    ZeroObject
};

struct ShimSymbol {
    ShimSymbolKind Kind;
    std::string Name;
    std::string Version;
    std::string Library;
};

struct ShimLibraryRequest {
    std::string SoName;
    std::vector<std::string> Needed;
    std::vector<ShimSymbol> Symbols;
    std::string ReporterName;
    std::string ReporterVersion;
    std::string ReporterFile;
};

class IShimLibraryBuilder {
public:
    virtual ~IShimLibraryBuilder() = default;

    [[nodiscard]] virtual std::vector<std::uint8_t> Build(const ShimLibraryRequest& request) const = 0;
};

class ShimLibraryBuilder : public IShimLibraryBuilder {
public:
    [[nodiscard]] std::vector<std::uint8_t> Build(const ShimLibraryRequest& request) const override;

private:
    static constexpr std::uint64_t kPageSize = 0x1000;
    static constexpr std::size_t kElfHeaderSize = 0x40;
    static constexpr std::size_t kProgramHeaderSize = 0x38;
    static constexpr std::uint16_t kProgramHeaderCount = 4;
    static constexpr std::size_t kReportStubSize = 32;
    static constexpr std::size_t kZeroObjectSize = 256;
    static constexpr std::size_t kGotSlotSize = 8;
    static constexpr std::uint8_t kGlobalFunction = 0x12;
    static constexpr std::uint8_t kGlobalObject = 0x11;
    static constexpr std::uint32_t kRelocationGlobDat = 6;

    struct Layout {
        std::uint64_t DynStr;
        std::uint64_t DynSym;
        std::uint64_t Hash;
        std::uint64_t Versym;
        std::uint64_t Verdef;
        std::uint64_t Verneed;
        std::uint64_t Rela;
        std::uint64_t Text;
        std::uint64_t Strings;
        std::uint64_t ReadOnlyEnd;
        std::uint64_t Got;
        std::uint64_t Objects;
        std::uint64_t Dynamic;
        std::uint64_t End;
    };

    [[nodiscard]] static SysVDynamicSection _buildSection(const ShimLibraryRequest& request, const Layout& layout, const std::vector<std::uint64_t>& symbolAddresses, std::uint64_t& soNameOffset);
};

}

#endif
