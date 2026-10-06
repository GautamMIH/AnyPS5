#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cwchar>
#include <cstdio>
#include <limits>

#include "prx/libc/include/General.hpp"

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

std::int64_t APS5_VABI strtol_nid_postfix(const char* str, char** endptr, int base) {
    return std::strtoll(str, endptr, base);
}

std::uint64_t APS5_VABI strtoul_nid_postfix(const char* str, char** endptr, int base) {
    return std::strtoull(str, endptr, base);
}

long long APS5_VABI strtoll_nid_postfix(const char* str, char** endptr, int base) {
    return std::strtoll(str, endptr, base);
}

unsigned long long APS5_VABI strtoull_nid_postfix(const char* str, char** endptr, int base) {
    return std::strtoull(str, endptr, base);
}

double APS5_VABI strtod_nid_postfix(const char* str, char** endptr) {
    return std::strtod(str, endptr);
}

double APS5_VABI atof_nid_postfix(const char* str) { return std::atof(str); }
float APS5_VABI strtof_nid_postfix(const char* str, char** endptr) { return std::strtof(str, endptr); }
long double APS5_VABI strtold_nid_postfix(const char* str, char** endptr) {
    static_assert(sizeof(long double) == 16, "Guest long double requires x87 extended precision storage");
    return std::strtold(str, endptr);
}

int APS5_VABI atoi_nid_postfix(const char* str) {
    return std::atoi(str);
}


}


extern "C" {

int APS5_VABI strcasecmp_nid_postfix(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        unsigned char a = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(*s1)));
        unsigned char b = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(*s2)));
        if (a != b) return a - b;
        ++s1; ++s2;
    }
    return static_cast<unsigned char>(*s1) - static_cast<unsigned char>(*s2);
}

int APS5_VABI strncasecmp_nid_postfix(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && *s2) {
        unsigned char a = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(*s1)));
        unsigned char b = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(*s2)));
        if (a != b) return a - b;
        ++s1; ++s2; --n;
    }
    if (!n) return 0;
    return static_cast<unsigned char>(*s1) - static_cast<unsigned char>(*s2);
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

int APS5_VABI memmove_s_nid_postfix(void* dest, size_t destsz, const void* src, size_t count) {
    constexpr int GuestEinval = 22;
    constexpr int GuestErange = 34;
    if (!dest) return GuestEinval;
    if (!src || count > destsz) {
        std::memset(dest, 0, destsz);
        return src ? GuestErange : GuestEinval;
    }
    std::memmove(dest, src, count);
    return 0;
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
    return std::strtoull(str, endptr, base);
}

size_t APS5_VABI strxfrm_nid_postfix(char* destination, const char* source, size_t count) {
    return std::strxfrm(destination, source, count);
}

}
