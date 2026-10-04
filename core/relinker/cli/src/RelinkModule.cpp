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

}

std::vector<std::string> ReadNeededLibraries(const Domain::SysVDynamicSection& section) {
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

void SetNeededLibraries(Domain::SysVDynamicSection& section, const std::vector<std::string>& libraries) {
    for (std::size_t offset = 0; offset + kDynEntrySize <= section.DynamicSegmentData.size(); offset += kDynEntrySize) {
        std::int64_t tag = 0;
        std::memcpy(&tag, section.DynamicSegmentData.data() + offset, 8);
        if (tag != kDtNeeded)
            throw Domain::RelinkerException("Rebuilt dependency table contains a non-DT_NEEDED entry");
    }
    section.DynamicSegmentData.clear();
    for (const auto& library : libraries) {
        const std::uint64_t nameOffset = section.DynStrData.size();
        for (const char c : library)
            section.DynStrData.push_back(static_cast<std::uint8_t>(c));
        section.DynStrData.push_back(0);
        const std::size_t entry = section.DynamicSegmentData.size();
        section.DynamicSegmentData.resize(entry + kDynEntrySize);
        std::memcpy(section.DynamicSegmentData.data() + entry, &kDtNeeded, 8);
        std::memcpy(section.DynamicSegmentData.data() + entry + 8, &nameOffset, 8);
    }
}

PreparedModule PrepareModule(const std::vector<std::uint8_t>& inputBytes, const Args& args) {
    const Relinker::SelfUnwrapper selfUnwrapper;
    if (selfUnwrapper.IsSelf(inputBytes))
        std::cout << "Input: SELF container; extracting the embedded ELF\n";
    PreparedModule module;
    module.Image = selfUnwrapper.Unwrap(inputBytes);

    auto elfReader = std::make_shared<Relinker::ElfReader>(module.Image);
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
    module.Result = pipeline->Relink(module.Image);
    for (const auto& patch : module.Result.Patches) {
        if (patch.Offset > module.Image.size() || patch.Bytes.size() > module.Image.size() - patch.Offset)
            throw Domain::RelinkerException("Relinker patch exceeds source image", patch.Offset);
        for (std::size_t index = 0; index < patch.Bytes.size(); ++index) module.Image[patch.Offset + index] = patch.Bytes[index];
    }
    return module;
}

ModuleRelinkOutcome EmitModule(PreparedModule& module, const std::string& absOutputPath, const Args& args) {
    Io::FileWriter fileWriter;
    auto& result = module.Result;
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
        // The game's icon (sce_sys/icon0.png beside the input) becomes the executable's icon (upstream).
        patcher = std::make_shared<Elfpatcher::Windows::WindowsPePatcher>(args.windowsGui, std::filesystem::path(args.inputPath).parent_path() / "sce_sys" / "icon0.png");
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

    fileWriter.Write(absOutputPath, patcher->Patch(module.Image, result.OriginalHeaders, result.DynamicSection, result.OriginalPltGotVaddr, runPath, args.lazyBinding, args.windowsDiagnostics, module.Trampolines, result.LinkInfo));
    std::cout << "External prx references: " << result.RegistryEntries.size() << "\nOutput file: " << absOutputPath << '\n';

    return ModuleRelinkOutcome{ReadNeededLibraries(result.DynamicSection), isLibrary};
}

ModuleRelinkOutcome RelinkModule(const std::vector<std::uint8_t>& inputBytes, const std::string& absOutputPath, const Args& args) {
    auto module = PrepareModule(inputBytes, args);
    return EmitModule(module, absOutputPath, args);
}

}
