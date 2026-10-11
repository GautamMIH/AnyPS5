#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <lmcons.h>
#else
#include <unistd.h>
#endif

extern "C" int APS5_VABI getlogin_r_nid_postfix(char*, int);
extern "C" int* APS5_VABI __error_nid_postfix();

static void Check(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "getlogin_r check failed at line %d\n", line);
        std::abort();
    }
}
#define Require(value) Check((value), __LINE__)

static int HostLogin(std::array<char, 4096>& output) {
#ifdef _WIN32
    wchar_t name[UNLEN + 1];
    DWORD count = UNLEN + 1;
    Require(GetUserNameW(name, &count) != 0);
    Require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, -1,
        output.data(), static_cast<int>(output.size()), nullptr, nullptr) > 0);
    return 0;
#else
    const int error = ::getlogin_r(output.data(), output.size());
    switch (error) {
        case 0: return 0;
        case ENOENT: return 2;
        case EINTR: return 4;
        case EIO: return 5;
        case ENXIO: return 6;
        case ENOMEM: return 12;
        case EACCES: return 13;
        case ENFILE: return 23;
        case EMFILE: return 24;
        case ENOTTY: return 25;
        case ERANGE: return 34;
        case ENOSYS: return 78;
        default: std::abort();
    }
#endif
}

static void RequireLogin(const std::string& expected) {
    std::vector<char> output(expected.size() + 3, '\x5A');
    *__error_nid_postfix() = 12345;
    Require(getlogin_r_nid_postfix(output.data() + 1, static_cast<int>(expected.size() + 1)) == 0);
    Require(*__error_nid_postfix() == 12345);
    Require(output.front() == '\x5A' && output.back() == '\x5A');
    Require(std::memcmp(output.data() + 1, expected.c_str(), expected.size() + 1) == 0);
    output.assign(output.size(), '\x5A');
    const auto saved = output;
    Require(getlogin_r_nid_postfix(output.data() + 1, static_cast<int>(expected.size())) == 34);
    Require(*__error_nid_postfix() == 12345 && output == saved);
}

int main() {
    std::array<char, 4096> output;
    output.fill('\x5A');
    const auto untouched = output;
    for (int size : {0, -1, std::numeric_limits<int>::min()}) {
        *__error_nid_postfix() = 12345;
        Require(getlogin_r_nid_postfix(output.data(), size) == 34);
        Require(*__error_nid_postfix() == 12345 && output == untouched);
    }
    Require(getlogin_r_nid_postfix(nullptr, 1) == 14);
    Require(*__error_nid_postfix() == 12345);
    std::array<char, 4096> host{};
    const int status = HostLogin(host);
    *__error_nid_postfix() = 12345;
    Require(getlogin_r_nid_postfix(output.data(), static_cast<int>(output.size())) == status);
    Require(*__error_nid_postfix() == 12345);
    if (status != 0) {
        std::fprintf(stderr, "Host has no login name: guest error %d\n", status);
        return 0;
    }
    Require(std::strcmp(output.data(), host.data()) == 0);
    const std::string expected(host.data());
    RequireLogin(expected);
    std::array<std::thread, 4> readers;
    for (auto& reader : readers) reader = std::thread([&] {
        for (int i = 0; i != 20; ++i) RequireLogin(expected);
    });
    for (auto& reader : readers) reader.join();
}
