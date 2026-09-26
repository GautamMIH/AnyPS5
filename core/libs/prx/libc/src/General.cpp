#include <stdexcept>
#include <string>
#include <filesystem>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>

#include "prx/libc/include/General.hpp"

extern "C" std::filesystem::path ResolvePath_nid_no_patch(const char* path) {
    if (path == nullptr) {
        APS5_INVALID_ARG_EX;
    }
    std::string s(path);
    std::size_t start = 0;
    while (start < s.size() && (s[start] == '/' || s[start] == '\\')) {
        ++start;
    }
    std::size_t end = s.size();
    while (end > start && (s[end - 1] == '/' || s[end - 1] == '\\')) {
        --end;
    }
    const std::string relative = s.substr(start, end - start);
#ifndef _WIN32
    // Character devices the guest kernel also provides map to the host's devices of the same name.
    if (relative == "dev/random" || relative == "dev/urandom" || relative == "dev/null" || relative == "dev/zero") {
        return std::filesystem::path("/") / relative;
    }
#endif
    std::filesystem::path result = std::filesystem::current_path() / std::filesystem::path(relative);
    return result.make_preferred();
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
