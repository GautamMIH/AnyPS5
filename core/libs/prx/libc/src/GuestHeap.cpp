#include "prx/libc/include/GuestHeap.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestHeapStorage.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace GuestHeap {
namespace {

// Clang's PS4/PS5 targets set the default new and malloc alignment to 32 bytes, so guest code may
// store to a fresh malloc or operator new result with 32-byte-aligned AVX instructions (upstream
// a43f1411c).
constexpr std::size_t MinimumAlignment = 32;

void* allocate(GuestAllocations::Mutation& mutation, std::size_t alignment, std::size_t bytes, bool gpu) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) throw std::invalid_argument("invalid guest heap alignment");
    alignment = std::max(alignment, MinimumAlignment);
    void* pointer = GuestHeapStorage::Allocate(alignment, bytes);
    try {
        mutation.Add(pointer, bytes, true, true, gpu);
    } catch (...) {
        GuestHeapStorage::Free(pointer);
        throw;
    }
    return pointer;
}

void free(GuestAllocations::Mutation& mutation, void* pointer) {
    mutation.Remove(pointer);
    GuestHeapStorage::Free(pointer);
}

}

void* GuestHeapAllocate_nid_postfix(std::size_t bytes) {
    GuestAllocations::Mutation mutation;
    return allocate(mutation, MinimumAlignment, bytes, false);
}

void GuestHeapFree_nid_postfix(void* pointer) {
    if (pointer == nullptr) return;
    GuestAllocations::Mutation mutation;
    const auto range = mutation.Find(pointer);
    mutation.RequireUnpinned(pointer, range.bytes);
    free(mutation, pointer);
}

void* GuestHeapReallocate_nid_postfix(void* pointer, std::size_t bytes) {
    if (pointer == nullptr) return GuestHeapAllocate_nid_postfix(bytes);
    GuestAllocations::Mutation mutation;
    const auto range = mutation.Find(pointer);
    mutation.RequireUnpinned(pointer, range.bytes);
    if (bytes == 0) {
        free(mutation, pointer);
        return nullptr;
    }
    void* result = allocate(mutation, MinimumAlignment, bytes, range.gpu);
    std::memcpy(result, pointer, std::min(bytes, range.bytes));
    free(mutation, pointer);
    return result;
}

void* GuestHeapRealign_nid_postfix(void* pointer, std::size_t bytes, std::size_t alignment) {
    if (pointer == nullptr) return GuestHeapAlign_nid_postfix(alignment, bytes);
    GuestAllocations::Mutation mutation;
    const auto range = mutation.Find(pointer);
    mutation.RequireUnpinned(pointer, range.bytes);
    if (bytes == 0) {
        free(mutation, pointer);
        return nullptr;
    }
    void* result = allocate(mutation, alignment, bytes, range.gpu);
    std::memcpy(result, pointer, std::min(bytes, range.bytes));
    free(mutation, pointer);
    return result;
}

std::size_t GuestHeapUsableSize_nid_postfix(const void* pointer) {
    if (pointer == nullptr) return 0;
    GuestAllocations::Mutation mutation;
    return mutation.Find(pointer).bytes;
}

void* GuestHeapAlign_nid_postfix(std::size_t alignment, std::size_t bytes) {
    GuestAllocations::Mutation mutation;
    return allocate(mutation, alignment, bytes, false);
}

void* GuestHeapAlignForGpu_nid_postfix(std::size_t alignment, std::size_t bytes) {
    GuestAllocations::Mutation mutation;
    return allocate(mutation, alignment, bytes, true);
}

}
