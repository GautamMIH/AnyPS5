#ifndef CORE_LIBS_PRX_LIBKERNEL_PTHREAD_PTHREADSTACKS_HPP
#define CORE_LIBS_PRX_LIBKERNEL_PTHREAD_PTHREADSTACKS_HPP

#include <cstdint>

namespace PthreadStacks {

void RegisterCurrent();
void UnregisterCurrent();
bool Find(std::uintptr_t address, void** start, void** end);

}

#endif
