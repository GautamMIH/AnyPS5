#pragma once

// Linux only: catches exceptions raised through libc.prx's guest C++ runtime in a guest frame
// (GuestTry.S explains why a host try/catch cannot).
#include <cstddef>
#include <cstdint>
#include <typeinfo>

extern "C" {
extern const std::type_info* GuestTryCatchType;
extern std::uint64_t GuestTryCaughtWord;
extern void (*GuestTryInspect)(void* object);
int GuestTry(const void* function, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c);
}

// Calls function(a, b, c) (up to three integer or pointer arguments) and reports whether it threw
// an exception of exactly the given type (null: any exception).
template<typename TFunction, typename... TArguments>
bool GuestThrows(const std::type_info* type, TFunction function, TArguments... arguments) {
    static_assert(sizeof...(TArguments) <= 3);
    std::uintptr_t words[3] {};
    std::size_t index = 0;
    ((words[index++] = (std::uintptr_t)(arguments)), ...);
    GuestTryCatchType = type;
    return GuestTry(reinterpret_cast<const void*>(function), words[0], words[1], words[2]) == 1;
}
