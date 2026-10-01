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

std::string utf8FromGuest(const GuestWchar* text, std::size_t limit) {
    std::string result;
    char buffer[4];
    const GuestWchar* cursor = text;
    const GuestWchar* end = text;
    while (static_cast<std::size_t>(end - text) < limit && *end != 0)
        ++end;
    while (cursor < end) {
        char32_t codePoint = 0;
        if (!decodeUtf16(cursor, codePoint))
            codePoint = 0xfffd;
        result.append(buffer, encodeUtf8(codePoint, buffer));
    }
    return result;
}

void appendFormatted(std::string& output, const std::string& specification, auto value) {
    const int length = std::snprintf(nullptr, 0, specification.c_str(), value);
    if (length < 0)
        throw std::runtime_error("vswprintf: invalid conversion " + specification);
    std::string buffer(static_cast<std::size_t>(length) + 1, '\0');
    std::snprintf(buffer.data(), buffer.size(), specification.c_str(), value);
    output.append(buffer.data(), static_cast<std::size_t>(length));
}

int formatWide(GuestWchar* destination, std::size_t count, const GuestWchar* format, std::va_list arguments) {
    if (destination == nullptr || format == nullptr || count == 0) {
        errno = kErrorInvalid;
        return -1;
    }
    std::string output;
    const GuestWchar* cursor = format;
    while (*cursor != 0) {
        if (*cursor != u'%') {
            char32_t codePoint = 0;
            if (!decodeUtf16(cursor, codePoint))
                codePoint = 0xfffd;
            char buffer[4];
            output.append(buffer, encodeUtf8(codePoint, buffer));
            continue;
        }
        ++cursor;
        if (*cursor == u'%') {
            output.push_back('%');
            ++cursor;
            continue;
        }
        std::string flags;
        while (*cursor == u'-' || *cursor == u'+' || *cursor == u' ' || *cursor == u'#' || *cursor == u'0')
            flags.push_back(static_cast<char>(*cursor++));
        std::string width;
        if (*cursor == u'*') {
            width = std::to_string(va_arg(arguments, int));
            ++cursor;
        } else {
            while (*cursor >= u'0' && *cursor <= u'9') width.push_back(static_cast<char>(*cursor++));
        }
        std::string precision;
        bool hasPrecision = false;
        if (*cursor == u'.') {
            hasPrecision = true;
            ++cursor;
            if (*cursor == u'*') {
                precision = std::to_string(va_arg(arguments, int));
                ++cursor;
            } else {
                while (*cursor >= u'0' && *cursor <= u'9') precision.push_back(static_cast<char>(*cursor++));
            }
        }
        std::string length;
        while (*cursor == u'h' || *cursor == u'l' || *cursor == u'j' || *cursor == u'z' || *cursor == u't' || *cursor == u'L' || *cursor == u'q')
            length.push_back(static_cast<char>(*cursor++));
        const GuestWchar conversion = *cursor++;
        const std::string prefix = "%" + flags + width + (hasPrecision ? "." + precision : "");
        switch (conversion) {
        case u'd': case u'i':
            if (length == "hh") appendFormatted(output, prefix + "hhd", static_cast<signed char>(va_arg(arguments, int)));
            else if (length == "h") appendFormatted(output, prefix + "hd", static_cast<short>(va_arg(arguments, int)));
            else if (length == "l") appendFormatted(output, prefix + "ld", va_arg(arguments, long));
            else if (length == "ll" || length == "q" || length == "j") appendFormatted(output, prefix + "lld", va_arg(arguments, long long));
            else if (length == "z" || length == "t") appendFormatted(output, prefix + "ld", va_arg(arguments, long));
            else appendFormatted(output, prefix + "d", va_arg(arguments, int));
            break;
        case u'u': case u'o': case u'x': case u'X': {
            const std::string kind(1, static_cast<char>(conversion));
            if (length == "hh") appendFormatted(output, prefix + "hh" + kind, static_cast<unsigned char>(va_arg(arguments, unsigned int)));
            else if (length == "h") appendFormatted(output, prefix + "h" + kind, static_cast<unsigned short>(va_arg(arguments, unsigned int)));
            else if (length == "l" || length == "z" || length == "t") appendFormatted(output, prefix + "l" + kind, va_arg(arguments, unsigned long));
            else if (length == "ll" || length == "q" || length == "j") appendFormatted(output, prefix + "ll" + kind, va_arg(arguments, unsigned long long));
            else appendFormatted(output, prefix + kind, va_arg(arguments, unsigned int));
            break;
        }
        case u'f': case u'F': case u'e': case u'E': case u'g': case u'G': case u'a': case u'A': {
            const std::string kind(1, static_cast<char>(conversion));
            if (length == "L") appendFormatted(output, prefix + "L" + kind, va_arg(arguments, long double));
            else appendFormatted(output, prefix + kind, va_arg(arguments, double));
            break;
        }
        case u'c':
            if (length == "l") {
                const GuestWchar unit[2] = {static_cast<GuestWchar>(va_arg(arguments, int)), 0};
                appendFormatted(output, "%" + flags + width + "s", utf8FromGuest(unit, 1).c_str());
            } else {
                appendFormatted(output, prefix + "c", va_arg(arguments, int));
            }
            break;
        case u'C': {
            const GuestWchar unit[2] = {static_cast<GuestWchar>(va_arg(arguments, int)), 0};
            appendFormatted(output, "%" + flags + width + "s", utf8FromGuest(unit, 1).c_str());
            break;
        }
        case u's':
            if (length == "l") {
                const auto* text = va_arg(arguments, const GuestWchar*);
                const std::string converted = text == nullptr ? std::string("(null)") : utf8FromGuest(text, hasPrecision ? static_cast<std::size_t>(std::stoul(precision.empty() ? "0" : precision)) : SIZE_MAX);
                appendFormatted(output, "%" + flags + width + "s", converted.c_str());
            } else {
                const char* text = va_arg(arguments, const char*);
                appendFormatted(output, prefix + "s", text == nullptr ? "(null)" : text);
            }
            break;
        case u'S': {
            const auto* text = va_arg(arguments, const GuestWchar*);
            const std::string converted = text == nullptr ? std::string("(null)") : utf8FromGuest(text, hasPrecision ? static_cast<std::size_t>(std::stoul(precision.empty() ? "0" : precision)) : SIZE_MAX);
            appendFormatted(output, "%" + flags + width + "s", converted.c_str());
            break;
        }
        case u'p':
            appendFormatted(output, prefix + "p", va_arg(arguments, void*));
            break;
        case u'n':
            *va_arg(arguments, int*) = static_cast<int>(output.size());
            break;
        default:
            throw std::runtime_error("vswprintf: unsupported conversion");
        }
    }
    std::size_t written = 0;
    const char* source = output.c_str();
    while (*source != '\0') {
        char32_t codePoint = 0;
        if (!decodeUtf8(source, codePoint))
            codePoint = 0xfffd;
        const std::size_t units = codePoint >= 0x10000 ? 2 : 1;
        if (written + units >= count) {
            destination[written] = 0;
            errno = kErrorRange;
            return -1;
        }
        if (units == 2) {
            destination[written] = static_cast<GuestWchar>(0xd800 + ((codePoint - 0x10000) >> 10));
            destination[written + 1] = static_cast<GuestWchar>(0xdc00 + ((codePoint - 0x10000) & 0x3ff));
        } else {
            destination[written] = static_cast<GuestWchar>(codePoint);
        }
        written += units;
    }
    destination[written] = 0;
    return static_cast<int>(written);
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

long double APS5_VABI wcstold_nid_postfix(const GuestWchar* text, GuestWchar** end) {
    // The guest's long double is the x87 80-bit format, as the host's.
    static_assert(sizeof(long double) == 16 && std::numeric_limits<long double>::digits == 64);
    return parseNumber<long double>(text, end, [](const char* narrow, char** narrowEnd) { return std::strtold(narrow, narrowEnd); });
}

unsigned long long APS5_VABI wcstoul_nid_postfix(const GuestWchar* text, GuestWchar** end, int base) {
    return parseNumber<unsigned long long>(text, end, [base](const char* narrow, char** narrowEnd) { return std::strtoull(narrow, narrowEnd, base); });
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
    if (length < count) {
        for (std::size_t index = 0; index <= length; ++index) destination[index] = source[index];
    }
    return length;
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
    const int result = formatWide(destination, count, format, *arguments);
    // Debug aid: APS5_TRACE_FORMAT=1 echoes every formatted wide string (engines format their log
    // and fatal-error text this way even when they print nothing).
    static const bool trace = std::getenv("APS5_TRACE_FORMAT") != nullptr;
    if (trace && destination != nullptr && result > 0) {
        std::string text;
        for (std::size_t i = 0; i < count && destination[i] != 0; ++i) text.push_back(destination[i] < 0x80 ? static_cast<char>(destination[i]) : '?');
        std::fprintf(stderr, "[format] %s\n", text.c_str());
    }
    return result;
}

int APS5_VABI swprintf_nid_postfix(GuestWchar* destination, std::size_t count, const GuestWchar* format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    const int result = formatWide(destination, count, format, arguments);
    va_end(arguments);
    return result;
}

int APS5_VABI _Iswctype_nid_postfix(std::uint32_t character, short description) {
    const bool upper = character >= 'A' && character <= 'Z';
    const bool lower = character >= 'a' && character <= 'z';
    const bool digit = character >= '0' && character <= '9';
    const bool graph = character >= 0x21 && character <= 0x7e;
    switch (description) {
    case 1: return upper || lower || digit;
    case 2: return upper || lower;
    case 3: return character <= 0x1f || character == 0x7f;
    case 4: return digit;
    case 5: return graph;
    case 6: return lower;
    case 7: return character >= 0x20 && character <= 0x7e;
    case 8: return graph && !upper && !lower && !digit;
    case 9: return character == 0x20 || (character >= 0x09 && character <= 0x0d);
    case 10: return upper;
    case 11: return digit || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
    case 12: return character == 0x20 || character == 0x09;
    default: return 0;
    }
}

short APS5_VABI wctype_nid_postfix(const char* name) {
    static constexpr const char* kClasses[] = {"alnum", "alpha", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit", "blank"};
    if (name == nullptr) return 0;
    for (short index = 0; index < static_cast<short>(sizeof(kClasses) / sizeof(kClasses[0])); ++index)
        if (std::strcmp(name, kClasses[index]) == 0) return static_cast<short>(index + 1);
    return 0;
}

int APS5_VABI iswctype_nid_postfix(std::uint32_t character, short description) {
    return _Iswctype_nid_postfix(character, description);
}

}
