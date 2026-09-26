#include <Cli.hpp>
#include <domain/Types.hpp>
#include <io/FileWriter.hpp>
#include <io/ByteWriter.hpp>
#include <elfpatcher/linux/LinuxElfPatcher.hpp>
#include <elfpatcher/general/SegmentFilter.hpp>
#include <elfpatcher/general/EntryStubBuilder.hpp>
#include <elfpatcher/general/ProgramHeaderLayoutBuilder.hpp>
#include <elfpatcher/general/SectionHeaderTableBuilder.hpp>
#include <elfpatcher/windows/WindowsElfPatcher.hpp>
#include <relinker/parsing/ElfReader.hpp>
#include <relinker/parsing/SelfUnwrapper.hpp>
#include <relinker/analysis/ValidationPolicy.hpp>
#include <relinker/analysis/SyscallScanner.hpp>
#include <relinker/analysis/CallSiteResolver.hpp>
#include <relinker/analysis/UnusedNidFilter.hpp>
#include <relinker/output/SysVDynamicSectionBuilder.hpp>
#include <relinker/output/CallRegistryWriter.hpp>
#include <relinker/pipeline/RelinkerPipeline.hpp>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>

namespace Cli {

namespace {

constexpr std::int64_t kDtNeeded = 1;
constexpr std::size_t kDynEntrySize = 16;
constexpr char kLibraryRunPath[] = "$ORIGIN";

std::vector<std::string> readNeededLibraries(const Domain::SysVDynamicSection& section) {
    std::vector<std::string> result;
    for (std::size_t offset = 0; offset + kDynEntrySize <= section.DynamicSegmentData.size(); offset += kDynEntrySize) {
        std::int64_t tag = 0;
        std::uint64_t value = 0;
        std::memcpy(&tag, section.DynamicSegmentData.data() + offset, 8);
        std::memcpy(&value, section.DynamicSegmentData.data() + offset + 8, 8);
        if (tag != kDtNeeded)
            continue;
        std::string name;
        for (std::size_t pos = value; pos < section.DynStrData.size() && section.DynStrData[pos] != 0; ++pos)
            name.push_back(static_cast<char>(section.DynStrData[pos]));
        result.push_back(std::move(name));
    }
    return result;
}

}

ModuleRelinkOutcome RelinkModule(const std::vector<std::uint8_t>& inputBytes, const std::string& absOutputPath, const Args& args) {
    const Relinker::SelfUnwrapper selfUnwrapper;
    if (selfUnwrapper.IsSelf(inputBytes))
        std::cout << "Input: SELF container; extracting the embedded ELF\n";
    auto sourceBytes = selfUnwrapper.Unwrap(inputBytes);

    Io::FileWriter fileWriter;
    auto elfReader = std::make_shared<Relinker::ElfReader>(sourceBytes);

    const auto pipeline = std::make_shared<Relinker::RelinkerPipeline>(
        elfReader,
        args.skipSyscallCheck ? Relinker::MakeNullSyscallScanner() : Relinker::MakeSyscallScanner(),
        Relinker::MakeCallSiteResolver(),
        std::make_shared<Relinker::ValidationPolicy>(),
        std::make_shared<Relinker::SysVDynamicSectionBuilder>(),
        args.unusedFilterLevel == 2 ? Relinker::MakeStrictUnusedNidFilter() : Relinker::MakeUnusedNidFilter(),
        args.unusedFilterLevel
    );

    std::cout << "System: " << (args.toWindows ? "Windows" : "Linux") << "; unused-filter=" << args.unusedFilterLevel << "\n";
    auto result = pipeline->Relink(sourceBytes);
    for (const auto& patch : result.Patches) {
        if (patch.Offset > sourceBytes.size() || patch.Bytes.size() > sourceBytes.size() - patch.Offset)
            throw Domain::RelinkerException("Relinker patch exceeds source image", patch.Offset);
        for (std::size_t index = 0; index < patch.Bytes.size(); ++index) sourceBytes[patch.Offset + index] = patch.Bytes[index];
    }

    const std::filesystem::path outFsPath(absOutputPath);
    if (args.writeRegistry) {
        const std::string registryPath = (outFsPath.parent_path() / (outFsPath.stem().string() + ".registry.json")).string();
        fileWriter.Write(registryPath, std::make_shared<Relinker::CallRegistryWriter>()->WriteCallRegistry(result.RegistryEntries));
    }

    const bool isLibrary = result.LinkInfo.Kind == Domain::ModuleKind::Library;
    if (isLibrary)
        result.LinkInfo.SoName = outFsPath.filename().string();
    const std::string runPath = isLibrary && !args.runPathSpecified ? std::string(kLibraryRunPath) : args.runPath;

    auto byteWriter = std::make_shared<Io::ByteWriter>();
    std::shared_ptr<Elfpatcher::IElfPatcher> patcher;
    if (args.toWindows) {
        patcher = std::make_shared<Elfpatcher::Windows::WindowsPePatcher>();
    } else {
        patcher = std::make_shared<Elfpatcher::Linux::LinuxElfPatcher>(
            std::make_shared<Elfpatcher::EntryStubBuilder>(),
            std::make_shared<Elfpatcher::ProgramHeaderLayoutBuilder>(
                std::make_shared<Elfpatcher::SegmentFilter>(),
                byteWriter
            ),
            std::make_shared<Elfpatcher::SectionHeaderTableBuilder>(byteWriter),
            byteWriter
        );
    }

    fileWriter.Write(absOutputPath, patcher->Patch(sourceBytes, result.OriginalHeaders, result.DynamicSection, result.OriginalPltGotVaddr, runPath, args.lazyBinding, args.windowsDiagnostics, result.LinkInfo));
    std::cout << "External prx references: " << result.RegistryEntries.size() << "\nOutput file: " << absOutputPath << '\n';

    return ModuleRelinkOutcome{readNeededLibraries(result.DynamicSection), isLibrary};
}

}
