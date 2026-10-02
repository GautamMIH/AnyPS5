#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

extern "C" char16_t* APS5_VABI wcsstr_nid_postfix(const char16_t* text, const char16_t* pattern);

static void require(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "wide search check failed: %s\n", message);
    std::abort();
}

// wcsstr never reads past the text's terminator: a text ending at the end of its mapping, searched
// for a longer pattern sharing its last characters, must not fault.
int main() {
    const char16_t text[] = u"main menu";
    require(wcsstr_nid_postfix(text, u"menu") == text + 5, "finds a pattern at the end");
    require(wcsstr_nid_postfix(text, u"menus") == nullptr, "a longer pattern is not found");
    require(wcsstr_nid_postfix(text, u"") == text, "an empty pattern matches at the start");
#if !defined(_WIN32)
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    auto* mapping = static_cast<char*>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    require(mapping != MAP_FAILED, "mapping");
    require(mprotect(mapping + page, page, PROT_NONE) == 0, "guard page");
    // "ab" plus its terminator, ending exactly at the guard page.
    auto* tail = reinterpret_cast<char16_t*>(mapping + page) - 3;
    tail[0] = u'a';
    tail[1] = u'b';
    tail[2] = 0;
    require(wcsstr_nid_postfix(tail, u"abcdefgh") == nullptr, "pattern longer than the text at a mapping end");
    require(wcsstr_nid_postfix(tail + 1, u"bcd") == nullptr, "partial match at a mapping end");
    munmap(mapping, page * 2);
#endif
    std::puts("wide search tests passed");
    return 0;
}
