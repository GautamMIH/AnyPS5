#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "prx/libc/include/General.hpp"
#include "prx/libc/include/MallocStatistics.hpp"

namespace {

constexpr std::size_t kMinimumAlignment = 16;
constexpr std::size_t kOwnedMemoryAlignment = 0x4000;
constexpr int kErrorInvalid = 22;
constexpr int kErrorNoMemory = 12;

std::size_t alignUp(const std::size_t value, const std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool isPowerOfTwo(const std::size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

struct Mspace {
    std::mutex Mutex;
    std::string Name;
    std::uintptr_t Base = 0;
    std::size_t Capacity = 0;
    bool OwnsMemory = false;
    std::map<std::uintptr_t, std::size_t> FreeBlocks;
    std::map<std::uintptr_t, std::size_t> UsedBlocks;
    std::size_t InUse = 0;
    std::size_t MaxInUse = 0;

    void* Allocate(std::size_t bytes, std::size_t alignment) {
        const std::size_t size = alignUp(std::max<std::size_t>(bytes, 1), kMinimumAlignment);
        alignment = std::max(alignment, kMinimumAlignment);
        for (auto it = FreeBlocks.begin(); it != FreeBlocks.end(); ++it) {
            const std::uintptr_t start = it->first;
            const std::uintptr_t end = start + it->second;
            const std::uintptr_t aligned = alignUp(start, alignment);
            if (aligned < start || aligned > end || end - aligned < size)
                continue;
            FreeBlocks.erase(it);
            if (aligned > start)
                FreeBlocks.emplace(start, aligned - start);
            if (end > aligned + size)
                FreeBlocks.emplace(aligned + size, end - aligned - size);
            UsedBlocks.emplace(aligned, size);
            InUse += size;
            MaxInUse = std::max(MaxInUse, InUse);
            return reinterpret_cast<void*>(aligned);
        }
        return nullptr;
    }

    bool Release(void* pointer) {
        const auto used = UsedBlocks.find(reinterpret_cast<std::uintptr_t>(pointer));
        if (used == UsedBlocks.end())
            return false;
        std::uintptr_t start = used->first;
        std::size_t size = used->second;
        InUse -= size;
        UsedBlocks.erase(used);
        auto next = FreeBlocks.lower_bound(start);
        if (next != FreeBlocks.end() && next->first == start + size) {
            size += next->second;
            next = FreeBlocks.erase(next);
        }
        if (next != FreeBlocks.begin()) {
            const auto previous = std::prev(next);
            if (previous->first + previous->second == start) {
                start = previous->first;
                size += previous->second;
                FreeBlocks.erase(previous);
            }
        }
        FreeBlocks.emplace(start, size);
        return true;
    }

    std::size_t UsableSize(void* pointer) const {
        const auto used = UsedBlocks.find(reinterpret_cast<std::uintptr_t>(pointer));
        return used == UsedBlocks.end() ? 0 : used->second;
    }

    void* Reallocate(void* pointer, std::size_t bytes, std::size_t alignment) {
        if (pointer == nullptr)
            return Allocate(bytes, alignment);
        const std::size_t previous = UsableSize(pointer);
        if (previous == 0)
            return nullptr;
        void* replacement = Allocate(bytes, alignment);
        if (replacement == nullptr)
            return nullptr;
        std::memcpy(replacement, pointer, std::min(previous, bytes));
        Release(pointer);
        return replacement;
    }

    void Fill(MallocStatistics::ManagedSize* statistics) const {
        statistics->maxSystemSize = Capacity;
        statistics->currentSystemSize = Capacity;
        statistics->maxInuseSize = MaxInUse;
        statistics->currentInuseSize = InUse;
    }
};

std::mutex& registryMutex() {
    static std::mutex instance;
    return instance;
}

std::vector<Mspace*>& registry() {
    static std::vector<Mspace*> instance;
    return instance;
}

Mspace* owner(void* pointer) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const std::lock_guard lock(registryMutex());
    for (auto* space : registry())
        if (address >= space->Base && address - space->Base < space->Capacity)
            return space;
    return nullptr;
}

Mspace* validate(void* handle) {
    if (handle == nullptr)
        return nullptr;
    const std::lock_guard lock(registryMutex());
    const auto& spaces = registry();
    return std::find(spaces.begin(), spaces.end(), handle) != spaces.end() ? static_cast<Mspace*>(handle) : nullptr;
}

}

extern "C" {

void* APS5_VABI sceLibcMspaceCreate_nid_postfix(const char* name, void* base, std::size_t capacity, unsigned int) {
    if (capacity == 0)
        return nullptr;
    auto* space = new (std::nothrow) Mspace();
    if (space == nullptr)
        return nullptr;
    if (base == nullptr) {
        base = std::aligned_alloc(kOwnedMemoryAlignment, alignUp(capacity, kOwnedMemoryAlignment));
        if (base == nullptr) {
            delete space;
            return nullptr;
        }
        space->OwnsMemory = true;
    }
    space->Name = name != nullptr ? name : "";
    space->Base = reinterpret_cast<std::uintptr_t>(base);
    space->Capacity = capacity;
    const std::uintptr_t first = alignUp(space->Base, kMinimumAlignment);
    if (first - space->Base < capacity)
        space->FreeBlocks.emplace(first, capacity - (first - space->Base));
    const std::lock_guard lock(registryMutex());
    registry().push_back(space);
    return space;
}

int APS5_VABI sceLibcMspaceDestroy_nid_postfix(void* handle) {
    Mspace* space = validate(handle);
    if (space == nullptr)
        return kErrorInvalid;
    {
        const std::lock_guard lock(registryMutex());
        std::erase(registry(), space);
    }
    if (space->OwnsMemory)
        std::free(reinterpret_cast<void*>(space->Base));
    delete space;
    return 0;
}

void* APS5_VABI sceLibcMspaceMalloc_nid_postfix(void* handle, std::size_t bytes) {
    Mspace* space = validate(handle);
    if (space == nullptr)
        return nullptr;
    const std::lock_guard lock(space->Mutex);
    return space->Allocate(bytes, kMinimumAlignment);
}

void* APS5_VABI sceLibcMspaceCalloc_nid_postfix(void* handle, std::size_t count, std::size_t bytes) {
    if (bytes != 0 && count > SIZE_MAX / bytes)
        return nullptr;
    void* pointer = sceLibcMspaceMalloc_nid_postfix(handle, count * bytes);
    if (pointer != nullptr)
        std::memset(pointer, 0, count * bytes);
    return pointer;
}

void* APS5_VABI sceLibcMspaceMemalign_nid_postfix(void* handle, std::size_t alignment, std::size_t bytes) {
    Mspace* space = validate(handle);
    if (space == nullptr || !isPowerOfTwo(alignment))
        return nullptr;
    const std::lock_guard lock(space->Mutex);
    return space->Allocate(bytes, alignment);
}

int APS5_VABI sceLibcMspacePosixMemalign_nid_postfix(void* handle, void** pointer, std::size_t alignment, std::size_t bytes) {
    if (pointer == nullptr || !isPowerOfTwo(alignment) || alignment < sizeof(void*))
        return kErrorInvalid;
    void* result = sceLibcMspaceMemalign_nid_postfix(handle, alignment, bytes);
    if (result == nullptr)
        return kErrorNoMemory;
    *pointer = result;
    return 0;
}

int APS5_VABI sceLibcMspaceFree_nid_postfix(void* handle, void* pointer) {
    if (pointer == nullptr)
        return 0;
    Mspace* space = validate(handle);
    if (space == nullptr)
        return kErrorInvalid;
    const std::lock_guard lock(space->Mutex);
    return space->Release(pointer) ? 0 : kErrorInvalid;
}

void* APS5_VABI sceLibcMspaceRealloc_nid_postfix(void* handle, void* pointer, std::size_t bytes) {
    Mspace* space = validate(handle);
    if (space == nullptr)
        return nullptr;
    const std::lock_guard lock(space->Mutex);
    if (bytes == 0 && pointer != nullptr) {
        space->Release(pointer);
        return nullptr;
    }
    return space->Reallocate(pointer, bytes, kMinimumAlignment);
}

void* APS5_VABI sceLibcMspaceReallocalign_nid_postfix(void* handle, void* pointer, std::size_t alignment, std::size_t bytes) {
    Mspace* space = validate(handle);
    if (space == nullptr || !isPowerOfTwo(alignment))
        return nullptr;
    const std::lock_guard lock(space->Mutex);
    return space->Reallocate(pointer, bytes, alignment);
}

std::size_t APS5_VABI sceLibcMspaceMallocUsableSize_nid_postfix(void* pointer) {
    Mspace* space = owner(pointer);
    if (space == nullptr)
        return 0;
    const std::lock_guard lock(space->Mutex);
    return space->UsableSize(pointer);
}

int APS5_VABI sceLibcMspaceMallocStats_nid_postfix(void* handle, MallocStatistics::ManagedSize* statistics) {
    Mspace* space = validate(handle);
    if (space == nullptr || statistics == nullptr)
        return kErrorInvalid;
    const std::lock_guard lock(space->Mutex);
    space->Fill(statistics);
    return 0;
}

int APS5_VABI sceLibcMspaceMallocStatsFast_nid_postfix(void* handle, MallocStatistics::ManagedSize* statistics) {
    return sceLibcMspaceMallocStats_nid_postfix(handle, statistics);
}

int APS5_VABI sceLibcMspaceIsHeapEmpty_nid_postfix(void* handle) {
    Mspace* space = validate(handle);
    if (space == nullptr)
        return kErrorInvalid;
    const std::lock_guard lock(space->Mutex);
    return space->UsedBlocks.empty() ? 1 : 0;
}

}
