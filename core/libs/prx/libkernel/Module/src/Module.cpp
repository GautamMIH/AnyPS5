#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#if !defined(__linux__)
// On Linux <link.h> (below) declares dl_phdr_info, Elf64_Phdr, PT_LOAD and PF_X itself.
#include "prx/libc/include/specifics/linux/ElfTypes.hpp"
#endif
#include "prx/libkernel/KernelErrors.hpp"
#include <nid/NidCompute.hpp>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#endif

#ifdef _WIN32
namespace {
std::uint64_t ReadEncoded(const std::uint8_t*& p, std::uint8_t encoding) {
  const auto application = encoding & 0x70;
  if ((encoding & 0x80) != 0 || (application != 0x00 && application != 0x10)) NotImplemented_nid_no_patch("sceKernelGetModuleInfoForUnwind eh_frame_ptr encoding other than absptr or pcrel");
  std::uint64_t value = 0;
  const auto* at = p;
  switch (encoding & 0x0f) {
  case 0x03: { std::uint32_t v; std::memcpy(&v, p, 4); value = v; p += 4; break; }
  case 0x0b: { std::int32_t v; std::memcpy(&v, p, 4); value = static_cast<std::uint64_t>(static_cast<std::int64_t>(v)); p += 4; break; }
  case 0x04: case 0x0c: std::memcpy(&value, p, 8); p += 8; break;
  default: NotImplemented_nid_no_patch("sceKernelGetModuleInfoForUnwind eh_frame_ptr value format");
  }
  if (application == 0x10) value += reinterpret_cast<std::uint64_t>(at);
  return value;
}

void FillGuestUnwindInfo(const std::uint8_t* base, ModuleInfoForUnwind* info) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) throw std::runtime_error("sceKernelGetModuleInfoForUnwind: image without a DOS header");
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) throw std::runtime_error("sceKernelGetModuleInfoForUnwind: image without an NT header");
  const auto* sections = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    if (std::memcmp(sections[i].Name, ".ehmeta", 8) != 0) continue;
    std::uint32_t rva = 0;
    std::memcpy(&rva, base + sections[i].VirtualAddress, 4);
    const auto* header = base + rva;
    if (header[0] != 1) NotImplemented_nid_no_patch("sceKernelGetModuleInfoForUnwind eh_frame_hdr version other than 1");
    const auto* p = header + 4;
    const auto frames = ReadEncoded(p, header[1]);
    const auto* record = reinterpret_cast<const std::uint8_t*>(frames);
    const auto* end = base + nt->OptionalHeader.SizeOfImage;
    if (record < base || record >= end) throw std::runtime_error("sceKernelGetModuleInfoForUnwind: eh_frame_ptr outside the image");
    for (;;) {
      if (record + 4 > end) throw std::runtime_error("sceKernelGetModuleInfoForUnwind: eh_frame has no terminator inside the image");
      std::uint32_t length = 0;
      std::memcpy(&length, record, 4);
      if (length == 0) break;
      if (length == 0xffffffffu) NotImplemented_nid_no_patch("sceKernelGetModuleInfoForUnwind eh_frame record with a 64-bit length");
      record += 4 + length;
    }
    info->eh_frame_hdr_addr = reinterpret_cast<std::uint64_t>(header);
    info->eh_frame_addr = frames;
    info->eh_frame_size = static_cast<std::uint64_t>(record - reinterpret_cast<const std::uint8_t*>(frames));
    info->seg0_addr = reinterpret_cast<std::uint64_t>(base);
    info->seg0_size = nt->OptionalHeader.SizeOfImage;
    return;
  }
}
}
#endif

extern "C" {
void* APS5_VABI dlopen_nid_postfix(const char* path, int flags);
void* APS5_VABI dlsym_nid_postfix(void* handle, const char* name);
int APS5_VABI dlclose_nid_postfix(void* handle);
}

namespace {

constexpr int kErrorNoEntry = SCE_KERNEL_ERROR_ENOENT;
constexpr int kErrorNoSuchModule = SCE_KERNEL_ERROR_ESRCH;
constexpr int kErrorInvalidArgument = SCE_KERNEL_ERROR_EINVAL;
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

// A module requested by guest path: the relinked copy in libs/ (game mode layout) first, then the
// guest path itself through the app0 mapping (single-executable layout, app0/sce_module/).
std::filesystem::path resolveModulePath(const char* guestPath) {
    const std::filesystem::path relinked = librariesDirectory() / moduleFileName(guestPath);
    if (std::filesystem::is_regular_file(relinked))
        return relinked;
    std::filesystem::path mapped = ResolvePath_nid_no_patch(guestPath);
    if (mapped.extension() == kSignedModuleExtension)
        mapped.replace_extension(kModuleExtension);
    return mapped;
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
  (void)flags;
  if (!info) return SCE_KERNEL_ERROR_EFAULT;
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return SCE_KERNEL_ERROR_ESRCH;
  info->st_size = sizeof(ModuleInfoForUnwind);
  info->eh_frame_hdr_addr = 0;
  info->eh_frame_addr = 0;
  info->eh_frame_size = 0;
  info->seg0_addr = reinterpret_cast<std::uint64_t>(mbi.BaseAddress);
  info->seg0_size = mbi.RegionSize;
  if (mbi.Type == MEM_IMAGE) FillGuestUnwindInfo(static_cast<const std::uint8_t*>(mbi.AllocationBase), info);
  char path[4096] = {};
  DWORD len = GetMappedFileNameA(GetCurrentProcess(), mbi.BaseAddress, path, sizeof(path) - 1);
  path[len] = '\0';
  std::strncpy(info->name, path, sizeof(info->name) - 1);
  info->name[sizeof(info->name) - 1] = '\0';
  return 0;
#elif defined(__APPLE__)
  struct Search {
    std::uint64_t address;
    ModuleInfoForUnwind* info;
    bool found;
  } search {addr, info, false};
  dl_iterate_phdr([](dl_phdr_info* image, std::size_t, void* data) {
    auto& search = *static_cast<Search*>(data);
    const Elf64_Phdr* first = nullptr;
    const Elf64_Phdr* frames = nullptr;
    bool contains = false;
    for (std::uint16_t index = 0; index < image->dlpi_phnum; ++index) {
      const auto& header = image->dlpi_phdr[index];
      const auto start = image->dlpi_addr + header.p_vaddr;
      if (header.p_type == PT_LOAD && first == nullptr) first = &header;
      if (header.p_type == PT_LOAD && search.address >= start && search.address - start < header.p_memsz) contains = true;
      if (header.p_type == PT_GNU_EH_FRAME) frames = &header;
    }
    if (!contains) return 0;
    auto* info = search.info;
    info->st_size = sizeof(ModuleInfoForUnwind);
    std::strncpy(info->name, image->dlpi_name != nullptr ? image->dlpi_name : "", sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    info->eh_frame_hdr_addr = frames != nullptr ? image->dlpi_addr + frames->p_vaddr : 0;
    info->eh_frame_addr = 0;
    info->eh_frame_size = 0;
    info->seg0_addr = image->dlpi_addr + first->p_vaddr;
    info->seg0_size = first->p_memsz;
    search.found = true;
    return 1;
  }, &search);
  if (search.found) return 0;
  Dl_info image {};
  if (!dladdr(reinterpret_cast<void*>(addr), &image) || image.dli_fbase == nullptr) return SCE_KERNEL_ERROR_ESRCH;
  unsigned long textSize = 0;
  getsegmentdata(static_cast<const mach_header_64*>(image.dli_fbase), "__TEXT", &textSize);
  info->st_size = sizeof(ModuleInfoForUnwind);
  std::strncpy(info->name, image.dli_fname != nullptr ? image.dli_fname : "", sizeof(info->name) - 1);
  info->name[sizeof(info->name) - 1] = '\0';
  info->eh_frame_hdr_addr = 0;
  info->eh_frame_addr = 0;
  info->eh_frame_size = 0;
  info->seg0_addr = reinterpret_cast<std::uint64_t>(image.dli_fbase);
  info->seg0_size = textSize;
  return 0;
#else
  std::ifstream maps("/proc/self/maps");
  if (!maps) throw std::runtime_error("sceKernelGetModuleInfoForUnwind: failed to open /proc/self/maps");
  std::string line;
  while (std::getline(maps, line)) {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    char perms[8] = {};
    std::uint64_t offset = 0;
    unsigned int devMajor = 0;
    unsigned int devMinor = 0;
    std::uint64_t inode = 0;
    char path[4096] = {};
    int parsed = std::sscanf(line.c_str(), "%llx-%llx %7s %llx %x:%x %llu %4095s",
      (unsigned long long*)&start, (unsigned long long*)&end, perms,
      (unsigned long long*)&offset, &devMajor, &devMinor, (unsigned long long*)&inode, path);
    if (parsed < 7 || addr < start || addr >= end) continue;
    info->st_size = sizeof(ModuleInfoForUnwind);
    std::strncpy(info->name, parsed >= 8 ? path : "", sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    info->eh_frame_hdr_addr = 0;
    info->eh_frame_addr = 0;
    info->eh_frame_size = 0;
    info->seg0_addr = start;
    info->seg0_size = end - start;
    return 0;
  }
  return SCE_KERNEL_ERROR_ESRCH;
#endif
}

// sceKernelGetModuleInfoFromAddr lives in ModuleInfo.cpp.

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
    const std::filesystem::path path = resolveModulePath(module_file_name);
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

int APS5_VABI __elf_phdr_match_addr_nid_postfix(dl_phdr_info* phdrInfo, void* addr) {
    if (phdrInfo == nullptr) throw std::invalid_argument("__elf_phdr_match_addr: phdr_info is null");
    const auto address = reinterpret_cast<std::uintptr_t>(addr);
    for (std::uint16_t i = 0; i < phdrInfo->dlpi_phnum; ++i) {
        const Elf64_Phdr& header = phdrInfo->dlpi_phdr[i];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) == 0) continue;
        const std::uintptr_t begin = phdrInfo->dlpi_addr + header.p_vaddr;
        if (begin <= address && address + sizeof(addr) < begin + header.p_memsz) return 1;
    }
    return 0;
}

// unknown signature
std::int32_t APS5_VABI sceKernelInternalMemoryGetModuleSegmentInfo_nid_postfix(void* result) {
    (void)result;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
