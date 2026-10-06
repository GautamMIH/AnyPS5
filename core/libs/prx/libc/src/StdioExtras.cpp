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

extern "C" int __cxa_thread_atexit_impl(void (*function)(void*), void* object, void* dso);

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
using ExitFunction = void (APS5_VABI *)(void*);

std::FILE* native(FileStream* stream) {
    return GetNativeStream(stream);
}

bool isCLocale(const char* name) {
    return name == nullptr || name[0] == '\0' || std::strcmp(name, "C") == 0 || std::strcmp(name, "POSIX") == 0;
}

thread_local char* tokenState = nullptr;
std::mt19937_64& rand48Engine() {
    static std::mt19937_64 engine(0x1234abcd330eULL);
    return engine;
}

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

void APS5_VABI _Lockfilelock_nid_postfix(FileStream* stream) {
    flockfile(native(stream));
}

void APS5_VABI _Unlockfilelock_nid_postfix(FileStream* stream) {
    funlockfile(native(stream));
}

FileStream* APS5_VABI fopen_nid_postfix(const char* filename, const char* mode);

int APS5_VABI fopen_s_nid_postfix(FileStream** stream, const char* path, const char* mode) {
    if (stream == nullptr || path == nullptr || mode == nullptr) return kErrorInvalid;
    *stream = fopen_nid_postfix(path, mode);
    return *stream == nullptr ? errno : 0;
}

#ifndef _WIN32

int APS5_VABI snprintf_s_nid_postfix(char* buffer, std::size_t size, const char* format, ...) {
    if (buffer == nullptr || size == 0 || format == nullptr) return -1;
    std::va_list arguments;
    va_start(arguments, format);
    const int result = std::vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return result;
}

int APS5_VABI sprintf_s_nid_postfix(char* buffer, std::size_t size, const char* format, ...) {
    if (buffer == nullptr || size == 0 || format == nullptr) return -1;
    std::va_list arguments;
    va_start(arguments, format);
    const int result = std::vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    if (result < 0 || static_cast<std::size_t>(result) >= size) {
        buffer[0] = '\0';
        return -1;
    }
    return result;
}

#endif

std::size_t APS5_VABI strspn_nid_postfix(const char* text, const char* accept) { return std::strspn(text, accept); }
int APS5_VABI bcmp_nid_postfix(const void* left, const void* right, std::size_t count) { return std::memcmp(left, right, count) == 0 ? 0 : 1; }

int APS5_VABI memcpy_s_nid_postfix(void* destination, std::size_t destinationSize, const void* source, std::size_t count) {
    if (destination == nullptr) return kErrorInvalid;
    if (source == nullptr || count > destinationSize) {
        std::memset(destination, 0, destinationSize);
        return source == nullptr ? kErrorInvalid : kErrorRange;
    }
    std::memmove(destination, source, count);
    return 0;
}

int APS5_VABI memset_s_nid_postfix(void* destination, std::size_t destinationSize, int value, std::size_t count) {
    if (destination == nullptr) return kErrorInvalid;
    std::memset(destination, value, std::min(destinationSize, count));
    return count > destinationSize ? kErrorRange : 0;
}

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

void APS5_VABI srand48_nid_postfix(long seed) {
    rand48Engine().seed(static_cast<std::uint64_t>(seed));
}

long APS5_VABI lrand48_nid_postfix() {
    return static_cast<long>(rand48Engine()() & 0x7fffffffu);
}

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

std::lconv* APS5_VABI localeconv_nid_postfix() {
    static std::lconv conventions = [] {
        std::lconv value = *std::localeconv();
        return value;
    }();
    return &conventions;
}

// quick_exit is defined in Process.cpp (runs guest at_quick_exit callbacks).

int APS5_VABI __cxa_thread_atexit_nid_postfix(ExitFunction function, void* object, void* dso) {
    return __cxa_thread_atexit_impl(reinterpret_cast<void (*)(void*)>(function), object, dso);
}

unsigned int APS5_VABI _ZNSt6thread20hardware_concurrencyEv_nid_postfix() {
    return std::thread::hardware_concurrency();
}

}
