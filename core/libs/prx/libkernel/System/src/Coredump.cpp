#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

static constexpr int SCE_COREDUMP_ERROR_INVALID_ARGUMENT = static_cast<int>(0x80020016);
static uint64_t registeredCoredumpHandler = 0;
static uint64_t registeredCoredumpContext = 0;

extern "C" {

int APS5_VABI sceCoredumpRegisterCoredumpHandler(uint64_t handler, size_t stack_size, uint64_t context) {
 if (handler == 0) return SCE_COREDUMP_ERROR_INVALID_ARGUMENT;
 (void)stack_size;
 registeredCoredumpHandler = handler;
 registeredCoredumpContext = context;
 return 0;
}

int APS5_VABI sceCoredumpUnregisterCoredumpHandler(void) {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

}
