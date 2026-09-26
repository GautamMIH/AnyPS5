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
std::mt19937& randomEngine() {
    static std::mt19937 engine(1);
    return engine;
}
std::mt19937_64& rand48Engine() {
    static std::mt19937_64 engine(0x1234abcd330eULL);
    return engine;
}

}

extern "C" {

extern FileStream _Stdout_nid_postfix;
extern FileStream _Stderr_nid_postfix;
FileStream _Stdin_nid_postfix{stdin};

int APS5_VABI fputc_nid_postfix(int character, FileStream* stream) {
    return std::fputc(character, native(stream));
}

int APS5_VABI fgetc_nid_postfix(FileStream* stream) {
    return std::fgetc(native(stream));
}

int APS5_VABI ungetc_nid_postfix(int character, FileStream* stream) {
    return std::ungetc(character, native(stream));
}

char* APS5_VABI fgets_nid_postfix(char* buffer, int count, FileStream* stream) {
    return std::fgets(buffer, count, native(stream));
}

int APS5_VABI feof_nid_postfix(FileStream* stream) {
    return std::feof(native(stream));
}

int APS5_VABI fseeko_nid_postfix(FileStream* stream, std::int64_t offset, int origin) {
    return fseeko(native(stream), static_cast<off_t>(offset), origin);
}

std::int64_t APS5_VABI ftello_nid_postfix(FileStream* stream) {
    return static_cast<std::int64_t>(ftello(native(stream)));
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

int APS5_VABI setvbuf_nid_postfix(FileStream* stream, char* buffer, int mode, std::size_t size) {
    return std::setvbuf(native(stream), buffer, mode, size);
}

void APS5_VABI _Lockfilelock_nid_postfix(FileStream* stream) {
    flockfile(native(stream));
}

void APS5_VABI _Unlockfilelock_nid_postfix(FileStream* stream) {
    funlockfile(native(stream));
}

FileStream* APS5_VABI freopen_nid_postfix(const char* path, const char* mode, FileStream* stream) {
    if (mode == nullptr) {
        errno = kErrorInvalid;
        return nullptr;
    }
    std::FILE* reopened = path == nullptr ? std::freopen(nullptr, mode, native(stream)) : std::freopen(ResolvePath_nid_no_patch(path).string().c_str(), mode, native(stream));
    return reopened == nullptr ? nullptr : stream;
}

FileStream* APS5_VABI fopen_nid_postfix(const char* filename, const char* mode);

int APS5_VABI fopen_s_nid_postfix(FileStream** stream, const char* path, const char* mode) {
    if (stream == nullptr || path == nullptr || mode == nullptr) return kErrorInvalid;
    *stream = fopen_nid_postfix(path, mode);
    return *stream == nullptr ? errno : 0;
}

void APS5_VABI perror_nid_postfix(const char* prefix) {
    if (prefix != nullptr && prefix[0] != '\0')
        std::fprintf(stderr, "%s: %s\n", prefix, std::strerror(errno));
    else
        std::fprintf(stderr, "%s\n", std::strerror(errno));
}

#ifndef _WIN32

int APS5_VABI fprintf_nid_postfix(FileStream* stream, const char* format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    const int result = std::vfprintf(native(stream), format, arguments);
    va_end(arguments);
    return result;
}

int APS5_VABI vfprintf_nid_postfix(FileStream* stream, const char* format, std::va_list* arguments) {
    return std::vfprintf(native(stream), format, *arguments);
}

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
std::size_t APS5_VABI strcspn_nid_postfix(const char* text, const char* reject) { return std::strcspn(text, reject); }
char* APS5_VABI strncat_nid_postfix(char* destination, const char* source, std::size_t count) { return std::strncat(destination, source, count); }
std::size_t APS5_VABI strnlen_nid_postfix(const char* text, std::size_t count) { return strnlen(text, count); }
int APS5_VABI bcmp_nid_postfix(const void* left, const void* right, std::size_t count) { return std::memcmp(left, right, count) == 0 ? 0 : 1; }

char* APS5_VABI strtok_nid_postfix(char* text, const char* separators) {
    return strtok_r(text, separators, &tokenState);
}

char* APS5_VABI strerror_nid_postfix(int error) {
    return std::strerror(error);
}

int APS5_VABI strerror_r_nid_postfix(int error, char* buffer, std::size_t size) {
    if (buffer == nullptr || size == 0) return kErrorRange;
    const char* message = std::strerror(error);
    const std::size_t length = std::strlen(message);
    const std::size_t copy = std::min(length, size - 1);
    std::memcpy(buffer, message, copy);
    buffer[copy] = '\0';
    return copy == length ? 0 : kErrorRange;
}

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

void* APS5_VABI bsearch_nid_postfix(const void* key, const void* base, std::size_t count, std::size_t size, CompareFunction compare) {
    const auto* bytes = static_cast<const unsigned char*>(base);
    std::size_t low = 0;
    std::size_t high = count;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        const void* element = bytes + middle * size;
        const int order = compare(key, element);
        if (order == 0) return const_cast<void*>(element);
        if (order < 0) high = middle;
        else low = middle + 1;
    }
    return nullptr;
}

int APS5_VABI rand_nid_postfix() {
    return static_cast<int>(randomEngine()() & 0x7fffffffu);
}

void APS5_VABI srand_nid_postfix(unsigned int seed) {
    randomEngine().seed(seed);
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

double APS5_VABI atof_nid_postfix(const char* text) {
    return std::atof(text);
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

[[noreturn]] void APS5_VABI _Exit_nid_postfix(int status) {
    std::_Exit(status);
}

[[noreturn]] void APS5_VABI quick_exit_nid_postfix(int status) {
    std::quick_exit(status);
}

int APS5_VABI __cxa_thread_atexit_nid_postfix(ExitFunction function, void* object, void* dso) {
    return __cxa_thread_atexit_impl(reinterpret_cast<void (*)(void*)>(function), object, dso);
}

unsigned int APS5_VABI _ZNSt6thread20hardware_concurrencyEv_nid_postfix() {
    return std::thread::hardware_concurrency();
}

}
