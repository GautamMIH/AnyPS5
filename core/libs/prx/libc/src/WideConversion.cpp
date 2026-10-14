#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/SonyCrt.hpp"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>

// wcstombs lives in WideStrings.cpp here (UTF-8 conversion like mbstowcs), so only upstream's
// Sony CRT constraint-handler exports and wcscpy_s are kept in this file.

extern "C" void APS5_VABI ignore_handler_s_nid_postfix(const char* message, void* pointer, int error) {
    SonyIgnoreHandlerS(message, pointer, error);
}

extern "C" void* APS5_VABI set_constraint_handler_s_nid_postfix(void* handler) {
    return SonySetConstraintHandlerS(handler);
}

extern "C" int APS5_VABI wcscpy_s_nid_postfix(std::uint16_t* dest, std::size_t size, const std::uint16_t* src) {
    return SonyWcscpyS(dest, size, src);
}
