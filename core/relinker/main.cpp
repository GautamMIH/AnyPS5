#include <Cli.hpp>
#include <domain/Types.hpp>
#include <io/FileReader.hpp>
#include <io/FileWriter.hpp>
#include <relinker/parsing/ElfReader.hpp>
#include <relinker/parsing/SelfUnwrapper.hpp>
#include <codegen/IAmd64OnlyConverter.hpp>
#include <filesystem>
#include <iostream>
#include <string>

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

        Cli::RelinkModule(inputBytes, absPath, args);

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
