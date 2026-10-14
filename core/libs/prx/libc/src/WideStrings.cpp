#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <cstdarg>

#include "prx/libc/include/General.hpp"

namespace {

using GuestWchar = char16_t;

constexpr int kErrorInvalid = 22;
constexpr int kErrorRange = 34;

std::size_t guestLength(const GuestWchar* text) {
    std::size_t length = 0;
    while (text[length] != 0)
        ++length;
    return length;
}

std::string narrowForParsing(const GuestWchar* text) {
    std::string result;
    for (std::size_t index = 0; text[index] != 0; ++index)
        result.push_back(text[index] < 0x80 ? static_cast<char>(text[index]) : '\x7f');
    return result;
}

template<typename TValue, typename TParse>
TValue parseNumber(const GuestWchar* text, GuestWchar** end, TParse parse) {
    const std::string narrow = narrowForParsing(text);
    char* narrowEnd = nullptr;
    const TValue value = parse(narrow.c_str(), &narrowEnd);
    if (end != nullptr)
        *end = const_cast<GuestWchar*>(text + (narrowEnd - narrow.c_str()));
    return value;
}

// FreeBSD's libc ignores a NaN's n-char-sequence: every parsed NaN is the default quiet NaN carrying
// the subject's sign (glibc would move the payload into the significand). Same rule as Strings.cpp.
bool isNanSubject(const char* text) {
    while (*text == ' ' || (*text >= '\t' && *text <= '\r')) ++text;
    if (*text == '+' || *text == '-') ++text;
    return *text == 'n' || *text == 'N';
}

double quietNan(double value, const char* text) {
    const auto bits = std::bit_cast<std::uint64_t>(value);
    if ((bits & 0x7ff0000000000000u) != 0x7ff0000000000000u || !isNanSubject(text)) return value;
    return std::bit_cast<double>((bits & 0x8000000000000000u) | 0x7ff8000000000000u);
}

float quietNan(float value, const char* text) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    if ((bits & 0x7f800000u) != 0x7f800000u || !isNanSubject(text)) return value;
    return std::bit_cast<float>((bits & 0x80000000u) | 0x7fc00000u);
}

long double quietNan(long double value, const char* text) {
    std::uint16_t exponent;
    std::memcpy(&exponent, reinterpret_cast<const unsigned char*>(&value) + sizeof(std::uint64_t), sizeof(exponent));
    if ((exponent & 0x7fffu) != 0x7fffu || !isNanSubject(text)) return value;
    const std::uint64_t significand = 0xc000000000000000u;
    std::memcpy(&value, &significand, sizeof(significand));
    return value;
}

template<typename TFloat>
TFloat parseFloat(const char* text, char** end, TFloat (*parse)(const char*, char**)) {
    const TFloat value = parse(text, end);
    return *end == text ? TFloat{0} : quietNan(value, text);
}

std::size_t encodeUtf8(char32_t codePoint, char* output) {
    if (codePoint < 0x80) {
        output[0] = static_cast<char>(codePoint);
        return 1;
    }
    if (codePoint < 0x800) {
        output[0] = static_cast<char>(0xc0 | (codePoint >> 6));
        output[1] = static_cast<char>(0x80 | (codePoint & 0x3f));
        return 2;
    }
    if (codePoint < 0x10000) {
        output[0] = static_cast<char>(0xe0 | (codePoint >> 12));
        output[1] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
        output[2] = static_cast<char>(0x80 | (codePoint & 0x3f));
        return 3;
    }
    output[0] = static_cast<char>(0xf0 | (codePoint >> 18));
    output[1] = static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f));
    output[2] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
    output[3] = static_cast<char>(0x80 | (codePoint & 0x3f));
    return 4;
}

bool decodeUtf16(const GuestWchar*& cursor, char32_t& codePoint) {
    const char32_t first = *cursor++;
    if (first < 0xd800 || first > 0xdfff) {
        codePoint = first;
        return true;
    }
    if (first > 0xdbff || *cursor < 0xdc00 || *cursor > 0xdfff)
        return false;
    codePoint = 0x10000 + ((first - 0xd800) << 10) + (static_cast<char32_t>(*cursor++) - 0xdc00);
    return true;
}

std::size_t toUtf8(char* destination, const GuestWchar** source, std::size_t limit) {
    std::size_t written = 0;
    const GuestWchar* cursor = *source;
    char buffer[4];
    while (true) {
        const GuestWchar* start = cursor;
        char32_t codePoint = 0;
        if (!decodeUtf16(cursor, codePoint)) {
            errno = kErrorInvalid;
            *source = start;
            return static_cast<std::size_t>(-1);
        }
        const std::size_t size = codePoint == 0 ? 1 : encodeUtf8(codePoint, buffer);
        if (destination != nullptr && written + (codePoint == 0 ? 0 : size) > limit) {
            *source = start;
            return written;
        }
        if (codePoint == 0) {
            if (destination != nullptr && written < limit)
                destination[written] = '\0';
            *source = nullptr;
            return written;
        }
        if (destination != nullptr)
            std::memcpy(destination + written, buffer, size);
        written += size;
    }
}

}

extern "C" {

std::size_t APS5_VABI wcslen_nid_postfix(const GuestWchar* text) {
    return guestLength(text);
}

int APS5_VABI wcscmp_nid_postfix(const GuestWchar* left, const GuestWchar* right) {
    while (*left != 0 && *left == *right) { ++left; ++right; }
    return static_cast<int>(*left) - static_cast<int>(*right);
}

int APS5_VABI wcsncmp_nid_postfix(const GuestWchar* left, const GuestWchar* right, std::size_t count) {
    for (; count != 0; --count, ++left, ++right) {
        if (*left != *right || *left == 0)
            return static_cast<int>(*left) - static_cast<int>(*right);
    }
    return 0;
}

GuestWchar* APS5_VABI wcscpy_nid_postfix(GuestWchar* destination, const GuestWchar* source) {
    GuestWchar* cursor = destination;
    while ((*cursor++ = *source++) != 0) {}
    return destination;
}

GuestWchar* APS5_VABI wcsncpy_nid_postfix(GuestWchar* destination, const GuestWchar* source, std::size_t count) {
    std::size_t index = 0;
    for (; index < count && source[index] != 0; ++index)
        destination[index] = source[index];
    for (; index < count; ++index)
        destination[index] = 0;
    return destination;
}

GuestWchar* APS5_VABI wcscat_nid_postfix(GuestWchar* destination, const GuestWchar* source) {
    wcscpy_nid_postfix(destination + guestLength(destination), source);
    return destination;
}

GuestWchar* APS5_VABI wcschr_nid_postfix(const GuestWchar* text, GuestWchar value) {
    for (;; ++text) {
        if (*text == value) return const_cast<GuestWchar*>(text);
        if (*text == 0) return nullptr;
    }
}

GuestWchar* APS5_VABI wcsrchr_nid_postfix(const GuestWchar* text, GuestWchar value) {
    const GuestWchar* found = nullptr;
    for (;; ++text) {
        if (*text == value) found = text;
        if (*text == 0) return const_cast<GuestWchar*>(found);
    }
}

GuestWchar* APS5_VABI wcsstr_nid_postfix(const GuestWchar* text, const GuestWchar* pattern) {
    const std::size_t patternLength = guestLength(pattern);
    if (patternLength == 0) return const_cast<GuestWchar*>(text);
    // Compared one character at a time: a mismatch (the text's terminator included) stops before
    // reading past the end of the text, which may be the end of its mapping.
    for (; *text != 0; ++text) {
        std::size_t matched = 0;
        while (matched < patternLength && text[matched] == pattern[matched]) ++matched;
        if (matched == patternLength) return const_cast<GuestWchar*>(text);
    }
    return nullptr;
}

const GuestWchar* APS5_VABI wmemchr_nid_postfix(const GuestWchar* text, GuestWchar value, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index)
        if (text[index] == value) return text + index;
    return nullptr;
}

int APS5_VABI wmemcmp_nid_postfix(const GuestWchar* left, const GuestWchar* right, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index)
        if (left[index] != right[index]) return left[index] < right[index] ? -1 : 1;
    return 0;
}

GuestWchar* APS5_VABI wmemcpy_nid_postfix(GuestWchar* destination, const GuestWchar* source, std::size_t count) {
    std::memcpy(destination, source, count * sizeof(GuestWchar));
    return destination;
}

GuestWchar* APS5_VABI wmemmove_nid_postfix(GuestWchar* destination, const GuestWchar* source, std::size_t count) {
    std::memmove(destination, source, count * sizeof(GuestWchar));
    return destination;
}

long APS5_VABI wcstol_nid_postfix(const GuestWchar* text, GuestWchar** end, int base) {
    return parseNumber<long>(text, end, [base](const char* narrow, char** narrowEnd) { return StopAtPrefixLetter_nid_no_patch(narrow, narrowEnd, base) ? 0L : std::strtol(narrow, narrowEnd, base); });
}

long long APS5_VABI wcstoll_nid_postfix(const GuestWchar* text, GuestWchar** end, int base) {
    return parseNumber<long long>(text, end, [base](const char* narrow, char** narrowEnd) { return StopAtPrefixLetter_nid_no_patch(narrow, narrowEnd, base) ? 0LL : std::strtoll(narrow, narrowEnd, base); });
}

unsigned long long APS5_VABI wcstoull_nid_postfix(const GuestWchar* text, GuestWchar** end, int base) {
    return parseNumber<unsigned long long>(text, end, [base](const char* narrow, char** narrowEnd) { return StopAtPrefixLetter_nid_no_patch(narrow, narrowEnd, base) ? 0ULL : std::strtoull(narrow, narrowEnd, base); });
}

double APS5_VABI wcstod_nid_postfix(const GuestWchar* text, GuestWchar** end) {
    return parseNumber<double>(text, end, [](const char* narrow, char** narrowEnd) { return parseFloat<double>(narrow, narrowEnd, std::strtod); });
}

long double APS5_VABI wcstold_nid_postfix(const GuestWchar* text, GuestWchar** end) {
    // The guest's long double is the x87 80-bit format, as the host's.
    static_assert(sizeof(long double) == 16 && std::numeric_limits<long double>::digits == 64);
    return parseNumber<long double>(text, end, [](const char* narrow, char** narrowEnd) { return parseFloat<long double>(narrow, narrowEnd, std::strtold); });
}

unsigned long long APS5_VABI wcstoul_nid_postfix(const GuestWchar* text, GuestWchar** end, int base) {
    return parseNumber<unsigned long long>(text, end, [base](const char* narrow, char** narrowEnd) { return StopAtPrefixLetter_nid_no_patch(narrow, narrowEnd, base) ? 0ULL : std::strtoull(narrow, narrowEnd, base); });
}

std::uint64_t APS5_VABI _WStoul_nid_postfix(const GuestWchar* text, GuestWchar** end, int base) {
    if (base < 0 || base == 1 || base > 36) throw std::invalid_argument("_WStoul: unsupported base " + std::to_string(base));
    return wcstoul_nid_postfix(text, end, base);
}

// The guest's wide characters are UTF-16 units (char16_t), not the host's 32-bit wchar_t, so these
// work on units; collation and transformation are those of the C locale.
const GuestWchar* APS5_VABI wcspbrk_nid_postfix(const GuestWchar* text, const GuestWchar* accept) {
    for (; *text != 0; ++text) {
        for (const auto* candidate = accept; *candidate != 0; ++candidate) {
            if (*candidate == *text) return text;
        }
    }
    return nullptr;
}

std::size_t APS5_VABI wcsspn_nid_postfix(const GuestWchar* text, const GuestWchar* accept) {
    std::size_t length = 0;
    for (; text[length] != 0; ++length) {
        bool found = false;
        for (const auto* candidate = accept; *candidate != 0 && !found; ++candidate) found = *candidate == text[length];
        if (!found) break;
    }
    return length;
}

GuestWchar* APS5_VABI wmemset_nid_postfix(GuestWchar* destination, GuestWchar value, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) destination[index] = value;
    return destination;
}

int APS5_VABI wcscoll_nid_postfix(const GuestWchar* first, const GuestWchar* second) {
    while (*first != 0 && *first == *second) {
        ++first;
        ++second;
    }
    return *first < *second ? -1 : *first > *second ? 1 : 0;
}

std::size_t APS5_VABI wcsxfrm_nid_postfix(GuestWchar* destination, const GuestWchar* source, std::size_t count) {
    std::size_t length = 0;
    while (source[length] != 0) ++length;
    // A short buffer receives the truncated transformation: count - 1 units and a terminator.
    if (count != 0) {
        const std::size_t copied = length < count ? length : count - 1;
        for (std::size_t index = 0; index < copied; ++index) destination[index] = source[index];
        destination[copied] = 0;
    }
    return length;
}

float APS5_VABI wcstof_nid_postfix(const GuestWchar* text, GuestWchar** end) {
    return parseNumber<float>(text, end, [](const char* narrow, char** narrowEnd) { return parseFloat<float>(narrow, narrowEnd, std::strtof); });
}

std::size_t APS5_VABI wcstombs_nid_postfix(char* destination, const GuestWchar* source, std::size_t count) {
    const GuestWchar* cursor = source;
    return toUtf8(destination, &cursor, count);
}

std::size_t APS5_VABI wcsrtombs_nid_postfix(char* destination, const GuestWchar** source, std::size_t count, void*) {
    if (source == nullptr || *source == nullptr) {
        errno = kErrorInvalid;
        return static_cast<std::size_t>(-1);
    }
    if (destination == nullptr) {
        const GuestWchar* cursor = *source;
        return toUtf8(nullptr, &cursor, 0);
    }
    return toUtf8(destination, source, count);
}

// wctype and iswctype use the rune locale's masks (RuneLocale.cpp).

}
