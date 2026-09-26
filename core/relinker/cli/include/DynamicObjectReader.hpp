#ifndef CORE_RELINKER_CLI_INCLUDE_DYNAMICOBJECTREADER_HPP
#define CORE_RELINKER_CLI_INCLUDE_DYNAMICOBJECTREADER_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace Cli {

struct DynamicSymbolInfo {
    std::string Name;
    std::uint8_t Info;
    bool Defined;
    std::string Version;
    std::string VersionFile;
};

struct DynamicObjectInfo {
    std::vector<std::string> Needed;
    std::vector<DynamicSymbolInfo> Symbols;
    bool HasVersionDefinitions = false;
};

DynamicObjectInfo ReadDynamicObject(const std::vector<std::uint8_t>& image);

}

#endif
