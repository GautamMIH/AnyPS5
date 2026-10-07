#include <Cli.hpp>
#include <domain/Types.hpp>
#include <io/FileReader.hpp>
#include <io/FileWriter.hpp>
#include <relinker/parsing/ElfReader.hpp>
#include <relinker/parsing/SelfUnwrapper.hpp>
#include <relinker/analysis/SyscallScanner.hpp>
#include <relinker/guest/GuestImage.hpp>
#include <codegen/IAmd64OnlyConverter.hpp>
#include <map>
#include <codegen/CodegenException.hpp>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main(const int argc, char* argv[]) {
    Cli::Args args;
    try {
        args = Cli::ParseArgs(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }

    try {
        if (args.gameMode)
            return Cli::RelinkGame(args, argc > 0 ? argv[0] : "");

        auto extension = std::filesystem::path(args.outputPath).extension().string();
        for (auto& character : extension) if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
        if (!args.toWindows && extension == ".exe") std::cerr << "WARNING: Output filename ends with .exe, but --windows was not specified. The output will be a Linux ELF executable.\n";
        Io::FileReader fileReader;
        Io::FileWriter fileWriter;

        auto inputBytes = fileReader.Read(args.inputPath);
        const std::string absPath = std::filesystem::absolute(args.outputPath).string();

        std::vector<Codegen::TrampolineSite> trampolines;
        if (args.toIntel) {
            auto sourceBytes = Relinker::SelfUnwrapper().Unwrap(inputBytes);
            const auto codeSegments = Relinker::ElfReader(sourceBytes).ReadCodeSegments();
            auto converted = Codegen::MakeAmd64OnlyConverter()->Convert(std::move(sourceBytes), codeSegments);
            inputBytes = std::move(converted.Bytes);
            trampolines = std::move(converted.Trampolines);
            std::map<std::string, std::size_t> stubsByName;
            for (const auto& report : converted.Reports) {
                if (report.Lowering == Codegen::Amd64OnlyLowering::Kept)
                    std::cout << "Intel substitution: " << report.InstructionName << " at 0x" << std::hex << report.Offset << std::dec << " (" << report.OriginalLength << " bytes) kept: no room for a jump\n";
                else if (report.InstructionName == "VRSQRTPS" || report.InstructionName == "VRCPPS")
                    ++stubsByName[report.InstructionName];
                else
                    std::cout << "Intel substitution: " << report.InstructionName << " at 0x" << std::hex << report.Offset << std::dec << " (" << report.OriginalLength << " bytes) -> " << (report.Lowering == Codegen::Amd64OnlyLowering::InPlace ? "in place " : "stub ") << report.ReplacementLength << " bytes\n";
            }
            for (const auto& [name, count] : stubsByName)
                std::cout << "Intel substitution: " << name << " -> stub at " << count << " sites\n";
            std::cout << "Intel conversion: " << converted.ReplacedCount << " in place, " << trampolines.size() << " stubs, " << converted.KeptCount << " kept\n";
        }

        // A single executable: modules shipped beside it (sce_module/) are relinked as guest
        // modules it loads first. --game mode relinks a whole dump, modules included.
        auto module = Cli::PrepareModule(inputBytes, args);
        std::cout << "sce_module/sce_modules/prx processing: " << (args.skipSceModule ? "disabled (--skip-sce-module)" : "enabled") << '\n';
        for (const auto& name : args.excludedSceModules) std::cout << "Guest module excluded: " << name << '\n';
        std::vector<Relinker::GuestArtifact> guestArtifacts;
        if (!args.skipSceModule) {
            const auto syscallScanner = args.skipSyscallCheck ? Relinker::MakeNullSyscallScanner() : Relinker::MakeSyscallScanner();
            guestArtifacts = Relinker::GuestModuleBuilder().Build(args.inputPath, absPath, module.Result.DynamicSection, args.toWindows, args.toIntel, *syscallScanner, args.lazyBinding, args.runPath, args.excludedSceModules);
        }
        module.Trampolines = std::move(trampolines);
        Cli::EmitModule(module, absPath, args);
        for (const auto& artifact : guestArtifacts) {
            std::filesystem::create_directories(artifact.Path.parent_path());
            fileWriter.Write(artifact.Path.string(), artifact.Bytes);
            std::cout << "Guest module: " << artifact.Path.string() << '\n';
        }
        std::cout << "Expected runtime layout (relative to the output executable):\n"
                  << std::filesystem::path(absPath).filename().string() << "\n"
                  << "libs/\n    *.prx\napp0/\n    <game resources>\n";
        for (const auto& artifact : guestArtifacts)
            std::cout << "    " << artifact.Path.lexically_relative(std::filesystem::path(absPath).parent_path() / "app0").generic_string() << '\n';
        std::cout << "Game resources and system libraries must be placed in this layout separately.\n";
        if (args.runPath != "$ORIGIN/libs") std::cout << "Custom library search path (--rpath): " << args.runPath << '\n';

        if (args.autorun) return Cli::Autorun(absPath, args.toWindows);

    } catch (const Domain::RelinkerException& e) {
        std::cerr << "FAIL: " << e.what();
        if (e.FailureOffset != 0) std::cerr << " (offset 0x" << std::hex << e.FailureOffset << ")";
        std::cerr << "\n";
        if (!e.InputPath.empty()) std::cerr << "Input: " << e.InputPath << '\n';
        return 2;
    } catch (const Codegen::CodegenException& e) {
        std::cerr << "FAIL: " << e.what();
        if (e.FailureOffset != 0) std::cerr << " (offset 0x" << std::hex << e.FailureOffset << ")";
        std::cerr << "\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 2;
    }

    return 0;
}
