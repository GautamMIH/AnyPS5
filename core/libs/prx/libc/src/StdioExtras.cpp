#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <random>
#include <thread>

#include "prx/libc/include/FileStream.hpp"
#include "prx/libc/include/ApplicationHeap.hpp"
#include "prx/libc/include/General.hpp"
#include "SceTypes.hpp"
#include "prx/libc/include/FileStream.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr int kErrorInvalid = 22;
constexpr int kErrorRange = 34;
constexpr long kClocksPerSecond = 1000000;
constexpr int kBsdLcAll = 0;
constexpr int kBsdLcCollate = 1;
constexpr int kBsdLcCtype = 2;
constexpr int kBsdLcMonetary = 3;
constexpr int kBsdLcNumeric = 4;
constexpr int kBsdLcTime = 5;
constexpr int kBsdLcMessages = 6;

using CompareFunction = int (APS5_VABI *)(const void*, const void*);

std::FILE* native(FileStream* stream) {
    return GetNativeStream(stream);
}

bool isCLocale(const char* name) {
    return name == nullptr || name[0] == '\0' || std::strcmp(name, "C") == 0 || std::strcmp(name, "POSIX") == 0;
}

thread_local char* tokenState = nullptr;

}

extern "C" {

extern FileStream _Stdout_nid_postfix;
extern FileStream _Stderr_nid_postfix;
FileStream _Stdin_nid_postfix{stdin};

// std::nothrow: an empty tag object whose address guest code passes to the nothrow operators.
unsigned char _ZSt7nothrow_nid_postfix = 0;

[[noreturn]] void APS5_VABI _Assert_nid_postfix(const char* message, const char* location) {
    std::fprintf(stderr, "[libc] guest assertion failed: %s (%s)\n", message ? message : "?", location ? location : "?");
    std::fflush(stderr);
    std::abort();
}

int APS5_VABI fgetpos_nid_postfix(FileStream* stream, std::int64_t* position) {
    if (position == nullptr) {
        errno = kErrorInvalid;
        return -1;
    }
    const auto current = ftello(native(stream));
    if (current < 0) return -1;
    *position = static_cast<std::int64_t>(current);
    return 0;
}

int APS5_VABI fsetpos_nid_postfix(FileStream* stream, const std::int64_t* position) {
    if (position == nullptr) {
        errno = kErrorInvalid;
        return -1;
    }
    return fseeko(native(stream), static_cast<off_t>(*position), SEEK_SET);
}

// _Lockfilelock, _Unlockfilelock, flockfile, funlockfile and ftrylockfile are in RuntimeSupport.cpp.

FileStream* APS5_VABI fopen_nid_postfix(const char* filename, const char* mode);

int APS5_VABI fopen_s_nid_postfix(FileStream** stream, const char* path, const char* mode) {
    if (stream == nullptr) return kErrorInvalid;
    *stream = nullptr;
    if (path == nullptr || mode == nullptr) return kErrorInvalid;
    *stream = fopen_nid_postfix(path, mode);
    return *stream == nullptr ? errno : 0;
}

// snprintf_s and sprintf_s are in Formatting.cpp (guest wide-string arguments need its formatter);
// strspn, bcmp, memcpy_s and memset_s are in Strings.cpp.

int APS5_VABI strcpy_s_nid_postfix(char* destination, std::size_t destinationSize, const char* source) {
    if (destination == nullptr || destinationSize == 0) return kErrorInvalid;
    if (source == nullptr) {
        destination[0] = '\0';
        return kErrorInvalid;
    }
    const std::size_t length = strnlen(source, destinationSize);
    if (length == destinationSize) {
        destination[0] = '\0';
        return kErrorRange;
    }
    std::memcpy(destination, source, length + 1);
    return 0;
}

int APS5_VABI strncpy_s_nid_postfix(char* destination, std::size_t destinationSize, const char* source, std::size_t count) {
    if (destination == nullptr || destinationSize == 0) return kErrorInvalid;
    if (source == nullptr) {
        destination[0] = '\0';
        return kErrorInvalid;
    }
    const std::size_t length = strnlen(source, count);
    if (length >= destinationSize) {
        destination[0] = '\0';
        return kErrorRange;
    }
    std::memcpy(destination, source, length);
    destination[length] = '\0';
    return 0;
}

// srand48 and lrand48 are in Random.cpp (the 48-bit generator FreeBSD uses).

std::div_t APS5_VABI div_nid_postfix(int numerator, int denominator) {
    return std::div(numerator, denominator);
}

long APS5_VABI clock_nid_postfix() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() * kClocksPerSecond / 1000000);
}

std::tm* APS5_VABI gmtime_nid_postfix(const std::int64_t* time) {
    if (time == nullptr) return nullptr;
    thread_local std::tm result{};
    const std::time_t value = static_cast<std::time_t>(*time);
    return gmtime_r(&value, &result);
}

char* APS5_VABI setlocale_nid_postfix(int category, const char* locale) {
    static char cLocale[] = "C";
    if (category < kBsdLcAll || category > kBsdLcMessages) return nullptr;
    return isCLocale(locale) ? cLocale : nullptr;
}

// localeconv is in LocaleSupport.cpp (it returns the guest's struct lconv layout).

// quick_exit is defined in Process.cpp (runs guest at_quick_exit callbacks).

// __cxa_thread_atexit is in CxxAbiSupport.cpp.

unsigned int APS5_VABI _ZNSt6thread20hardware_concurrencyEv_nid_postfix() {
    return std::thread::hardware_concurrency();
}

// The sized aligned delete is defined with the other operators in NewDelete.cpp.

}
