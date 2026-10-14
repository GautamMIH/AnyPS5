#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cwchar>
#include <cstdio>
#include <limits>
#include <string>

#include "prx/libc/include/General.hpp"
#include "prx/libc/include/SonyCrt.hpp"

namespace {

bool Contains(const char16_t* set, char16_t character) {
    for (; *set != 0; ++set) {
        if (*set == character) return true;
    }
    return false;
}

bool IsNanSubject(const char* text) {
    while (*text == ' ' || (*text >= '\t' && *text <= '\r')) ++text;
    if (*text == '+' || *text == '-') ++text;
    return *text == 'n' || *text == 'N';
}

double QuietNan(double value, const char* text) {
    const auto bits = std::bit_cast<std::uint64_t>(value);
    if ((bits & 0x7ff0000000000000u) != 0x7ff0000000000000u || !IsNanSubject(text)) return value;
    return std::bit_cast<double>((bits & 0x8000000000000000u) | 0x7ff8000000000000u);
}

float QuietNan(float value, const char* text) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    if ((bits & 0x7f800000u) != 0x7f800000u || !IsNanSubject(text)) return value;
    return std::bit_cast<float>((bits & 0x80000000u) | 0x7fc00000u);
}

long double QuietNan(long double value, const char* text) {
    std::uint16_t exponent;
    std::memcpy(&exponent, reinterpret_cast<const unsigned char*>(&value) + sizeof(std::uint64_t), sizeof(exponent));
    if ((exponent & 0x7fffu) != 0x7fffu || !IsNanSubject(text)) return value;
    const std::uint64_t significand = 0xc000000000000000u;
    std::memcpy(&value, &significand, sizeof(significand));
    return value;
}

template<typename TFloat>
TFloat ParseFloat(const char* text, char** end, TFloat (*parse)(const char*, char**)) {
    char* parsedEnd = nullptr;
    const TFloat value = parse(text, &parsedEnd);
    if (end != nullptr) *end = parsedEnd;
    return parsedEnd == text ? TFloat{0} : QuietNan(value, text);
}

}

extern "C" {

void* APS5_VABI memset_nid_postfix(void* s, int c, size_t n) {
    // APS5_LOG_OUT("s=%p c=%d n=%zu", s, c, n);
    return std::memset(s, c, n);
}

void APS5_VABI bzero_nid_postfix(void* destination, size_t count) {
    std::memset(destination, 0, count);
}

void* APS5_VABI memcpy_nid_postfix(void* dest, const void* src, size_t n) {
    return std::memcpy(dest, src, n);
}

void* APS5_VABI memmove_nid_postfix(void* dest, const void* src, size_t n) {
    return std::memmove(dest, src, n);
}

int APS5_VABI memcmp_nid_postfix(const void* s1, const void* s2, size_t n) {
    return std::memcmp(s1, s2, n);
}

const void* APS5_VABI memchr_nid_postfix(const void* s, int c, size_t n) {
    return std::memchr(s, c, n);
}

void* APS5_VABI memrchr_nid_postfix(const void* s, int c, size_t n) {
    const auto* bytes = static_cast<const unsigned char*>(s);
    const auto value = static_cast<unsigned char>(c);
    while (n != 0) {
        --n;
        if (bytes[n] == value) return const_cast<unsigned char*>(bytes + n);
    }
    return nullptr;
}

int APS5_VABI strcmp_nid_postfix(const char* s1, const char* s2) {
    return std::strcmp(s1, s2);
}

int APS5_VABI strncmp_nid_postfix(const char* s1, const char* s2, size_t n) {
    return std::strncmp(s1, s2, n);
}

size_t APS5_VABI strlen_nid_postfix(const char* s) {
    return std::strlen(s);
}

char* APS5_VABI strcpy_nid_postfix(char* dest, const char* src) {
    return std::strcpy(dest, src);
}

char* APS5_VABI strncpy_nid_postfix(char* dest, const char* src, size_t count) {
    return std::strncpy(dest, src, count);
}

char* APS5_VABI strcat_nid_postfix(char* dest, const char* src) {
    return std::strcat(dest, src);
}

const char* APS5_VABI strchr_nid_postfix(const char* s, int c) {
    return std::strchr(s, c);
}

char* APS5_VABI strrchr_nid_postfix(const char* s, int c) {
    return std::strrchr(const_cast<char*>(s), c);
}

char* APS5_VABI strstr_nid_postfix(const char* haystack, const char* needle) {
    return std::strstr(const_cast<char*>(haystack), needle);
}

size_t APS5_VABI strlcpy_nid_postfix(char* dest, const char* src, size_t size) {
    const size_t srcLen = std::strlen(src);
    if (size != 0u) {
        const size_t copyLen = srcLen < size - 1u ? srcLen : size - 1u;
        std::memcpy(dest, src, copyLen);
        dest[copyLen] = '\0';
    }
    return srcLen;
}

bool StopAtPrefixLetter_nid_no_patch(const char* str, char** endptr, int base) {
    if (base != 0 && base != 2 && base != 16) return false;
    while (*str == ' ' || (*str >= '\t' && *str <= '\r')) ++str;
    if (*str == '+' || *str == '-') ++str;
    if (str[0] != '0' || str[1] == '\0') return false;
    const char next = str[2];
    const bool binary = base != 16 && (str[1] == 'b' || str[1] == 'B') && (next == '0' || next == '1');
    const bool hexDigit = (next >= '0' && next <= '9') || (next >= 'a' && next <= 'f') || (next >= 'A' && next <= 'F');
    const bool bareHex = base != 2 && (str[1] == 'x' || str[1] == 'X') && !hexDigit;
    if (!binary && !bareHex) return false;
    if (endptr) *endptr = const_cast<char*>(str + 1);
    return true;
}

std::int64_t APS5_VABI strtol_nid_postfix(const char* str, char** endptr, int base) {
    if (StopAtPrefixLetter_nid_no_patch(str, endptr, base)) return 0;
    return std::strtoll(str, endptr, base);
}

std::uint64_t APS5_VABI strtoul_nid_postfix(const char* str, char** endptr, int base) {
    if (StopAtPrefixLetter_nid_no_patch(str, endptr, base)) return 0;
    return std::strtoull(str, endptr, base);
}

long long APS5_VABI strtoll_nid_postfix(const char* str, char** endptr, int base) {
    if (StopAtPrefixLetter_nid_no_patch(str, endptr, base)) return 0;
    return std::strtoll(str, endptr, base);
}

unsigned long long APS5_VABI strtoull_nid_postfix(const char* str, char** endptr, int base) {
    if (StopAtPrefixLetter_nid_no_patch(str, endptr, base)) return 0;
    return std::strtoull(str, endptr, base);
}

double APS5_VABI strtod_nid_postfix(const char* str, char** endptr) {
    return ParseFloat<double>(str, endptr, std::strtod);
}

double APS5_VABI atof_nid_postfix(const char* str) { return strtod_nid_postfix(str, nullptr); }
float APS5_VABI strtof_nid_postfix(const char* str, char** endptr) {
    return ParseFloat<float>(str, endptr, std::strtof);
}
long double APS5_VABI strtold_nid_postfix(const char* str, char** endptr) {
    static_assert(sizeof(long double) == 16, "Guest long double requires x87 extended precision storage");
    return ParseFloat<long double>(str, endptr, std::strtold);
}

int APS5_VABI atoi_nid_postfix(const char* str) {
    return static_cast<int>(strtol_nid_postfix(str, nullptr, 10));
}

std::int64_t APS5_VABI atol_nid_postfix(const char* str) {
    return std::strtoll(str, nullptr, 10);
}

long long APS5_VABI atoll_nid_postfix(const char* str) {
    return std::strtoll(str, nullptr, 10);
}


}


extern "C" {

int APS5_VABI strcasecmp_nid_postfix(const char* s1, const char* s2) {
    for (;; ++s1, ++s2) {
        const int a = std::tolower(static_cast<unsigned char>(*s1));
        const int b = std::tolower(static_cast<unsigned char>(*s2));
        if (a != b || *s1 == 0) return a - b;
    }
}

int APS5_VABI strncasecmp_nid_postfix(const char* s1, const char* s2, size_t n) {
    for (; n != 0; ++s1, ++s2, --n) {
        const int a = std::tolower(static_cast<unsigned char>(*s1));
        const int b = std::tolower(static_cast<unsigned char>(*s2));
        if (a != b) return a - b;
        if (*s1 == 0) break;
    }
    return 0;
}

int APS5_VABI bcmp_nid_postfix(const void* s1, const void* s2, size_t n) {
    return std::memcmp(s1, s2, n);
}

size_t APS5_VABI strspn_nid_postfix(const char* s, const char* accept) {
    return std::strspn(s, accept);
}

int APS5_VABI strcoll_nid_postfix(const char* s1, const char* s2) {
    return std::strcmp(s1, s2);
}

int APS5_VABI strncat_s_nid_postfix(char* dest, size_t destsz, const char* src, size_t count) {
    constexpr int GuestEinval = 22;
    constexpr int GuestErange = 34;
    if (!dest || destsz == 0) return GuestEinval;
    size_t used = 0;
    while (used < destsz && dest[used] != '\0') ++used;
    if (used == destsz || !src) {
        dest[0] = '\0';
        return used == destsz ? GuestErange : GuestEinval;
    }
    size_t length = 0;
    while (length < count && src[length] != '\0') ++length;
    if (length >= destsz - used) {
        dest[0] = '\0';
        return GuestErange;
    }
    std::memcpy(dest + used, src, length);
    dest[used + length] = '\0';
    return 0;
}

int APS5_VABI strcat_s_nid_postfix(char* dest, size_t destsz, const char* src) {
    return strncat_s_nid_postfix(dest, destsz, src, static_cast<size_t>(-1));
}

int APS5_VABI memcpy_s_nid_postfix(void* dest, size_t destsz, const void* src, size_t count) {
    constexpr int GuestEinval = 22;
    constexpr int GuestErange = 34;
    constexpr size_t RsizeMax = SIZE_MAX >> 1;
    if (!dest) return GuestEinval;
    if (destsz > RsizeMax) return GuestErange;
    if (!src || count > destsz) {
        std::memset(dest, 0, destsz);
        return src ? GuestErange : GuestEinval;
    }
    const auto destination = reinterpret_cast<std::uintptr_t>(dest);
    const auto source = reinterpret_cast<std::uintptr_t>(src);
    const auto distance = destination < source ? source - destination : destination - source;
    if (count != 0 && distance < count) {
        std::memset(dest, 0, destsz);
        return GuestEinval;
    }
    std::memcpy(dest, src, count);
    return 0;
}

int APS5_VABI memmove_s_nid_postfix(void* dest, size_t destsz, const void* src, size_t count) {
    constexpr int GuestEinval = 22;
    constexpr int GuestErange = 34;
    constexpr size_t RsizeMax = SIZE_MAX >> 1;
    if (!dest) return GuestEinval;
    if (destsz > RsizeMax) return GuestErange;
    if (!src || count > destsz) {
        std::memset(dest, 0, destsz);
        return src ? GuestErange : GuestEinval;
    }
    std::memmove(dest, src, count);
    return 0;
}

int APS5_VABI memset_s_nid_postfix(void* dest, size_t destsz, int value, size_t count) {
    constexpr int GuestEinval = 22;
    constexpr size_t RsizeMax = SIZE_MAX >> 1;
    if (!dest || destsz > RsizeMax || count > RsizeMax) return GuestEinval;
    const auto length = count > destsz ? destsz : count;
    auto* bytes = static_cast<volatile unsigned char*>(dest);
    for (size_t index = 0; index < length; ++index) bytes[index] = static_cast<unsigned char>(value);
    return count > destsz ? GuestEinval : 0;
}

char* APS5_VABI strnstr_nid_postfix(const char* haystack, const char* needle, size_t length) {
    const size_t needleLength = std::strlen(needle);
    if (needleLength == 0) return const_cast<char*>(haystack);
    for (size_t index = 0; index < length && haystack[index] != '\0'; ++index) {
        if (needleLength > length - index) break;
        if (std::strncmp(haystack + index, needle, needleLength) == 0) return const_cast<char*>(haystack + index);
    }
    return nullptr;
}

unsigned long long APS5_VABI _Stoull_nid_postfix(const char* str, char** endptr, int base) {
    if (StopAtPrefixLetter_nid_no_patch(str, endptr, base)) return 0;
    return std::strtoull(str, endptr, base);
}

// The other wide string functions are in WideStrings.cpp.
size_t APS5_VABI wcsnlen_s_nid_postfix(const char16_t* s, size_t maxlen) {
    if (!s) return 0;
    size_t length = 0;
    while (length < maxlen && s[length] != 0) ++length;
    return length;
}

char16_t* APS5_VABI wcstok_nid_postfix(char16_t* text, const char16_t* delimiters, char16_t** last) {
    if (text == nullptr) text = *last;
    if (text == nullptr) return nullptr;
    while (*text != 0 && Contains(delimiters, *text)) ++text;
    if (*text == 0) {
        *last = nullptr;
        return nullptr;
    }
    char16_t* end = text;
    while (*end != 0 && !Contains(delimiters, *end)) ++end;
    if (*end == 0) {
        *last = nullptr;
    } else {
        *end = 0;
        *last = end + 1;
    }
    return text;
}

size_t APS5_VABI strxfrm_nid_postfix(char* destination, const char* source, size_t count) {
    return strlcpy_nid_postfix(destination, source, count);
}

char* APS5_VABI strtok_s_nid_postfix(char* str, const char* delim, char** context) {
    return SonyStrtokS(str, delim, context);
}

}
