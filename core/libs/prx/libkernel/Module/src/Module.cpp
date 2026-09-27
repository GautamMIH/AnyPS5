#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include <nid/NidCompute.hpp>

#if defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#endif

namespace {

constexpr int kErrorNoEntry = static_cast<int>(0x80020002);
constexpr int kErrorNoSuchModule = static_cast<int>(0x80020003);
constexpr int kErrorInvalidArgument = static_cast<int>(0x80020016);
constexpr char kModuleStartName[] = "module_start";
constexpr char kModuleStopName[] = "module_stop";
constexpr char kLibrariesDirectoryName[] = "libs";
constexpr char kModuleExtension[] = ".prx";
constexpr char kSignedModuleExtension[] = ".sprx";

using ModuleEntry = int (APS5_VABI*)(std::size_t args, const void* argp);
using ModuleInit = int (APS5_VABI*)(std::size_t args, const void* argp, void* entry);

#if defined(__linux__)

// A module being started by sceKernelLoadStartModule. dlopen runs init functions on the calling
// thread, dependencies first, so the hook matches the module by path before handing over the
// start arguments.
struct PendingStart {
    std::filesystem::path Path;
    std::size_t Args;
    const void* Argp;
    bool Started;
    int Result;
};

thread_local PendingStart* pendingStart = nullptr;

bool isPendingModule(const PendingStart& pending, const char* loadedPath) {
    if (loadedPath == nullptr)
        return false;
    if (pending.Path == loadedPath)
        return true;
    std::error_code error;
    return std::filesystem::equivalent(pending.Path, loadedPath, error);
}

struct LoadedModule {
    void* Handle;
    std::filesystem::path Path;
    bool Dynamic;
};

std::filesystem::path executablePath() {
    return std::filesystem::read_symlink("/proc/self/exe");
}

std::filesystem::path librariesDirectory() {
    return executablePath().parent_path() / kLibrariesDirectoryName;
}

class ModuleRegistry {
public:
    KernelModule Register(void* handle, const std::filesystem::path& path, const bool dynamic) {
        const std::lock_guard lock(_mutex);
        _ensureStartupModules();
        for (std::size_t index = 0; index < _modules.size(); ++index) {
            if (_modules[index].Handle == handle) {
                if (dynamic)
                    dlclose(handle);
                return static_cast<KernelModule>(index);
            }
        }
        _modules.push_back({handle, path, dynamic});
        return static_cast<KernelModule>(_modules.size() - 1);
    }

    void* Handle(const KernelModule module) {
        const std::lock_guard lock(_mutex);
        _ensureStartupModules();
        if (module < 0 || static_cast<std::size_t>(module) >= _modules.size())
            return nullptr;
        return _modules[static_cast<std::size_t>(module)].Handle;
    }

    bool Release(const KernelModule module) {
        const std::lock_guard lock(_mutex);
        _ensureStartupModules();
        if (module < 0 || static_cast<std::size_t>(module) >= _modules.size() || _modules[static_cast<std::size_t>(module)].Handle == nullptr)
            return false;
        auto& entry = _modules[static_cast<std::size_t>(module)];
        if (entry.Dynamic && dlclose(entry.Handle) != 0)
            throw std::runtime_error(std::string("module unload failed: ") + dlerror());
        entry.Handle = nullptr;
        return true;
    }

private:
    std::mutex _mutex;
    std::vector<LoadedModule> _modules;
    bool _initialized = false;

    void _ensureStartupModules() {
        if (_initialized)
            return;
        _initialized = true;
        void* self = dlopen(nullptr, RTLD_NOW);
        if (self == nullptr)
            throw std::runtime_error(std::string("cannot open the main module: ") + dlerror());
        _modules.push_back({self, executablePath(), false});

        const std::filesystem::path libraries = std::filesystem::weakly_canonical(librariesDirectory());
        std::vector<std::filesystem::path> startupLibraries;
        dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* data) {
            if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0')
                static_cast<std::vector<std::filesystem::path>*>(data)->emplace_back(info->dlpi_name);
            return 0;
        }, &startupLibraries);
        for (const auto& path : startupLibraries) {
            std::error_code error;
            const auto canonical = std::filesystem::weakly_canonical(path, error);
            if (error || canonical.parent_path() != libraries)
                continue;
            void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
            if (handle == nullptr)
                throw std::runtime_error("startup module is not loaded: " + path.string());
            _modules.push_back({handle, canonical, true});
        }
    }
};

ModuleRegistry& registry() {
    static ModuleRegistry instance;
    return instance;
}

std::string moduleFileName(const char* guestPath) {
    std::filesystem::path name = std::filesystem::path(guestPath).filename();
    if (name.extension() == kSignedModuleExtension)
        name.replace_extension(kModuleExtension);
    return name.string();
}

void* findSymbol(void* handle, const char* symbol) {
    const std::string nid = Nid::ComputeNid(symbol, {});
    if (void* address = dlsym(handle, nid.c_str()))
        return address;
    return dlsym(handle, symbol);
}

#endif

}

extern "C" {

int APS5_VABI sceKernelDlsym(KernelModule handle, const char* symbol, void** addr) {
    if (symbol == nullptr || symbol[0] == '\0' || addr == nullptr)
        return kErrorInvalidArgument;
#if defined(__linux__)
    void* module = registry().Handle(handle);
    if (module == nullptr)
        return kErrorNoSuchModule;
    void* address = findSymbol(module, symbol);
    if (address == nullptr)
        return kErrorNoSuchModule;
    *addr = address;
    return 0;
#else
    NotImplemented_nid_no_patch(__func__);
    return 0;
#endif
}

int APS5_VABI sceKernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info) {
 (void)addr;
 (void)flags;
 (void)info;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceKernelGetModuleInfoFromAddr(uint64_t addr, int n, ModuleInfo* r) {
 (void)addr;
 (void)n;
 (void)r;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

#if defined(__linux__)
// Exported as __anyps5_module_init. DT_INIT of every relinked library tail-calls this with the
// PS5 init entry, which runs the module's initializers and then module_start(args, argp).
// Modules loaded at startup or as dependencies are started without arguments, as the PS5
// loader does.
__attribute__((visibility("default"))) int __anyps5_module_init_nid_no_patch_cut(ModuleInit init) {
    PendingStart* pending = pendingStart;
    if (pending != nullptr && !pending->Started) {
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(init), &info) != 0 && isPendingModule(*pending, info.dli_fname)) {
            pending->Started = true;
            pending->Result = init(pending->Args, pending->Argp, nullptr);
            return 0;
        }
    }
    init(0, nullptr, nullptr);
    return 0;
}
#endif

KernelModule APS5_VABI sceKernelLoadStartModule(const char* module_file_name, size_t args, const void* argp, uint32_t flags, const KernelLoadModuleOpt* opt, int* res) {
    (void)flags;
    (void)opt;
    if (module_file_name == nullptr || module_file_name[0] == '\0')
        return kErrorInvalidArgument;
#if defined(__linux__)
    const std::filesystem::path path = librariesDirectory() / moduleFileName(module_file_name);
    if (!std::filesystem::is_regular_file(path))
        return kErrorNoEntry;
    // The relinked init runs module_start itself; see __anyps5_module_init.
    PendingStart pending{path, args, argp, false, 0};
    pendingStart = &pending;
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    pendingStart = nullptr;
    if (handle == nullptr)
        throw std::runtime_error(std::string("module load failed: ") + dlerror());
    const KernelModule module = registry().Register(handle, path, true);
    int result = pending.Result;
    if (!pending.Started) {
        if (auto* start = reinterpret_cast<ModuleEntry>(findSymbol(handle, kModuleStartName)))
            result = start(args, argp);
    }
    if (res != nullptr)
        *res = result;
    return module;
#else
    (void)args;
    (void)argp;
    (void)res;
    NotImplemented_nid_no_patch(__func__);
    return {};
#endif
}

int APS5_VABI sceKernelStopUnloadModule(KernelModule handle, size_t args, const void* argp, uint32_t flags, const KernelUnloadModuleOpt* opt, int* res) {
    (void)flags;
    (void)opt;
#if defined(__linux__)
    void* module = registry().Handle(handle);
    if (module == nullptr)
        return kErrorNoSuchModule;
    int result = 0;
    if (auto* stop = reinterpret_cast<ModuleEntry>(findSymbol(module, kModuleStopName)))
        result = stop(args, argp);
    if (res != nullptr)
        *res = result;
    registry().Release(handle);
    return 0;
#else
    (void)handle;
    (void)args;
    (void)argp;
    (void)res;
    NotImplemented_nid_no_patch(__func__);
    return 0;
#endif
}

}

extern "C" {

int APS5_VABI __elf_phdr_match_addr_nid_postfix(ModuleInfo* module, std::uint64_t address) {
    (void)module;
    (void)address;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

// unknown signature
std::int32_t APS5_VABI sceKernelInternalMemoryGetModuleSegmentInfo_nid_postfix(void* result) {
    (void)result;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
