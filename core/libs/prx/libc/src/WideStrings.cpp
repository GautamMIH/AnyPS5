#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
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

bool decodeUtf8(const char*& cursor, char32_t& codePoint) {
    const auto lead = static_cast<unsigned char>(*cursor++);
    int extra = 0;
    if (lead < 0x80) { codePoint = lead; return true; }
    if ((lead & 0xe0) == 0xc0) { codePoint = lead & 0x1f; extra = 1; }
    else if ((lead & 0xf0) == 0xe0) { codePoint = lead & 0x0f; extra = 2; }
    else if ((lead & 0xf8) == 0xf0) { codePoint = lead & 0x07; extra = 3; }
    else return false;
    for (int index = 0; index < extra; ++index) {
        const auto next = static_cast<unsigned char>(*cursor);
        if ((next & 0xc0) != 0x80) return false;
        codePoint = (codePoint << 6) | (next & 0x3f);
        ++cursor;
    }
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
    for (; *text != 0; ++text)
        if (std::memcmp(text, pattern, patternLength * sizeof(GuestWchar)) == 0 && guestLength(text) >= patternLength)
            return const_cast<GuestWchar*>(text);
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
    return parseNumber<long>(text, end, [base](const char* narrow, char** narrowEnd) { return std::strtol(narrow, narrowEnd, base); });
}

long long APS5_VABI wcstoll_nid_postfix(const GuestWchar* text, GuestWchar** end, int base) {
    return parseNumber<long long>(text, end, [base](const char* narrow, char** narrowEnd) { return std::strtoll(narrow, narrowEnd, base); });
}

unsigned long long APS5_VABI wcstoull_nid_postfix(const GuestWchar* text, GuestWchar** end, int base) {
    return parseNumber<unsigned long long>(text, end, [base](const char* narrow, char** narrowEnd) { return std::strtoull(narrow, narrowEnd, base); });
}

double APS5_VABI wcstod_nid_postfix(const GuestWchar* text, GuestWchar** end) {
    return parseNumber<double>(text, end, [](const char* narrow, char** narrowEnd) { return std::strtod(narrow, narrowEnd); });
}

float APS5_VABI wcstof_nid_postfix(const GuestWchar* text, GuestWchar** end) {
    return parseNumber<float>(text, end, [](const char* narrow, char** narrowEnd) { return std::strtof(narrow, narrowEnd); });
}

std::size_t APS5_VABI mbstowcs_nid_postfix(GuestWchar* destination, const char* source, std::size_t count) {
    std::size_t written = 0;
    const char* cursor = source;
    while (*cursor != '\0') {
        char32_t codePoint = 0;
        if (!decodeUtf8(cursor, codePoint)) {
            errno = kErrorInvalid;
            return static_cast<std::size_t>(-1);
        }
        const std::size_t units = codePoint >= 0x10000 ? 2 : 1;
        if (destination != nullptr) {
            if (written + units > count) return written;
            if (units == 2) {
                destination[written] = static_cast<GuestWchar>(0xd800 + ((codePoint - 0x10000) >> 10));
                destination[written + 1] = static_cast<GuestWchar>(0xdc00 + ((codePoint - 0x10000) & 0x3ff));
            } else {
                destination[written] = static_cast<GuestWchar>(codePoint);
            }
        }
        written += units;
    }
    if (destination != nullptr && written < count)
        destination[written] = 0;
    return written;
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

int APS5_VABI vswprintf_nid_postfix(GuestWchar* destination, std::size_t count, const GuestWchar* format, std::va_list* arguments) {
#ifdef _WIN32
    (void)destination; (void)count; (void)format; (void)arguments;
    NotImplemented_nid_no_patch(__func__);
    return -1;
#else
    if (destination == nullptr || format == nullptr || count == 0) {
        errno = kErrorInvalid;
        return -1;
    }
    std::wstring hostFormat;
    for (const GuestWchar* cursor = format; *cursor != 0; ++cursor) {
        if (cursor[0] == u'%' && (cursor[1] == u'l' || cursor[1] == u'S') && (cursor[1] == u'S' || cursor[2] == u's'))
            throw std::runtime_error("vswprintf: wide string arguments are not implemented");
        hostFormat.push_back(static_cast<wchar_t>(*cursor));
    }
    std::vector<wchar_t> buffer(count);
    const int written = std::vswprintf(buffer.data(), count, hostFormat.c_str(), *arguments);
    const std::size_t copy = written < 0 ? count - 1 : std::min<std::size_t>(static_cast<std::size_t>(written), count - 1);
    for (std::size_t index = 0; index < copy; ++index)
        destination[index] = static_cast<GuestWchar>(buffer[index]);
    destination[copy] = 0;
    if (written < 0 || static_cast<std::size_t>(written) >= count) {
        errno = kErrorRange;
        return -1;
    }
    return written;
#endif
}

}
