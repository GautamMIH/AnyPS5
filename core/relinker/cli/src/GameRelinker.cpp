#include <Cli.hpp>
#include <DynamicObjectReader.hpp>
#include <domain/Types.hpp>
#include <io/FileReader.hpp>
#include <io/FileWriter.hpp>
#include <relinker/output/ShimLibraryBuilder.hpp>
#include <relinker/output/SysVDynamicSectionBuilder.hpp>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>

namespace Cli {

namespace {

namespace fs = std::filesystem;

constexpr char kSystemDirectoryName[] = "sce_sys";
constexpr char kGameExecutableName[] = "eboot.bin";
constexpr char kOutputExecutableName[] = "eboot.elf";
constexpr char kLibsDirectoryName[] = "libs";
constexpr char kGameMountName[] = "app0";
constexpr char kModuleExtension[] = ".prx";
constexpr char kSignedModuleExtension[] = ".sprx";
constexpr char kNativeSuffix[] = ".native";
constexpr char kProvidedProbeName[] = "libkernel.prx";
constexpr char kUnresolvedLibraryName[] = "aps5_unresolved.prx";
constexpr char kReportFileName[] = "unresolved.txt";
constexpr char kReporterSymbol[] = "UnresolvedImport_nid_no_patch";
constexpr char kReporterFile[] = "libc.prx";
constexpr char kDefaultBaseVersion[] = "module";
constexpr std::uint8_t kSymbolBindingWeak = 2;
constexpr std::uint8_t kSymbolTypeObject = 1;
constexpr std::size_t kSymbolEntrySize = 24;
constexpr int kExitCompletedWithProblems = 3;

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool isModuleFile(const fs::path& path) {
    const std::string extension = toLower(path.extension().string());
    return extension == kModuleExtension || extension == kSignedModuleExtension;
}

std::string moduleFileName(const fs::path& path) {
    return path.stem().string() + kModuleExtension;
}

bool isWithin(const fs::path& path, const fs::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

fs::path findProvidedLibraries(const std::string& executablePath) {
    std::vector<fs::path> executableDirectories;
    std::error_code error;
    const fs::path selfPath = fs::read_symlink("/proc/self/exe", error);
    if (!error)
        executableDirectories.push_back(selfPath.parent_path());
    if (!executablePath.empty())
        executableDirectories.push_back(fs::absolute(executablePath).parent_path());

    for (const auto& directory : executableDirectories) {
        for (const auto& candidate : {directory / ".." / "libs" / "libs", directory / "libs"}) {
            if (fs::is_regular_file(candidate / kProvidedProbeName, error))
                return fs::weakly_canonical(candidate);
        }
    }
    throw std::runtime_error("Cannot locate the AnyPS5 prx libraries next to the relinker; build the 'libs' target or pass --libs <dir>");
}

std::vector<std::uint8_t> readFile(const fs::path& path) {
    Io::FileReader reader;
    return reader.Read(path.string());
}

std::size_t pathDepth(const fs::path& path) {
    return static_cast<std::size_t>(std::distance(path.begin(), path.end()));
}

std::string aliasCandidate(const std::string& file) {
    const std::string stem = file.substr(0, file.size() - (sizeof(kModuleExtension) - 1));
    const std::string native = kNativeSuffix;
    if (stem.size() > native.size() && stem.compare(stem.size() - native.size(), native.size(), native) == 0)
        return stem.substr(0, stem.size() - native.size()) + kModuleExtension;
    return stem + native + kModuleExtension;
}

// Dumps place stand-ins for system libraries in fakelib/; they are never the real implementation.
bool isPlaceholderModule(const fs::path& relative) {
    for (const auto& part : relative)
        if (toLower(part.string()) == "fakelib")
            return true;
    return false;
}

// PS5 system libraries (the ones AnyPS5 reimplements), as opposed to middleware a game ships.
bool isSystemLibrary(const std::string& file) {
    const std::string name = toLower(file);
    return name.rfind("libsce", 0) == 0 || name.rfind("libkernel", 0) == 0 || name == "libc.prx";
}

struct UnresolvedUse {
    bool IsObject = false;
    std::set<std::string> Users;
};

struct SectionSymbol {
    std::size_t Index;
    std::string Name;
    std::uint8_t Info;
    bool Defined;
};

std::vector<SectionSymbol> listSymbols(const Domain::SysVDynamicSection& section) {
    std::vector<SectionSymbol> symbols;
    for (std::size_t index = 1; (index + 1) * kSymbolEntrySize <= section.DynSymData.size(); ++index) {
        const std::size_t entry = index * kSymbolEntrySize;
        std::uint32_t nameOffset = 0;
        std::uint16_t sectionIndex = 0;
        std::memcpy(&nameOffset, section.DynSymData.data() + entry, 4);
        std::memcpy(&sectionIndex, section.DynSymData.data() + entry + 6, 2);
        std::string name;
        for (std::size_t pos = nameOffset; pos < section.DynStrData.size() && section.DynStrData[pos] != 0; ++pos)
            name.push_back(static_cast<char>(section.DynStrData[pos]));
        symbols.push_back({index, std::move(name), section.DynSymData[entry + 4], sectionIndex != 0});
    }
    return symbols;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

}

int RelinkGame(const Args& args, const std::string& executablePath) {
    const fs::path gameDirectory = fs::weakly_canonical(fs::absolute(args.inputPath));
    const fs::path outputDirectory = fs::weakly_canonical(fs::absolute(args.outputPath));
    const fs::path gameExecutable = gameDirectory / kGameExecutableName;
    if (!fs::is_regular_file(gameExecutable))
        throw std::runtime_error("Game dump has no " + std::string(kGameExecutableName) + ": " + gameDirectory.string());
    if (outputDirectory == gameDirectory || isWithin(outputDirectory, gameDirectory))
        throw std::runtime_error("The output directory must be outside the game dump");

    const fs::path libsDirectory = args.libsPath.empty() ? findProvidedLibraries(executablePath) : fs::weakly_canonical(fs::absolute(args.libsPath));
    if (!fs::is_directory(libsDirectory))
        throw std::runtime_error("AnyPS5 prx library directory does not exist: " + libsDirectory.string());

    const fs::path outputLibs = outputDirectory / kLibsDirectoryName;
    if (fs::exists(outputLibs))
        fs::remove_all(outputLibs);
    fs::create_directories(outputLibs);

    std::set<std::string> providedLibraries;
    for (const auto& entry : fs::directory_iterator(libsDirectory)) {
        if (!entry.is_regular_file() || entry.path().extension() != kModuleExtension)
            continue;
        const std::string name = entry.path().filename().string();
        providedLibraries.insert(name);
        fs::copy_file(entry.path(), outputLibs / name, fs::copy_options::overwrite_existing);
    }
    if (providedLibraries.empty())
        throw std::runtime_error("No AnyPS5 prx libraries found in " + libsDirectory.string());
    std::cout << "AnyPS5 libraries: " << providedLibraries.size() << " from " << libsDirectory.string() << "\n";

    std::map<std::string, std::vector<fs::path>> gameModules;
    for (auto it = fs::recursive_directory_iterator(gameDirectory, fs::directory_options::skip_permission_denied); it != fs::recursive_directory_iterator(); ++it) {
        const fs::path& path = it->path();
        if (it->is_directory()) {
            if (path.filename() == kSystemDirectoryName || path == libsDirectory)
                it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file() && isModuleFile(path))
            gameModules[moduleFileName(path)].push_back(path);
    }

    Args moduleArgs = args;
    moduleArgs.gameMode = false;
    moduleArgs.libsPath.clear();

    std::map<std::string, PreparedModule> prepared;
    std::map<std::string, std::string> preparedSources;
    std::vector<std::string> failures;
    std::vector<std::string> overridden;
    std::vector<std::string> preferredGameCopies;
    // Game modules that AnyPS5 also provides: the side that resolves more of the game's imports wins.
    std::map<std::string, PreparedModule> contested;
    std::map<std::string, std::string> contestedSources;

    for (auto& [name, paths] : gameModules) {
        std::sort(paths.begin(), paths.end(), [](const fs::path& left, const fs::path& right) {
            const std::size_t leftDepth = pathDepth(left);
            const std::size_t rightDepth = pathDepth(right);
            return leftDepth != rightDepth ? leftDepth < rightDepth : left.string() < right.string();
        });
        const fs::path& chosen = paths.front();
        const std::string relative = chosen.lexically_relative(gameDirectory).string();

        const bool provided = providedLibraries.count(name) != 0;

        std::cout << "\n== Preparing module " << relative << "\n";
        for (std::size_t index = 1; index < paths.size(); ++index)
            std::cout << "Ignoring duplicate module " << paths[index].lexically_relative(gameDirectory).string() << "\n";

        try {
            auto module = PrepareModule(readFile(chosen), moduleArgs);
            if (module.Result.LinkInfo.Kind != Domain::ModuleKind::Library)
                throw std::runtime_error("module is not an SCE library");
            if (provided) {
                contested.emplace(name, std::move(module));
                contestedSources.emplace(name, relative);
            } else {
                prepared.emplace(name, std::move(module));
                preparedSources.emplace(name, relative);
            }
        } catch (const std::exception& e) {
            if (provided) {
                overridden.push_back(name + " (game copy " + relative + " could not be relinked)");
                continue;
            }
            std::cerr << "FAIL: " << relative << ": " << e.what() << "\n";
            failures.push_back(relative + ": " + e.what());
        }
    }

    std::cout << "\n== Preparing " << kGameExecutableName << "\n";
    auto executable = PrepareModule(readFile(gameExecutable), moduleArgs);
    if (executable.Result.LinkInfo.Kind == Domain::ModuleKind::Library)
        throw std::runtime_error(std::string(kGameExecutableName) + " is a library module, not an executable");

    std::map<std::string, std::set<std::string>> exportsByProvided;
    for (const auto& name : providedLibraries)
        for (const auto& symbol : ReadDynamicObject(readFile(outputLibs / name)).Symbols)
            if (symbol.Defined)
                exportsByProvided[name].insert(symbol.Name);

    // Which copy of a module both the game and AnyPS5 provide is used: a fakelib/ placeholder never
    // wins. For a system library, AnyPS5's implementation wins unless the game's copy resolves more of
    // the game's imports from it. For middleware the game ships, the game's code is authoritative and
    // an AnyPS5 library replaces it only when it resolves every import the game uses.
    std::map<std::string, std::set<std::string>> importsByLibrary;
    const auto collectImports = [&](const Domain::SysVDynamicSection& section) {
        for (const auto& symbol : listSymbols(section)) {
            const auto& version = section.SymbolVersions.at(symbol.Index);
            if (!symbol.Defined && !version.File.empty())
                importsByLibrary[version.File].insert(symbol.Name);
        }
    };
    collectImports(executable.Result.DynamicSection);
    for (const auto& [file, module] : prepared)
        collectImports(module.Result.DynamicSection);
    for (const auto& [file, module] : contested)
        collectImports(module.Result.DynamicSection);
    for (auto& [name, module] : contested) {
        std::set<std::string> gameCopyExports;
        for (const auto& symbol : listSymbols(module.Result.DynamicSection))
            if (symbol.Defined)
                gameCopyExports.insert(symbol.Name);
        std::size_t resolvedByAnyPS5 = 0;
        std::size_t onlyInGameCopy = 0;
        for (const auto& symbol : importsByLibrary[name]) {
            if (exportsByProvided[name].count(symbol) != 0)
                ++resolvedByAnyPS5;
            else if (gameCopyExports.count(symbol) != 0)
                ++onlyInGameCopy;
        }
        const std::string coverage = std::to_string(resolvedByAnyPS5) + " imports resolved by AnyPS5, " + std::to_string(onlyInGameCopy) + " only by the game copy";
        const bool placeholder = isPlaceholderModule(fs::path(contestedSources[name]));
        const bool preferGameCopy = !placeholder && (isSystemLibrary(name) ? onlyInGameCopy > resolvedByAnyPS5 : onlyInGameCopy > 0);
        if (preferGameCopy) {
            preferredGameCopies.push_back(name + " (" + coverage + ")");
            providedLibraries.erase(name);
            exportsByProvided.erase(name);
            fs::remove(outputLibs / name);
            preparedSources.emplace(name, contestedSources[name]);
            prepared.emplace(name, std::move(module));
        } else {
            overridden.push_back(name + " (game copy " + contestedSources[name] + "; " + coverage + ")");
        }
    }
    contested.clear();

    std::map<std::string, std::vector<std::string>> providedExports;
    for (const auto& [name, symbols] : exportsByProvided)
        for (const auto& symbol : symbols)
            providedExports[symbol].push_back(name);

    std::map<std::pair<std::string, std::string>, std::vector<std::string>> gameExports;
    for (const auto& [file, module] : prepared) {
        const auto& section = module.Result.DynamicSection;
        for (const auto& symbol : listSymbols(section))
            if (symbol.Defined)
                gameExports[{symbol.Name, section.SymbolVersions.at(symbol.Index).Name}].push_back(file);
    }

    std::set<std::string> gameExportNames;
    for (const auto& [key, files] : gameExports)
        gameExportNames.insert(key.first);
    for (const auto& symbol : listSymbols(executable.Result.DynamicSection))
        if (symbol.Defined)
            gameExportNames.insert(symbol.Name);

    std::vector<std::pair<std::string, PreparedModule*>> modules;
    for (auto& [file, module] : prepared)
        modules.emplace_back(file, &module);
    modules.emplace_back(kOutputExecutableName, &executable);

    const auto exists = [&](const std::string& file) {
        return providedLibraries.count(file) != 0 || prepared.count(file) != 0;
    };

    std::vector<Relinker::ShimSymbol> stubs;
    std::set<std::pair<std::string, std::string>> stubKeys;
    std::map<std::string, std::map<std::string, UnresolvedUse>> unresolved;
    std::set<std::string> droppedDependencies;
    std::size_t retargetedCount = 0;

    for (auto& [moduleName, module] : modules) {
        auto& section = module->Result.DynamicSection;
        std::vector<std::string> needed;
        for (const auto& library : ReadNeededLibraries(section)) {
            if (exists(library))
                needed.push_back(library);
            else
                droppedDependencies.insert(library);
        }
        bool usesStubs = moduleName == kOutputExecutableName;

        for (const auto& symbol : listSymbols(section)) {
            auto& version = section.SymbolVersions.at(symbol.Index);
            if (symbol.Defined || version.Name.empty() || version.File.empty())
                continue;
            const bool isObject = (symbol.Info & 0xf) == kSymbolTypeObject;
            const bool isWeak = (symbol.Info >> 4) == kSymbolBindingWeak;

            const auto provided = providedExports.find(symbol.Name);
            const auto game = gameExports.find({symbol.Name, version.Name});
            if (game != gameExports.end() && contains(game->second, version.File))
                continue;
            if (providedLibraries.count(version.File) != 0 && provided != providedExports.end() && contains(provided->second, version.File)) {
                if (gameExportNames.count(symbol.Name) == 0)
                    version = {};
                continue;
            }

            std::string target;
            if (provided != providedExports.end()) {
                const std::string alias = aliasCandidate(version.File);
                target = contains(provided->second, alias) ? alias : provided->second.front();
            } else if (game != gameExports.end()) {
                for (const auto& file : game->second)
                    if (file != moduleName)
                        target = file;
            }

            if (!target.empty()) {
                version.File = target;
                if (!contains(needed, target))
                    needed.push_back(target);
                if (providedLibraries.count(target) != 0 && gameExportNames.count(symbol.Name) == 0)
                    version = {};
                ++retargetedCount;
                continue;
            }
            if (isWeak) {
                version = {};
                continue;
            }

            const std::string library = version.Name;
            version.File = kUnresolvedLibraryName;
            usesStubs = true;
            if (stubKeys.emplace(symbol.Name, library).second)
                stubs.push_back({isObject ? Relinker::ShimSymbolKind::ZeroObject : Relinker::ShimSymbolKind::Report, symbol.Name, library, library});
            auto& use = unresolved[library][symbol.Name];
            use.IsObject = use.IsObject || isObject;
            use.Users.insert(moduleName);
        }

        if (usesStubs)
            needed.push_back(kUnresolvedLibraryName);
        SetNeededLibraries(section, needed);
        std::string baseVersion = kDefaultBaseVersion;
        for (const auto& symbol : listSymbols(section)) {
            if (symbol.Defined && !section.SymbolVersions.at(symbol.Index).Name.empty()) {
                baseVersion = section.SymbolVersions.at(symbol.Index).Name;
                break;
            }
        }
        Relinker::SysVDynamicSectionBuilder().BuildVersionTables(section, baseVersion);
    }

    std::set<std::string> relinkedLibraries;
    for (auto& [file, module] : prepared) {
        std::cout << "\n== Writing " << kLibsDirectoryName << "/" << file << "\n";
        try {
            EmitModule(module, (outputLibs / file).string(), moduleArgs);
            relinkedLibraries.insert(file);
        } catch (const std::exception& e) {
            std::cerr << "FAIL: " << preparedSources[file] << ": " << e.what() << "\n";
            failures.push_back(preparedSources[file] + ": " + e.what());
        }
    }
    std::cout << "\n== Writing " << kOutputExecutableName << "\n";
    EmitModule(executable, (outputDirectory / kOutputExecutableName).string(), moduleArgs);
    fs::permissions(outputDirectory / kOutputExecutableName, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add);

    Relinker::ShimLibraryRequest stubRequest;
    stubRequest.SoName = kUnresolvedLibraryName;
    stubRequest.Symbols = stubs;
    if (std::any_of(stubs.begin(), stubs.end(), [](const Relinker::ShimSymbol& symbol) { return symbol.Kind == Relinker::ShimSymbolKind::Report; })) {
        stubRequest.Needed.push_back(kReporterFile);
        stubRequest.ReporterName = kReporterSymbol;
        stubRequest.ReporterFile = kReporterFile;
    }
    Io::FileWriter().Write((outputLibs / kUnresolvedLibraryName).string(), Relinker::ShimLibraryBuilder().Build(stubRequest));

    std::vector<std::pair<std::string, const std::map<std::string, UnresolvedUse>*>> byLibrary;
    std::size_t unresolvedCount = 0;
    for (const auto& [library, symbols] : unresolved) {
        byLibrary.emplace_back(library, &symbols);
        unresolvedCount += symbols.size();
    }
    std::sort(byLibrary.begin(), byLibrary.end(), [](const auto& left, const auto& right) {
        return left.second->size() != right.second->size() ? left.second->size() > right.second->size() : left.first < right.first;
    });

    std::ofstream report(outputDirectory / kReportFileName, std::ios::trunc);
    if (!report)
        throw std::runtime_error("Cannot write " + (outputDirectory / kReportFileName).string());
    report << "AnyPS5 unresolved import report\n";
    report << "Game: " << gameDirectory.string() << "\n\n";
    report << "Dependencies without a library file: " << droppedDependencies.size() << "\n";
    for (const auto& file : droppedDependencies)
        report << "  " << file << "\n";
    report << "\nUnresolved functions: " << unresolvedCount << "\n";
    for (const auto& [library, symbols] : byLibrary) {
        report << "\n" << library << ": " << symbols->size() << "\n";
        for (const auto& [name, use] : *symbols) {
            report << "  " << name << (use.IsObject ? " [object]" : "") << " used by";
            for (const auto& user : use.Users)
                report << " " << user;
            report << "\n";
        }
    }
    report.close();

    const fs::path gameMount = outputDirectory / kGameMountName;
    std::error_code linkError;
    const auto mountStatus = fs::symlink_status(gameMount, linkError);
    if (!fs::exists(mountStatus)) {
        fs::create_directory_symlink(gameDirectory, gameMount, linkError);
        if (linkError)
            std::cerr << "Could not link " << gameMount.string() << " to the game dump: " << linkError.message() << "\n";
    } else if (!fs::is_symlink(mountStatus) || fs::read_symlink(gameMount) != gameDirectory) {
        std::cerr << "Keeping existing " << gameMount.string() << "; it does not point to " << gameDirectory.string() << "\n";
    }

    std::cout << "\n== Game relink summary\n";
    std::cout << "Game libraries relinked: " << relinkedLibraries.size() << "\n";
    for (const auto& name : relinkedLibraries)
        std::cout << "  " << name << "\n";
    std::cout << "Game libraries replaced by AnyPS5: " << overridden.size() << "\n";
    for (const auto& name : overridden)
        std::cout << "  " << name << "\n";
    std::cout << "Game copies kept over partial AnyPS5 libraries: " << preferredGameCopies.size() << "\n";
    for (const auto& name : preferredGameCopies)
        std::cout << "  " << name << "\n";
    std::cout << "Failed modules: " << failures.size() << "\n";
    for (const auto& failure : failures)
        std::cout << "  " << failure << "\n";
    std::cout << "Dependencies without a library file: " << droppedDependencies.size() << "\n";
    for (const auto& file : droppedDependencies)
        std::cout << "  " << file << "\n";
    std::cout << "Imports bound to another library file: " << retargetedCount << "\n";
    std::cout << "Unresolved functions: " << unresolvedCount << " in " << byLibrary.size() << " libraries (details in " << kReportFileName << ")\n";
    for (std::size_t index = 0; index < byLibrary.size() && index < 10; ++index)
        std::cout << "  " << byLibrary[index].first << ": " << byLibrary[index].second->size() << "\n";
    std::cout << "Run: cd \"" << outputDirectory.string() << "\" && ./" << kOutputExecutableName << "\n";

    return failures.empty() ? 0 : kExitCompletedWithProblems;
}

}
