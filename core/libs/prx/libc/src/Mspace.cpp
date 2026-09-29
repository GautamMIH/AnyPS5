#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/MallocStatistics.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

extern "C" int* APS5_VABI __error_nid_postfix();

namespace {

constexpr unsigned MspaceThreadUnsafe = 1;
constexpr std::uintptr_t Granule = 16;
// The start of the arena memory stays reserved, as dlmalloc's create_mspace_with_base keeps its
// header there, so the handle is an address inside that memory but never an allocation.
constexpr std::uintptr_t ArenaHeaderBytes = 64;

struct Chunk {
    std::uintptr_t end;
    bool used;
    std::size_t requested;
};

// Chunks are kept ordered by address for coalescing; free chunks are also indexed by size for
// best-fit search, so allocation and release stay logarithmic however fragmented the arena is.
struct Arena {
    std::uintptr_t base;
    std::uintptr_t end;
    std::map<std::uintptr_t, Chunk> chunks;
    std::set<std::pair<std::size_t, std::uintptr_t>> free;
    std::size_t inUse = 0;
    std::size_t peakInUse = 0;
    bool ownsMemory = false;
};

using ChunkIterator = std::map<std::uintptr_t, Chunk>::iterator;

std::mutex arenaMutex;
std::map<std::uintptr_t, std::unique_ptr<Arena>> arenas;

void Error(int value) { *__error_nid_postfix() = value; }
bool PowerOfTwo(std::size_t value) { return value != 0 && (value & (value - 1)) == 0; }

std::uintptr_t AlignUp(std::uintptr_t value, std::uintptr_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

Arena* Find(void* handle) {
    const auto found = arenas.find(reinterpret_cast<std::uintptr_t>(handle));
    if (found != arenas.end()) return found->second.get();
    Error(22);
    return nullptr;
}

void AddFree(Arena& arena, std::uintptr_t start, std::uintptr_t end) {
    arena.chunks[start] = {end, false, 0};
    arena.free.emplace(end - start, start);
}

void RemoveFree(Arena& arena, ChunkIterator chunk) {
    arena.free.erase({chunk->second.end - chunk->first, chunk->first});
}

void* Allocate(Arena* arena, std::size_t size, std::size_t alignment) {
    if (!arena) return nullptr;
    alignment = std::max<std::size_t>(alignment, Granule);
    const auto needed = AlignUp(std::max<std::size_t>(size, 1), Granule);
    if (needed < size || needed > std::numeric_limits<std::uintptr_t>::max() - alignment) {
        Error(12);
        return nullptr;
    }
    for (auto candidate = arena->free.lower_bound({needed, 0}); candidate != arena->free.end(); ++candidate) {
        const auto start = candidate->second;
        const auto chunk = arena->chunks.find(start);
        const auto end = chunk->second.end;
        const auto aligned = AlignUp(start, alignment);
        if (aligned < start || aligned > end || end - aligned < needed) continue;
        RemoveFree(*arena, chunk);
        arena->chunks.erase(chunk);
        if (aligned > start) AddFree(*arena, start, aligned);
        const auto finish = aligned + needed;
        if (finish < end) AddFree(*arena, finish, end);
        arena->chunks[aligned] = {finish, true, size};
        arena->inUse += needed;
        arena->peakInUse = std::max(arena->peakInUse, arena->inUse);
        return reinterpret_cast<void*>(aligned);
    }
    Error(12);
    return nullptr;
}

bool FindUsed(Arena* arena, const void* pointer, ChunkIterator& chunk) {
    if (arena && pointer) {
        chunk = arena->chunks.find(reinterpret_cast<std::uintptr_t>(pointer));
        if (chunk != arena->chunks.end() && chunk->second.used) return true;
    }
    Error(22);
    return false;
}

void Release(Arena& arena, ChunkIterator chunk) {
    auto start = chunk->first;
    auto end = chunk->second.end;
    arena.inUse -= end - start;
    if (chunk != arena.chunks.begin()) {
        const auto previous = std::prev(chunk);
        if (!previous->second.used) {
            start = previous->first;
            RemoveFree(arena, previous);
            arena.chunks.erase(previous);
        }
    }
    const auto next = std::next(chunk);
    if (next != arena.chunks.end() && !next->second.used) {
        end = next->second.end;
        RemoveFree(arena, next);
        arena.chunks.erase(next);
    }
    arena.chunks.erase(chunk);
    AddFree(arena, start, end);
}

// A nested mspace may be created inside an allocation of another one.
bool InsideAllocation(const Arena& arena, std::uintptr_t start, std::size_t size) {
    auto found = arena.chunks.upper_bound(start);
    if (found == arena.chunks.begin()) return false;
    --found;
    return found->second.used && size <= found->second.end - start && start - found->first <= found->second.end - found->first - size;
}

// Moves an allocation to a new chunk, keeping the contents that fit.
void* Move(Arena* arena, ChunkIterator chunk, std::size_t size, std::size_t alignment) {
    const auto start = chunk->first;
    const auto previousSize = chunk->second.requested;
    void* result = Allocate(arena, size, alignment);
    if (result) {
        std::memcpy(result, reinterpret_cast<void*>(start), std::min(previousSize, size));
        Release(*arena, arena->chunks.find(start));
    }
    return result;
}

void* Reallocate(Arena* arena, void* pointer, std::size_t size, std::size_t alignment) {
    if (!arena) return nullptr;
    if (!pointer) return Allocate(arena, size, alignment);
    ChunkIterator chunk;
    if (!FindUsed(arena, pointer, chunk)) return nullptr;
    if (!size) { Release(*arena, chunk); return nullptr; }
    const auto needed = AlignUp(size, Granule);
    if (needed < size) {
        Error(12);
        return nullptr;
    }
    const auto start = chunk->first;
    if ((start & (alignment - 1)) != 0) return Move(arena, chunk, size, alignment);
    const auto capacity = chunk->second.end - start;
    if (needed <= capacity) {
        chunk->second.requested = size;
        return pointer;
    }
    // Grow in place into the adjacent free chunk when it is large enough.
    const auto next = std::next(chunk);
    if (next != arena->chunks.end() && !next->second.used && next->first == chunk->second.end && next->second.end - start >= needed) {
        const auto nextEnd = next->second.end;
        RemoveFree(*arena, next);
        arena->chunks.erase(next);
        const auto finish = start + needed;
        if (finish < nextEnd) AddFree(*arena, finish, nextEnd);
        arena->inUse += needed - capacity;
        arena->peakInUse = std::max(arena->peakInUse, arena->inUse);
        arena->chunks[start] = {finish, true, size};
        return pointer;
    }
    return Move(arena, chunk, size, alignment);
}

int FillStats(void* handle, MallocStatistics::ManagedSize* statistics) {
    // The caller declares the structure's size; a shorter structure cannot hold the statistics.
    if (!statistics || statistics->size < sizeof(MallocStatistics::ManagedSize)) return 22;
    std::lock_guard lock(arenaMutex);
    auto* arena = Find(handle);
    if (!arena) return 22;
    const auto capacity = static_cast<std::size_t>(arena->end - arena->base);
    statistics->maxSystemSize = capacity;
    statistics->currentSystemSize = capacity;
    statistics->maxInuseSize = arena->peakInUse;
    statistics->currentInuseSize = arena->inUse;
    return 0;
}

}

extern "C" {
void* APS5_VABI sceLibcMspaceCreate_nid_postfix(const char* name, void* base,
                                              std::size_t size, unsigned flags) {
    (void)name;
    if (size < ArenaHeaderBytes + 2 * Granule || (flags & ~MspaceThreadUnsafe) != 0) {
        Error(22);
        return nullptr;
    }
    // A null base asks the library to provide the arena's memory.
    const bool ownsMemory = base == nullptr;
    if (ownsMemory) {
        const auto rounded = AlignUp(size, Granule);
        if (rounded < size || !(base = std::aligned_alloc(Granule, rounded))) {
            Error(12);
            return nullptr;
        }
    }
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    if ((start & (Granule - 1)) || size > std::numeric_limits<std::uintptr_t>::max() - start) {
        if (ownsMemory) std::free(base);
        Error(22);
        return nullptr;
    }
    std::lock_guard lock(arenaMutex);
    for (const auto& [otherBase, other] : arenas) {
        if (start < other->end && otherBase < start + size && !InsideAllocation(*other, start, size)) {
            if (ownsMemory) std::free(base);
            Error(22);
            return nullptr;
        }
    }
    auto arena = std::make_unique<Arena>();
    arena->base = start;
    arena->end = start + size;
    arena->ownsMemory = ownsMemory;
    AddFree(*arena, start + ArenaHeaderBytes, start + (size & ~(Granule - 1)));
    arenas.emplace(start, std::move(arena));
    return base;
}

int APS5_VABI sceLibcMspaceDestroy_nid_postfix(void* handle) {
    std::lock_guard lock(arenaMutex);
    const auto found = arenas.find(reinterpret_cast<std::uintptr_t>(handle));
    if (found == arenas.end()) {
        Error(22);
        return -1;
    }
    const bool ownsMemory = found->second->ownsMemory;
    arenas.erase(found);
    if (ownsMemory) std::free(handle);
    return 0;
}

void* APS5_VABI sceLibcMspaceMalloc_nid_postfix(void* handle, std::size_t size) {
    std::lock_guard lock(arenaMutex);
    return Allocate(Find(handle), size, Granule);
}

int APS5_VABI sceLibcMspaceFree_nid_postfix(void* handle, void* pointer) {
    if (!pointer) return 0;
    std::lock_guard lock(arenaMutex);
    auto* arena = Find(handle);
    ChunkIterator chunk;
    if (!FindUsed(arena, pointer, chunk)) return 22;
    Release(*arena, chunk);
    return 0;
}

void* APS5_VABI sceLibcMspaceCalloc_nid_postfix(void* handle, std::size_t count, std::size_t size) {
    if (size && count > std::numeric_limits<std::size_t>::max() / size) {
        Error(12);
        return nullptr;
    }
    std::lock_guard lock(arenaMutex);
    void* result = Allocate(Find(handle), count * size, Granule);
    if (result) std::memset(result, 0, count * size);
    return result;
}

void* APS5_VABI sceLibcMspaceRealloc_nid_postfix(void* handle, void* pointer, std::size_t size) {
    std::lock_guard lock(arenaMutex);
    return Reallocate(Find(handle), pointer, size, Granule);
}

// Same argument order as libc reallocalign(ptr, size, alignment).
void* APS5_VABI sceLibcMspaceReallocalign_nid_postfix(void* handle, void* pointer, std::size_t size, std::size_t alignment) {
    if (!PowerOfTwo(alignment)) {
        Error(22);
        return nullptr;
    }
    std::lock_guard lock(arenaMutex);
    return Reallocate(Find(handle), pointer, size, std::max<std::size_t>(alignment, Granule));
}

void* APS5_VABI sceLibcMspaceMemalign_nid_postfix(void* handle, std::size_t alignment, std::size_t size) {
    if (!PowerOfTwo(alignment)) {
        Error(22);
        return nullptr;
    }
    std::lock_guard lock(arenaMutex);
    return Allocate(Find(handle), size, alignment);
}

int APS5_VABI sceLibcMspacePosixMemalign_nid_postfix(void* handle, void** result,
                                                   std::size_t alignment, std::size_t size) {
    if (!result || alignment < sizeof(void*) || (alignment & (alignment - 1))) return 22;
    std::lock_guard lock(arenaMutex);
    auto* arena = Find(handle);
    if (!arena) return 22;
    void* pointer = Allocate(arena, size, alignment);
    if (!pointer) return 12;
    *result = pointer;
    return 0;
}

std::size_t APS5_VABI sceLibcMspaceMallocUsableSize_nid_postfix(const void* pointer) {
    if (!pointer) return 0;
    std::lock_guard lock(arenaMutex);
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    // Nested arenas share addresses with their parent's allocation; the innermost owns the chunk.
    for (auto arena = arenas.upper_bound(address); arena != arenas.begin();) {
        --arena;
        if (address >= arena->second->end) continue;
        const auto found = arena->second->chunks.find(address);
        if (found != arena->second->chunks.end() && found->second.used) return found->second.end - found->first;
    }
    Error(22);
    return 0;
}

int APS5_VABI sceLibcMspaceMallocStats_nid_postfix(void* handle, MallocStatistics::ManagedSize* statistics) {
    return FillStats(handle, statistics);
}

int APS5_VABI sceLibcMspaceMallocStatsFast_nid_postfix(void* handle, MallocStatistics::ManagedSize* statistics) {
    return FillStats(handle, statistics);
}

int APS5_VABI sceLibcMspaceIsHeapEmpty_nid_postfix(void* handle) {
    std::lock_guard lock(arenaMutex);
    auto* arena = Find(handle);
    if (!arena) return 22;
    return arena->inUse == 0 ? 1 : 0;
}
}
