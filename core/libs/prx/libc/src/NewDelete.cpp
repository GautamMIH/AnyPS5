#include <atomic>
#include <cstddef>
#include <new>

#include "prx/libc/include/ApplicationHeap.hpp"
#include "prx/libc/include/General.hpp"

namespace {

using NewHandler = void (APS5_VABI *)();

std::atomic<NewHandler> currentNewHandler{nullptr};

}

extern "C" {

[[noreturn]] void APS5_VABI _ZSt11_Xbad_allocv_nid_postfix();

}

namespace {

void* tryAllocate(std::size_t bytes, std::size_t alignment) {
    try {
        return alignment == 0 ? ApplicationHeapAllocate_nid_no_patch(bytes == 0 ? 1 : bytes) : ApplicationHeapAlign_nid_no_patch(alignment, bytes == 0 ? 1 : bytes);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void* allocateOrThrow(std::size_t bytes, std::size_t alignment) {
    for (;;) {
        if (void* pointer = tryAllocate(bytes, alignment))
            return pointer;
        const NewHandler handler = currentNewHandler.load();
        if (handler == nullptr)
            _ZSt11_Xbad_allocv_nid_postfix();
        handler();
    }
}

void* allocateOrNull(std::size_t bytes, std::size_t alignment) {
    for (;;) {
        if (void* pointer = tryAllocate(bytes, alignment))
            return pointer;
        const NewHandler handler = currentNewHandler.load();
        if (handler == nullptr)
            return nullptr;
        handler();
    }
}

}

extern "C" {

void* APS5_VABI _Znwm_nid_postfix(std::size_t bytes) { return allocateOrThrow(bytes, 0); }
void* APS5_VABI _Znam_nid_postfix(std::size_t bytes) { return allocateOrThrow(bytes, 0); }
void* APS5_VABI _ZnwmRKSt9nothrow_t_nid_postfix(std::size_t bytes, const void*) { return allocateOrNull(bytes, 0); }
void* APS5_VABI _ZnamRKSt9nothrow_t_nid_postfix(std::size_t bytes, const void*) { return allocateOrNull(bytes, 0); }
void* APS5_VABI _ZnwmSt11align_val_t_nid_postfix(std::size_t bytes, std::size_t alignment) { return allocateOrThrow(bytes, alignment); }
void* APS5_VABI _ZnamSt11align_val_t_nid_postfix(std::size_t bytes, std::size_t alignment) { return allocateOrThrow(bytes, alignment); }
void* APS5_VABI _ZnwmSt11align_val_tRKSt9nothrow_t_nid_postfix(std::size_t bytes, std::size_t alignment, const void*) { return allocateOrNull(bytes, alignment); }
void* APS5_VABI _ZnamSt11align_val_tRKSt9nothrow_t_nid_postfix(std::size_t bytes, std::size_t alignment, const void*) { return allocateOrNull(bytes, alignment); }

void APS5_VABI _ZdlPv_nid_postfix(void* pointer) { ApplicationHeapFree_nid_no_patch(pointer); }
void APS5_VABI _ZdaPv_nid_postfix(void* pointer) { ApplicationHeapFree_nid_no_patch(pointer); }
void APS5_VABI _ZdlPvm_nid_postfix(void* pointer, std::size_t) { ApplicationHeapFree_nid_no_patch(pointer); }
void APS5_VABI _ZdaPvm_nid_postfix(void* pointer, std::size_t) { ApplicationHeapFree_nid_no_patch(pointer); }
void APS5_VABI _ZdlPvRKSt9nothrow_t_nid_postfix(void* pointer, const void*) { ApplicationHeapFree_nid_no_patch(pointer); }
void APS5_VABI _ZdaPvRKSt9nothrow_t_nid_postfix(void* pointer, const void*) { ApplicationHeapFree_nid_no_patch(pointer); }
void APS5_VABI _ZdlPvSt11align_val_t_nid_postfix(void* pointer, std::size_t) { ApplicationHeapFree_nid_no_patch(pointer); }
void APS5_VABI _ZdaPvSt11align_val_t_nid_postfix(void* pointer, std::size_t) { ApplicationHeapFree_nid_no_patch(pointer); }
void APS5_VABI _ZdlPvmSt11align_val_t_nid_postfix(void* pointer, std::size_t, std::size_t) { ApplicationHeapFree_nid_no_patch(pointer); }
void APS5_VABI _ZdaPvmSt11align_val_t_nid_postfix(void* pointer, std::size_t, std::size_t) { ApplicationHeapFree_nid_no_patch(pointer); }

NewHandler APS5_VABI _ZSt15get_new_handlerv_nid_postfix() { return currentNewHandler.load(); }
NewHandler APS5_VABI _ZSt15set_new_handlerPFvvE_nid_postfix(NewHandler handler) { return currentNewHandler.exchange(handler); }

}
