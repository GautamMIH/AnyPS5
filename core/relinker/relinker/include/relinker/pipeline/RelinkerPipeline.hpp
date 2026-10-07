#ifndef RELINKER_PIPELINE_RELINKERPIPELINE_HPP
#define RELINKER_PIPELINE_RELINKERPIPELINE_HPP

#include <relinker/domain/IRelinkerPipeline.hpp>
#include <relinker/domain/IElfReader.hpp>
#include <relinker/domain/ISyscallScanner.hpp>
#include <relinker/domain/ICallSiteResolver.hpp>
#include <relinker/domain/IValidationPolicy.hpp>
#include <relinker/domain/ISysVDynamicSectionBuilder.hpp>
#include <relinker/domain/IUnusedNidFilter.hpp>
#include <memory>

namespace Relinker {

class RelinkerPipeline : public IRelinkerPipeline {
public:
    RelinkerPipeline(std::shared_ptr<IElfReader> elfReader, std::shared_ptr<ISyscallScanner> syscallScanner, std::shared_ptr<ICallSiteResolver> callSiteResolver, std::shared_ptr<IValidationPolicy> validationPolicy, std::shared_ptr<ISysVDynamicSectionBuilder> dynamicSectionBuilder, std::shared_ptr<IUnusedNidFilter> unusedNidFilter, std::uint32_t unusedFilterLevel);

    RelinkResult Relink(const std::vector<std::uint8_t>& sourceElf) override;

private:
    std::shared_ptr<IElfReader> _elfReader;
    std::shared_ptr<ISyscallScanner> _syscallScanner;
    std::shared_ptr<ICallSiteResolver> _callSiteResolver;
    std::shared_ptr<IValidationPolicy> _validationPolicy;
    std::shared_ptr<ISysVDynamicSectionBuilder> _dynamicSectionBuilder;
    std::shared_ptr<IUnusedNidFilter> _unusedNidFilter;
    std::uint32_t unusedFilterLevel;

    // Standard ELF constants come from elfpatcher/general/ElfConstants.hpp.
    static constexpr std::uint16_t ET_SCE_DYNAMIC = 0xfe18;
    static constexpr std::uint32_t R_X86_64_DTPMOD64 = 0x10;
    static constexpr std::uint32_t R_X86_64_DTPOFF64 = 0x11;
    static constexpr std::uint32_t R_X86_64_TPOFF64 = 0x12;
    static constexpr std::uint8_t STB_LOCAL = 0;

    static constexpr std::int64_t DT_SCE_NEEDED_MODULE_PS4 = 0x6100000f;
    static constexpr std::int64_t DT_SCE_EXPORT_LIB_PS4 = 0x61000013;
    static constexpr std::int64_t DT_SCE_IMPORT_LIB_PS4 = 0x61000015;
    static constexpr std::int64_t DT_SCE_NEEDED_MODULE_PS5 = 0x61000045;
    static constexpr std::int64_t DT_SCE_EXPORT_LIB_PS5 = 0x61000047;
    static constexpr std::int64_t DT_SCE_IMPORT_LIB_PS5 = 0x61000049;
    static constexpr std::uint32_t kSceTableIdShift = 48;
    static constexpr std::uint64_t kSceTableNameMask = 0xffffffff;
    static constexpr char kDefaultBaseVersionName[] = "module";

    static bool _isTlsRelocation(std::uint32_t type);
    static bool _decodeSceIndex(const std::string& text, std::uint64_t& value);

    static std::string _relocationTypeName(std::uint32_t type);
};

}

#endif
