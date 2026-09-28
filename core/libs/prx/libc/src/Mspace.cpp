#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/MallocStatistics.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>

extern "C" int* APS5_VABI __error_nid_postfix();

namespace {
struct alignas(16) Block {
    std::size_t capacity;
    Block* next;
    void* pointer;
    std::size_t used;
};
// The arena header lives at the start of the caller's memory, as with dlmalloc's
// create_mspace_with_base, so the handle is an address inside that memory.
struct alignas(16) Arena {
    Arena* next;
    Block* first;
    std::uintptr_t end;
    std::size_t inUse;
    std::size_t peakInUse;
    bool ownsMemory;
};
constexpr unsigned MspaceThreadUnsafe = 1;
std::mutex arenaMutex;
Arena* arenas = nullptr;

void Error(int value) { *__error_nid_postfix() = value; }
bool PowerOfTwo(std::size_t value) { return value != 0 && (value & (value - 1)) == 0; }
Arena* Find(void* handle) {
    for (auto* arena = arenas; arena; arena = arena->next)
        if (arena == handle) return arena;
    Error(22);
    return nullptr;
}
Block* FindBlock(Arena* arena, const void* pointer) {
    if (arena && pointer)
        for (auto* block = arena->first; block; block = block->next)
            if (block->pointer == pointer) return block;
    Error(22);
    return nullptr;
}
void* Allocate(Arena* arena, std::size_t size, std::size_t alignment) {
    if (!arena) return nullptr;
    size = std::max<std::size_t>(size, 1);
    for (auto* block = arena->first; block; block = block->next) {
        if (block->pointer) continue;
        const auto start = reinterpret_cast<std::uintptr_t>(block + 1);
        if (start > std::numeric_limits<std::uintptr_t>::max() - (alignment - 1)) continue;
        const auto aligned = (start + alignment - 1) & ~(alignment - 1);
        const auto padding = aligned - start;
        if (padding > block->capacity || size > block->capacity - padding) continue;
        auto consumed = padding + size;
        // Splitting preserves header alignment and avoids small unusable fragments.
        if (consumed <= std::numeric_limits<std::size_t>::max() - 15) {
            const auto rounded = (consumed + 15) & ~std::size_t{15};
            if (rounded <= block->capacity && block->capacity - rounded >= sizeof(Block) + 16) {
                auto* tail = reinterpret_cast<Block*>(start + rounded);
                *tail = {block->capacity - rounded - sizeof(Block), block->next, nullptr, 0};
                block->capacity = rounded;
                block->next = tail;
            }
        }
        block->pointer = reinterpret_cast<void*>(aligned);
        block->used = size;
        arena->inUse += sizeof(Block) + block->capacity;
        arena->peakInUse = std::max(arena->peakInUse, arena->inUse);
        return block->pointer;
    }
    Error(12);
    return nullptr;
}
bool InsideAllocation(Arena* arena, std::uintptr_t start, std::size_t size) {
    for (auto* block = arena->first; block; block = block->next) {
        const auto pointer = reinterpret_cast<std::uintptr_t>(block->pointer);
        if (pointer && start >= pointer && size <= block->used && start - pointer <= block->used - size) return true;
    }
    return false;
}

void Release(Arena* arena, Block* released) {
    arena->inUse -= sizeof(Block) + released->capacity;
    released->pointer = nullptr;
    released->used = 0;
    for (auto* block = arena->first; block && block->next;) {
        if (!block->pointer && !block->next->pointer) {
            block->capacity += sizeof(Block) + block->next->capacity;
            block->next = block->next->next;
        } else block = block->next;
    }
}
void* Reallocate(Arena* arena, void* pointer, std::size_t size, std::size_t alignment) {
    if (!arena) return nullptr;
    if (!pointer) return Allocate(arena, size, alignment);
    auto* block = FindBlock(arena, pointer);
    if (!block) return nullptr;
    if (!size) { Release(arena, block); return nullptr; }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto padding = address - reinterpret_cast<std::uintptr_t>(block + 1);
    if ((address & (alignment - 1)) == 0 && size <= block->capacity - padding) {
        block->used = size;
        return pointer;
    }
    void* result = Allocate(arena, size, alignment);
    if (result) {
        std::memcpy(result, pointer, std::min(block->used, size));
        Release(arena, block);
    }
    return result;
}
}

extern "C" {
void* APS5_VABI sceLibcMspaceCreate_nid_postfix(const char* name, void* base,
                                              std::size_t size, unsigned flags) {
    (void)name;
    if (size < sizeof(Arena) + sizeof(Block) + 16 || (flags & ~MspaceThreadUnsafe) != 0) {
        Error(22);
        return nullptr;
    }
    // A null base asks the library to provide the arena's memory.
    const bool ownsMemory = base == nullptr;
    if (ownsMemory) {
        const auto rounded = (size + 15) & ~std::size_t{15};
        if (rounded < size || !(base = std::aligned_alloc(16, rounded))) {
            Error(12);
            return nullptr;
        }
    }
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    if ((start & 15) || size > std::numeric_limits<std::uintptr_t>::max() - start) {
        if (ownsMemory) std::free(base);
        Error(22);
        return nullptr;
    }
    std::lock_guard lock(arenaMutex);
    for (auto* arena = arenas; arena; arena = arena->next) {
        if (start < arena->end && reinterpret_cast<std::uintptr_t>(arena) < start + size && !InsideAllocation(arena, start, size)) {
            if (ownsMemory) std::free(base);
            Error(22);
            return nullptr;
        }
    }
    auto* arena = static_cast<Arena*>(base);
    auto* block = reinterpret_cast<Block*>(arena + 1);
    *block = {size - sizeof(Arena) - sizeof(Block), nullptr, nullptr, 0};
    *arena = {arenas, block, start + size, 0, 0, ownsMemory};
    arenas = arena;
    return arena;
}

int APS5_VABI sceLibcMspaceDestroy_nid_postfix(void* handle) {
    std::lock_guard lock(arenaMutex);
    for (auto** entry = &arenas; *entry; entry = &(*entry)->next) {
        if (*entry == handle) {
            auto* arena = *entry;
            *entry = arena->next;
            if (arena->ownsMemory) std::free(arena);
            return 0;
        }
    }
    Error(22);
    return -1;
}

void* APS5_VABI sceLibcMspaceMalloc_nid_postfix(void* handle, std::size_t size) {
    std::lock_guard lock(arenaMutex);
    return Allocate(Find(handle), size, 16);
}

int APS5_VABI sceLibcMspaceFree_nid_postfix(void* handle, void* pointer) {
    if (!pointer) return 0;
    std::lock_guard lock(arenaMutex);
    auto* arena = Find(handle);
    auto* block = FindBlock(arena, pointer);
    if (!block) return 22;
    Release(arena, block);
    return 0;
}

void* APS5_VABI sceLibcMspaceCalloc_nid_postfix(void* handle, std::size_t count, std::size_t size) {
    if (size && count > std::numeric_limits<std::size_t>::max() / size) {
        Error(12);
        return nullptr;
    }
    std::lock_guard lock(arenaMutex);
    void* result = Allocate(Find(handle), count * size, 16);
    if (result) std::memset(result, 0, count * size);
    return result;
}

void* APS5_VABI sceLibcMspaceRealloc_nid_postfix(void* handle, void* pointer, std::size_t size) {
    std::lock_guard lock(arenaMutex);
    return Reallocate(Find(handle), pointer, size, 16);
}

void* APS5_VABI sceLibcMspaceReallocalign_nid_postfix(void* handle, void* pointer, std::size_t alignment, std::size_t size) {
    if (!PowerOfTwo(alignment)) {
        Error(22);
        return nullptr;
    }
    std::lock_guard lock(arenaMutex);
    return Reallocate(Find(handle), pointer, size, std::max<std::size_t>(alignment, 16));
}

void* APS5_VABI sceLibcMspaceMemalign_nid_postfix(void* handle, std::size_t alignment, std::size_t size) {
    if (!PowerOfTwo(alignment)) {
        Error(22);
        return nullptr;
    }
    std::lock_guard lock(arenaMutex);
    return Allocate(Find(handle), size, std::max<std::size_t>(alignment, 16));
}

int APS5_VABI sceLibcMspacePosixMemalign_nid_postfix(void* handle, void** result,
                                                   std::size_t alignment, std::size_t size) {
    if (!result || alignment < sizeof(void*) || (alignment & (alignment - 1))) return 22;
    std::lock_guard lock(arenaMutex);
    auto* arena = Find(handle);
    if (!arena) return 22;
    void* pointer = Allocate(arena, size, std::max<std::size_t>(alignment, 16));
    if (!pointer) return 12;
    *result = pointer;
    return 0;
}

std::size_t APS5_VABI sceLibcMspaceMallocUsableSize_nid_postfix(const void* pointer) {
    if (!pointer) return 0;
    std::lock_guard lock(arenaMutex);
    for (auto* arena = arenas; arena; arena = arena->next)
        for (auto* block = arena->first; block; block = block->next)
            if (block->pointer == pointer) return block->used;
    Error(22);
    return 0;
}

int APS5_VABI sceLibcMspaceMallocStats_nid_postfix(void* handle, MallocStatistics::ManagedSize* statistics) {
    // The caller declares the structure's size; a shorter structure cannot hold the statistics.
    if (!statistics || statistics->size < sizeof(MallocStatistics::ManagedSize)) return 22;
    std::lock_guard lock(arenaMutex);
    auto* arena = Find(handle);
    if (!arena) return 22;
    const auto capacity = static_cast<std::size_t>(arena->end - reinterpret_cast<std::uintptr_t>(arena));
    statistics->maxSystemSize = capacity;
    statistics->currentSystemSize = capacity;
    statistics->maxInuseSize = arena->peakInUse;
    statistics->currentInuseSize = arena->inUse;
    return 0;
}

int APS5_VABI sceLibcMspaceMallocStatsFast_nid_postfix(void* handle, MallocStatistics::ManagedSize* statistics) {
    return sceLibcMspaceMallocStats_nid_postfix(handle, statistics);
}

int APS5_VABI sceLibcMspaceIsHeapEmpty_nid_postfix(void* handle) {
    std::lock_guard lock(arenaMutex);
    auto* arena = Find(handle);
    if (!arena) return 22;
    for (auto* block = arena->first; block; block = block->next)
        if (block->pointer) return 0;
    return 1;
}
}
