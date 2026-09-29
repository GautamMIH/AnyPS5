#include "prx/libSceFont/include/FontTypes.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" {
int APS5_VABI sceFontMemoryInit(FontMemory*, void*, std::uint32_t, const FontMemoryInterface*, void*, FontMemoryDestroyFunction, void*);
int APS5_VABI sceFontMemoryTerm(FontMemory*);
int APS5_VABI sceFontCreateLibrary(const FontMemory*, const void*, FontLibrary*);
int APS5_VABI sceFontDestroyLibrary(FontLibrary*);
int APS5_VABI sceFontCreateRenderer(const FontMemory*, const void*, FontRenderer*);
int APS5_VABI sceFontDestroyRenderer(FontRenderer*);
int APS5_VABI sceFontSupportSystemFonts(FontLibrary);
int APS5_VABI sceFontSupportExternalFonts(FontLibrary, std::uint32_t, std::uint32_t);
int APS5_VABI sceFontOpenFontSet(FontLibrary, std::uint32_t, std::uint32_t, const FontOpenDetail*, FontHandle*);
int APS5_VABI sceFontOpenFontMemory(FontLibrary, const void*, std::uint32_t, const FontOpenDetail*, FontHandle*);
const void* APS5_VABI sceFontSelectLibraryFt(int);
const void* APS5_VABI sceFontSelectRendererFt(int);
}

static void Check(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "Font check failed at line %d\n", line);
        std::abort();
    }
}
#define Require(value) Check((value), __LINE__)

static int allocations = 0;
static void* APS5_VABI Allocate(void*, std::uint32_t size) {
    ++allocations;
    return std::malloc(size);
}
static void APS5_VABI Release(void*, void* pointer) {
    if (pointer) --allocations;
    std::free(pointer);
}

int main() {
    constexpr std::uint32_t SystemFontSet = 0x18070043u;
    const FontMemoryInterface iface{Allocate, Release, nullptr, nullptr, nullptr, nullptr};
    FontMemory memory{};
    Require(sceFontMemoryInit(&memory, nullptr, 0, &iface, nullptr, nullptr, nullptr) == SCE_FONT_OK);
    Require(sceFontSelectLibraryFt(0) != nullptr && sceFontSelectLibraryFt(1) == nullptr);
    Require(sceFontSelectRendererFt(0) != nullptr && sceFontSelectRendererFt(1) == nullptr);

    FontLibrary library = nullptr;
    Require(sceFontCreateLibrary(&memory, nullptr, &library) == SCE_FONT_ERROR_INVALID_PARAMETER && library == nullptr);
    Require(sceFontCreateLibrary(&memory, sceFontSelectLibraryFt(0), &library) == SCE_FONT_OK && library != nullptr);

    FontHandle font = reinterpret_cast<FontHandle>(&memory);
    Require(sceFontOpenFontSet(library, SystemFontSet, 1, nullptr, &font) == SCE_FONT_ERROR_NO_SUPPORT_FUNCTION && font == nullptr);
    Require(sceFontSupportSystemFonts(library) == SCE_FONT_OK);
    Require(sceFontOpenFontSet(library, SystemFontSet, 1, nullptr, &font) == SCE_FONT_ERROR_FONT_OPEN_FAILED && font == nullptr);
    Require(sceFontOpenFontSet(library, 0x12345678u, 1, nullptr, &font) == SCE_FONT_ERROR_NO_SUPPORT_FONTSET);
    Require(sceFontOpenFontSet(library, SystemFontSet, 7, nullptr, &font) == SCE_FONT_ERROR_INVALID_PARAMETER);
    Require(sceFontOpenFontSet(nullptr, SystemFontSet, 1, nullptr, &font) == SCE_FONT_ERROR_INVALID_LIBRARY);
    const unsigned char notAFont[64] = {1, 2, 3, 4};
    Require(sceFontOpenFontMemory(library, notAFont, sizeof(notAFont), nullptr, &font) == SCE_FONT_ERROR_NO_SUPPORT_FUNCTION && font == nullptr);
    Require(sceFontSupportExternalFonts(library, 4, 0x52) == SCE_FONT_OK);
    Require(sceFontSupportExternalFonts(library, 4, 0x52) == SCE_FONT_ERROR_ALREADY_SPECIFIED);
    Require(sceFontOpenFontMemory(library, nullptr, 0, nullptr, &font) == SCE_FONT_ERROR_INVALID_PARAMETER && font == nullptr);
    Require(sceFontOpenFontMemory(library, notAFont, sizeof(notAFont), nullptr, &font) == SCE_FONT_ERROR_NO_SUPPORT_FORMAT && font == nullptr);

    FontRenderer renderer = nullptr;
    Require(sceFontCreateRenderer(&memory, sceFontSelectRendererFt(0), &renderer) == SCE_FONT_OK && renderer != nullptr);
    Require(sceFontDestroyRenderer(&renderer) == SCE_FONT_OK && renderer == nullptr);
    Require(sceFontDestroyRenderer(&renderer) == SCE_FONT_ERROR_INVALID_RENDERER);

    Require(sceFontDestroyLibrary(&library) == SCE_FONT_OK && library == nullptr);
    Require(sceFontDestroyLibrary(&library) == SCE_FONT_ERROR_INVALID_LIBRARY);
    Require(allocations == 0);
    Require(sceFontMemoryTerm(&memory) == SCE_FONT_OK);
    Require(sceFontMemoryTerm(&memory) == SCE_FONT_ERROR_INVALID_MEMORY);
}
