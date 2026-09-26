#ifndef RELINKER_DOMAIN_ISYSVDYNAMICSECTIONBUILDER_HPP
#define RELINKER_DOMAIN_ISYSVDYNAMICSECTIONBUILDER_HPP

#include <relinker/domain/Types.hpp>
#include <vector>

namespace Relinker {

class ISysVDynamicSectionBuilder {
public:
    virtual ~ISysVDynamicSectionBuilder() = default;

    virtual SysVDynamicSection BuildDynamicSection(
        const std::vector<NidReference>& nidReferences,
        const std::vector<std::string>& neededLibraries,
        FileByteOffset originalJmprelOffset,
        std::uint32_t originalJmprelCount
    ) = 0;

    virtual void AppendExportsAndHash(
        SysVDynamicSection& section,
        const std::vector<ExportedSymbol>& exports
    ) = 0;

    virtual void BuildVersionTables(
        SysVDynamicSection& section,
        const std::string& baseVersionName
    ) = 0;
};

}

#endif
