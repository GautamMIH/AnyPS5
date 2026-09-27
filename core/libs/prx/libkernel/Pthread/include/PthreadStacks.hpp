#ifndef CORE_LIBS_PRX_LIBKERNEL_PTHREAD_PTHREADSTACKS_HPP
#define CORE_LIBS_PRX_LIBKERNEL_PTHREAD_PTHREADSTACKS_HPP

#include <cstddef>
#include <cstdint>

namespace PthreadStacks {

// Lowest address and size of the calling thread's stack, as scePthreadAttrGet reports them.
void CurrentBounds(void** lowest, std::size_t* size);
void RegisterCurrent();
void UnregisterCurrent();
bool Find(std::uintptr_t address, void** start, void** end);

}

#endif
