#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#include <lmcons.h>
#else
#include <unistd.h>
#endif

namespace {
int ReadLogin(char* output, int size) {
#ifdef _WIN32
    std::array<wchar_t, UNLEN + 1> name{};
    DWORD count = static_cast<DWORD>(name.size());
    if (!GetUserNameW(name.data(), &count))
        throw std::system_error(GetLastError(), std::system_category(), "getlogin_r: GetUserNameW");
    std::array<char, UNLEN * 4 + 1> utf8{};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(),
        static_cast<int>(count), utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
    if (length == 0)
        throw std::system_error(GetLastError(), std::system_category(), "getlogin_r: UTF-8 conversion");
    if (length > size) return 34;
    std::memcpy(output, utf8.data(), static_cast<std::size_t>(length));
    return 0;
#else
    const int error = ::getlogin_r(output, static_cast<std::size_t>(size));
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
        default: throw std::system_error(error, std::generic_category(), "getlogin_r: unsupported host error");
    }
#endif
}
}

extern "C" int APS5_VABI getlogin_r_nid_postfix(char* output, int size) {
    if (size <= 0) return 34;
    if (output == nullptr) return 14;
    const int saved = errno;
    const int result = ReadLogin(output, size);
    errno = saved;
    return result;
}
