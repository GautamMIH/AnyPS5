#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <filesystem>
#include <optional>
#include <cerrno>
#include <cstring>
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestHeap.hpp"
#include "prx/libc/include/AmprContainer.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <map>
#include <set>

namespace {
// Guest prefixes (without leading slashes) mapped to host directories, e.g. save-data mount points:
// the PS5 hands the title a short mount point ("/_sm/0") whose files live under _sd/<dir name>.
struct PathAliases {
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> entries;
};

PathAliases& Aliases() {
    static PathAliases aliases;
    return aliases;
}

std::string TrimSlashes(const char* path) {
    std::string s(path);
    std::size_t start = 0;
    while (start < s.size() && (s[start] == '/' || s[start] == '\\')) {
        ++start;
    }
    std::size_t end = s.size();
    while (end > start && (s[end - 1] == '/' || s[end - 1] == '\\')) {
        --end;
    }
    return s.substr(start, end - start);
}

std::optional<std::filesystem::path> ResolveAlias(const std::string& guestPath) {
    const auto relative = TrimSlashes(guestPath.c_str());
    auto& aliases = Aliases();
    std::lock_guard lock(aliases.mutex);
    for (const auto& [prefix, host] : aliases.entries) {
        if (relative.size() < prefix.size() || relative.compare(0, prefix.size(), prefix) != 0) continue;
        if (relative.size() == prefix.size()) return std::filesystem::path(host).make_preferred();
        if (relative[prefix.size()] != '/') continue;
        std::filesystem::path result = std::filesystem::path(host) / std::filesystem::path(relative.substr(prefix.size() + 1));
        return result.make_preferred();
    }
    return std::nullopt;
}

struct WorkingDirectory {
    std::mutex mutex;
    const std::filesystem::path root = std::filesystem::canonical(std::filesystem::current_path());
    std::filesystem::path current = root;
    // Guest mount points ("/savedata0") backed by host directories outside the root.
    std::map<std::string, std::filesystem::path> mounts;
};
WorkingDirectory& Directories() { static WorkingDirectory state; return state; }
std::filesystem::path Resolve(WorkingDirectory& state, const char* path) {
    std::string text(path);
    for (auto& character : text) if (character == '\\') character = '/';
    std::filesystem::path input(text);
#ifdef _WIN32
    // Preserve the existing ability to pass explicit native drive paths.
    if (input.has_root_name()) return input;
#endif
    auto guest = (std::filesystem::path("/") / state.current.lexically_relative(state.root));
    guest = (input.is_absolute() ? input : guest / input).lexically_normal();
    const auto normalised = guest.generic_string();
    for (const auto& [mountPoint, host] : state.mounts) {
        if (normalised == mountPoint) return host;
        if (normalised.size() > mountPoint.size() && normalised.compare(0, mountPoint.size(), mountPoint) == 0 && normalised[mountPoint.size()] == '/') {
            return (host / normalised.substr(mountPoint.size() + 1)).make_preferred();
        }
    }
    if (auto aliased = ResolveAlias(guest.relative_path().generic_string())) return *aliased;
    return (state.root / guest.relative_path()).make_preferred();
}
int DirectoryFailure(const std::error_code& error) {
    if (error == std::errc::permission_denied) return 13;
    if (error == std::errc::not_a_directory) return 20;
    if (error == std::errc::no_such_file_or_directory) return 2;
    if (error == std::errc::filename_too_long) return 63;
    if (error == std::errc::too_many_symbolic_link_levels) return 62;
    return 5;
}

// The title's AMPR asset container, if it ships one, serves /app0 files that are not on disk.
AmprContainer::Container* appContainer(const std::filesystem::path& root) {
    static std::once_flag once;
    static std::unique_ptr<AmprContainer::Container> container;
    std::call_once(once, [&root] { container = AmprContainer::Container::Open(root / "app0", root / "ampr_cache"); });
    return container.get();
}
}

extern "C" void MountGuestPath_nid_no_patch(const char* mountPoint, const std::filesystem::path& host) {
    if (!mountPoint || mountPoint[0] != '/' || mountPoint[1] == '\0' || std::strchr(mountPoint + 1, '/') != nullptr) { APS5_INVALID_ARG_EX; }
    auto& state = Directories();
    std::lock_guard lock(state.mutex);
    state.mounts[mountPoint] = std::filesystem::absolute(host).lexically_normal().make_preferred();
}

extern "C" void UnmountGuestPath_nid_no_patch(const char* mountPoint) {
    if (!mountPoint) { APS5_INVALID_ARG_EX; }
    auto& state = Directories();
    std::lock_guard lock(state.mutex);
    state.mounts.erase(mountPoint);
}

extern "C" void AddPathAlias_nid_no_patch(const char* guestPrefix, const char* hostPath) {
    if (guestPrefix == nullptr || hostPath == nullptr) {
        APS5_INVALID_ARG_EX;
    }
    auto& aliases = Aliases();
    std::lock_guard lock(aliases.mutex);
    const auto prefix = TrimSlashes(guestPrefix);
    for (auto& entry : aliases.entries) {
        if (entry.first == prefix) {
            entry.second = hostPath;
            return;
        }
    }
    aliases.entries.emplace_back(prefix, hostPath);
}

extern "C" void RemovePathAlias_nid_no_patch(const char* guestPrefix) {
    if (guestPrefix == nullptr) {
        APS5_INVALID_ARG_EX;
    }
    auto& aliases = Aliases();
    std::lock_guard lock(aliases.mutex);
    const auto prefix = TrimSlashes(guestPrefix);
    std::erase_if(aliases.entries, [&](const auto& entry) { return entry.first == prefix; });
}

extern "C" std::filesystem::path ResolvePath_nid_no_patch(const char* path) {
    if (!path) { APS5_INVALID_ARG_EX; }
    auto& state = Directories();
    std::lock_guard lock(state.mutex);
    auto result = Resolve(state, path);
    const auto guest = "/" + result.lexically_relative(state.root).generic_string();
#ifndef _WIN32
    // Character devices the guest kernel also provides map to the host's devices of the same name.
    if (guest == "/dev/random" || guest == "/dev/urandom" || guest == "/dev/null" || guest == "/dev/zero") {
        return std::filesystem::path(guest);
    }
#endif
    std::error_code error;
    if (AmprContainer::NormalisePath(guest).rfind("/app0/", 0) == 0 && !std::filesystem::exists(result, error)) {
        if (auto* container = appContainer(state.root)) {
            if (auto packed = container->Resolve(guest)) return packed->make_preferred();
        }
    }
    return result;
}

extern "C" int APS5_VABI chdir_nid_postfix(const char* path) {
    if (!path) { errno = 14; return -1; }
    if (!*path) { errno = 2; return -1; }
    try {
        auto& state = Directories();
        std::lock_guard lock(state.mutex);
        std::error_code error;
        const auto resolved = std::filesystem::canonical(Resolve(state, path), error);
        if (error) { errno = DirectoryFailure(error); return -1; }
        if (!std::filesystem::is_directory(resolved, error)) {
            errno = error ? DirectoryFailure(error) : 20; return -1;
        }
        const auto relative = resolved.lexically_relative(state.root);
        if (relative.empty() || *relative.begin() == "..") { errno = 45; return -1; }
        state.current = resolved;
        return 0;
    } catch (const std::bad_alloc&) { errno = 12; return -1; }
      catch (const std::filesystem::filesystem_error& error) { errno = DirectoryFailure(error.code()); return -1; }
}

extern "C" char* APS5_VABI getcwd_nid_postfix(char* buffer, std::size_t size) {
    if (buffer && size == 0) { errno = 22; return nullptr; }
    try {
        auto& state = Directories();
        std::lock_guard lock(state.mutex);
        std::error_code error;
        if (!std::filesystem::is_directory(state.current, error)) {
            errno = error ? DirectoryFailure(error) : 2; return nullptr;
        }
        const auto relative = state.current.lexically_relative(state.root);
        const auto path = relative == "." ? std::string("/") : "/" + relative.generic_string();
        const auto required = path.size() + 1;
        if ((buffer || size) && size < required) { errno = 34; return nullptr; }
        if (!buffer) buffer = static_cast<char*>(GuestHeap::GuestHeapAllocate_nid_postfix(size ? size : required));
        std::memcpy(buffer, path.c_str(), required);
        return buffer;
    } catch (const std::bad_alloc&) { errno = 12; return nullptr; }
      catch (const std::filesystem::filesystem_error& error) { errno = DirectoryFailure(error.code()); return nullptr; }
}

namespace {

constexpr char kReportUnimplementedVariable[] = "ANYPS5_REPORT_UNIMPLEMENTED";

bool reportUnimplementedEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv(kReportUnimplementedVariable);
        return value != nullptr && value[0] != '\0' && std::string(value) != "0";
    }();
    return enabled;
}

void reportOnce(const std::string& message) {
    static std::mutex mutex;
    static std::set<std::string> reported;
    const std::lock_guard lock(mutex);
    if (reported.insert(message).second)
        std::fprintf(stderr, "[AnyPS5] %s\n", message.c_str());
}

}

extern "C" void NotImplemented_nid_no_patch(const char* funcName) {
    const std::string message = std::string(funcName) + " not implemented";
    if (!reportUnimplementedEnabled())
        throw std::runtime_error(message);
    reportOnce(message);
}

extern "C" std::uint64_t APS5_VABI UnresolvedImport_nid_no_patch(const char* nid, const char* library) {
    const std::string message = std::string("unresolved import ") + nid + " from " + library + " called";
    if (!reportUnimplementedEnabled())
        throw std::runtime_error(message);
    reportOnce(message);
    return 0;
}
