#include <Cli.hpp>
#include <domain/Types.hpp>
#include <io/FileReader.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
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
constexpr char kProvidedProbeName[] = "libkernel.prx";
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

}

int RelinkGame(const Args& args, const std::string& executablePath) {
    const fs::path gameDirectory = fs::weakly_canonical(fs::absolute(args.inputPath));
    const fs::path outputDirectory = fs::weakly_canonical(fs::absolute(args.outputPath));
    const fs::path gameExecutable = gameDirectory / kGameExecutableName;
    if (!fs::is_regular_file(gameExecutable))
        throw std::runtime_error("Game dump has no " + std::string(kGameExecutableName) + ": " + gameDirectory.string());

    const fs::path libsDirectory = args.libsPath.empty() ? findProvidedLibraries(executablePath) : fs::weakly_canonical(fs::absolute(args.libsPath));
    if (!fs::is_directory(libsDirectory))
        throw std::runtime_error("AnyPS5 prx library directory does not exist: " + libsDirectory.string());

    const fs::path outputLibs = outputDirectory / kLibsDirectoryName;
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
            if (path.filename() == kSystemDirectoryName || path == outputDirectory || path == libsDirectory)
                it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file() && isModuleFile(path) && !isWithin(path, outputDirectory))
            gameModules[moduleFileName(path)].push_back(path);
    }

    Args moduleArgs = args;
    moduleArgs.gameMode = false;
    moduleArgs.libsPath.clear();

    std::map<std::string, std::vector<std::string>> neededByModule;
    std::set<std::string> relinkedLibraries;
    std::vector<std::string> failures;
    std::vector<std::string> overridden;

    for (auto& [name, paths] : gameModules) {
        std::sort(paths.begin(), paths.end(), [](const fs::path& left, const fs::path& right) {
            const std::size_t leftDepth = pathDepth(left);
            const std::size_t rightDepth = pathDepth(right);
            return leftDepth != rightDepth ? leftDepth < rightDepth : left.string() < right.string();
        });
        const fs::path& chosen = paths.front();
        const std::string relative = chosen.lexically_relative(gameDirectory).string();

        if (providedLibraries.count(name) != 0) {
            overridden.push_back(name + " (game copy " + relative + ")");
            continue;
        }

        std::cout << "\n== Relinking module " << relative << " -> " << kLibsDirectoryName << "/" << name << "\n";
        for (std::size_t index = 1; index < paths.size(); ++index)
            std::cout << "Ignoring duplicate module " << paths[index].lexically_relative(gameDirectory).string() << "\n";

        try {
            const auto outcome = RelinkModule(readFile(chosen), (outputLibs / name).string(), moduleArgs);
            if (!outcome.IsLibrary)
                throw std::runtime_error("module is not an SCE library");
            neededByModule[name] = outcome.NeededLibraries;
            relinkedLibraries.insert(name);
        } catch (const std::exception& e) {
            std::cerr << "FAIL: " << relative << ": " << e.what() << "\n";
            failures.push_back(relative + ": " + e.what());
            std::error_code ignored;
            fs::remove(outputLibs / name, ignored);
        }
    }

    std::cout << "\n== Relinking " << kGameExecutableName << " -> " << kOutputExecutableName << "\n";
    const auto executableOutcome = RelinkModule(readFile(gameExecutable), (outputDirectory / kOutputExecutableName).string(), moduleArgs);
    if (executableOutcome.IsLibrary)
        throw std::runtime_error(std::string(kGameExecutableName) + " is a library module, not an executable");
    neededByModule[kOutputExecutableName] = executableOutcome.NeededLibraries;
    fs::permissions(outputDirectory / kOutputExecutableName, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add);

    std::map<std::string, std::set<std::string>> missingLibraries;
    for (const auto& [module, needed] : neededByModule)
        for (const auto& library : needed)
            if (providedLibraries.count(library) == 0 && relinkedLibraries.count(library) == 0)
                missingLibraries[library].insert(module);

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
    std::cout << "Failed modules: " << failures.size() << "\n";
    for (const auto& failure : failures)
        std::cout << "  " << failure << "\n";
    std::cout << "Unresolved libraries: " << missingLibraries.size() << "\n";
    for (const auto& [library, users] : missingLibraries) {
        std::cout << "  " << library << " needed by";
        for (const auto& user : users)
            std::cout << " " << user;
        std::cout << "\n";
    }
    std::cout << "Run: cd \"" << outputDirectory.string() << "\" && ./" << kOutputExecutableName << "\n";

    return failures.empty() && missingLibraries.empty() ? 0 : kExitCompletedWithProblems;
}

}
