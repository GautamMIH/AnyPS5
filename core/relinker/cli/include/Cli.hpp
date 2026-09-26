#ifndef CORE_RELINKER_CLI_INCLUDE_CLI_HPP
#define CORE_RELINKER_CLI_INCLUDE_CLI_HPP

#include <string>
#include <cstdint>
#include <vector>

namespace Cli {

struct Args {
    bool skipSyscallCheck = false;
    bool toIntel = false;
    bool writeRegistry = false;
    bool toWindows = false;
    bool lazyBinding = false;
    bool autorun = false;
    bool windowsDiagnostics = false;
    bool gameMode = false;
    bool runPathSpecified = false;
    std::uint32_t unusedFilterLevel = 0;
    std::string inputPath;
    std::string outputPath;
    std::string runPath = "$ORIGIN/libs";
    std::string libsPath;
};

struct ModuleRelinkOutcome {
    std::vector<std::string> NeededLibraries;
    bool IsLibrary = false;
};

Args ParseArgs(int argc, char* argv[]);

int Autorun(const std::string& absPath, bool toWindows);

ModuleRelinkOutcome RelinkModule(const std::vector<std::uint8_t>& inputBytes, const std::string& absOutputPath, const Args& args);

int RelinkGame(const Args& args, const std::string& executablePath);

}

#endif
