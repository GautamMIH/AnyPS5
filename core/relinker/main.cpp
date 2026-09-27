#include <Cli.hpp>
#include <domain/Types.hpp>
#include <io/FileReader.hpp>
#include <io/FileWriter.hpp>
#include <relinker/parsing/ElfReader.hpp>
#include <relinker/parsing/SelfUnwrapper.hpp>
#include <relinker/analysis/SyscallScanner.hpp>
#include <relinker/guest/GuestImage.hpp>
#include <codegen/IAmd64OnlyConverter.hpp>
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

        Io::FileReader fileReader;
        Io::FileWriter fileWriter;

        auto inputBytes = fileReader.Read(args.inputPath);
        const std::string absPath = std::filesystem::absolute(args.outputPath).string();

        if (args.toIntel) {
            std::cout << "Mode: Intel instruction conversion; system unchanged; unused-filter=" << args.unusedFilterLevel << " (not applied)\n";

            auto sourceBytes = Relinker::SelfUnwrapper().Unwrap(inputBytes);
            const Relinker::ElfReader elfReader(sourceBytes);
            const auto converter = Codegen::MakeAmd64OnlyConverter();

            auto codeSegments = elfReader.ReadCodeSegments();
            auto result = converter->Convert(std::move(sourceBytes), codeSegments);

            std::cout << "OK: " << result.ReplacedCount << " instructions replaced\n";
            // The converted image continues through the general relink pipeline.
            inputBytes = std::move(result.Bytes);
        }

        // A single executable: modules shipped beside it (sce_module/) are relinked as guest
        // modules it loads first. --game mode relinks a whole dump, modules included.
        auto module = Cli::PrepareModule(inputBytes, args);
        std::cout << "sce_module/sce_modules processing: " << (args.skipSceModule ? "disabled (--skip-sce-module)" : "enabled") << '\n';
        std::vector<Relinker::GuestArtifact> guestArtifacts;
        if (!args.skipSceModule) {
            const auto syscallScanner = args.skipSyscallCheck ? Relinker::MakeNullSyscallScanner() : Relinker::MakeSyscallScanner();
            guestArtifacts = Relinker::GuestModuleBuilder().Build(args.inputPath, absPath, module.Result.DynamicSection, args.toWindows, args.toIntel, *syscallScanner, args.lazyBinding, args.runPath);
        }
        Cli::EmitModule(module, absPath, args);
        for (const auto& artifact : guestArtifacts) {
            std::filesystem::create_directories(artifact.Path.parent_path());
            fileWriter.Write(artifact.Path.string(), artifact.Bytes);
            std::cout << "Guest module: " << artifact.Path.string() << '\n';
        }

        if (args.autorun) return Cli::Autorun(absPath, args.toWindows);

    } catch (const Domain::RelinkerException& e) {
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
