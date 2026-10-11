#include <Cli.hpp>
#include <domain/Types.hpp>
#include <io/FileReader.hpp>
#include <io/FileWriter.hpp>
#include <io/NativePath.hpp>
#include <relinker/parsing/ElfReader.hpp>
#include <relinker/parsing/SelfUnwrapper.hpp>
#include <relinker/analysis/SyscallScanner.hpp>
#include <relinker/guest/GuestImage.hpp>
#include <relinker/output/CallRegistryWriter.hpp>
#include <codegen/IAmd64OnlyConverter.hpp>
#include <map>
#include <codegen/CodegenException.hpp>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <cwchar>
#endif

namespace {

int Run(const int argc, char* argv[]) {
    Cli::Args args;
    try {
        args = Cli::ParseArgs(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }

    if (args.showHelp) {
        std::cout << Cli::Usage() << '\n';
        return 0;
    }

    try {
        if (args.gameMode)
            return Cli::RelinkGame(args, argc > 0 ? argv[0] : "");

        auto extension = Io::Utf8Path(Io::NativePath(args.outputPath).extension());
        for (auto& character : extension) if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
        if (!args.toWindows && !args.toMacos && extension == ".exe") std::cerr << "WARNING: Output filename ends with .exe, but --windows was not specified. The output will be a Linux ELF executable.\n";
        Io::FileReader fileReader;
        Io::FileWriter fileWriter;

        auto inputBytes = fileReader.Read(args.inputPath);
        if (std::filesystem::exists(Io::NativePath(args.outputPath)) && std::filesystem::equivalent(Io::NativePath(args.inputPath), Io::NativePath(args.outputPath)))
            throw Domain::RelinkerException("Executable output would overwrite the input executable");
        const std::string absPath = Io::Utf8Path(std::filesystem::absolute(Io::NativePath(args.outputPath)));

        std::vector<Codegen::TrampolineSite> trampolines;
        if (args.toIntel) {
            auto sourceBytes = Relinker::SelfUnwrapper().Unwrap(inputBytes);
            const auto codeSegments = Relinker::ElfReader(sourceBytes).ReadCodeSegments();
            auto converted = Codegen::MakeAmd64OnlyConverter(args.toRosetta ? Codegen::Amd64OnlyTarget::Rosetta : Codegen::Amd64OnlyTarget::Intel)->Convert(std::move(sourceBytes), codeSegments);
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
        std::cout << Relinker::GuestModulePattern << " processing: " << (args.skipSceModule ? "disabled (--skip-sce-module)" : "enabled") << '\n';
        for (const auto& name : args.excludedSceModules) std::cout << "Guest module excluded: " << name << '\n';
        std::vector<Relinker::GuestArtifact> guestArtifacts;
        if (!args.skipSceModule) {
            const auto syscallScanner = args.skipSyscallCheck ? Relinker::MakeNullSyscallScanner() : Relinker::MakeSyscallScanner();
            guestArtifacts = Relinker::GuestModuleBuilder().Build(Io::NativePath(args.inputPath), Io::NativePath(absPath), module.Result.DynamicSection, args.toWindows, args.toMacos, args.toIntel, args.toRosetta, *syscallScanner, args.lazyBinding, args.runPath, args.excludedSceModules, Io::NativePath(args.sceModulePath));
        }
        if (args.writeRegistry) {
            // Each converted guest module gets its imports registry beside the executable's (upstream bd750b59).
            const std::filesystem::path outFsPath = Io::NativePath(absPath);
            const auto registryWriter = std::make_shared<Relinker::CallRegistryWriter>();
            for (const auto& artifact : guestArtifacts) {
                const auto modulePath = outFsPath.parent_path() / Io::NativePath(Io::Utf8Path(outFsPath.stem()) + "." + Io::Utf8Path(artifact.Path.filename()) + ".registry.json");
                fileWriter.Write(Io::Utf8Path(modulePath), registryWriter->WriteModuleImports(artifact.Imports));
            }
        }
        module.Trampolines = std::move(trampolines);
        Cli::EmitModule(module, absPath, args);
        for (const auto& artifact : guestArtifacts) {
            std::filesystem::create_directories(artifact.Path.parent_path());
            fileWriter.Write(Io::Utf8Path(artifact.Path), artifact.Bytes);
            std::cout << "Guest module: " << Io::Utf8Path(artifact.Path) << '\n';
        }
        std::cout << "Expected runtime layout (relative to the output executable):\n"
                  << Io::Utf8Path(Io::NativePath(absPath).filename()) << "\n"
                  << "libs/\n    *.prx\napp0/\n    <game resources>\n";
        for (const auto& artifact : guestArtifacts) {
            std::string relative = Io::Utf8Path(artifact.Path.lexically_relative(Io::NativePath(absPath).parent_path() / "app0"));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            std::cout << "    " << relative << '\n';
        }
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

#ifdef _WIN32

std::string Utf8Argument(const wchar_t* argument) {
    const std::size_t argumentSize = std::wcslen(argument);
    if (argumentSize == 0) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, static_cast<int>(argumentSize), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("Cannot encode the command line as UTF-8");
    std::string utf8(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, static_cast<int>(argumentSize), utf8.data(), size, nullptr, nullptr) != size)
        throw std::runtime_error("Cannot encode the command line as UTF-8");
    return utf8;
}

}

int wmain(const int argc, wchar_t* argv[]) {
    try {
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<std::size_t>(argc));
        arguments.emplace_back();
        for (int index = 1; index < argc; ++index) arguments.push_back(Utf8Argument(argv[index]));
        std::vector<char*> pointers;
        pointers.reserve(arguments.size() + 1);
        for (auto& argument : arguments) pointers.push_back(argument.data());
        pointers.push_back(nullptr);
        return Run(static_cast<int>(arguments.size()), pointers.data());
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

#else

}

int main(const int argc, char* argv[]) {
    return Run(argc, argv);
}

#endif
