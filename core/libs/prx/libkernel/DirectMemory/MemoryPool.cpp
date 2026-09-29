#define _GLIBCXX_HAS_GTHREADS 0
#include "MemoryPool.hpp"
#include "DirectMemory.hpp"
#include <map>
#include <mutex>

static constexpr size_t NUM_PAGES = DIRECT_MEMORY_SIZE / PS5_PAGE_SIZE;

struct PhysicalMemoryPool {
    static PhysicalMemoryPool& Instance() {
        static PhysicalMemoryPool inst;
        return inst;
    }

    int Alloc(int64_t searchStart, int64_t searchEnd, size_t len, size_t alignment, int memoryType, int64_t* physOut) {
        std::lock_guard<std::mutex> lock(_mutex);
        size_t align = (alignment == 0) ? PS5_PAGE_SIZE : alignment;
        uint64_t start = static_cast<uint64_t>(searchStart);
        uint64_t end = static_cast<uint64_t>(searchEnd);
        uint64_t cur = (start + align - 1) & ~(align - 1);
        while (cur + len <= end && cur + len <= DIRECT_MEMORY_SIZE) {
            if (_isFree(cur, len)) {
                _mark(cur, len, true);
                _ranges[cur] = {cur, cur + len, memoryType};
                *physOut = static_cast<int64_t>(cur);
                return 0;
            }
            cur += align;
        }
        return SCE_KERNEL_ERROR_EAGAIN;
    }

    void Free(uint64_t start, size_t len) {
        std::lock_guard<std::mutex> lock(_mutex);
        _mark(start, len, false);
        auto it = _ranges.upper_bound(start);
        if (it == _ranges.begin()) return;
        --it;
        if (start + len > it->second.end) return;
        const DirectMemoryBlock block = it->second;
        _ranges.erase(it);
        if (block.start < start) _ranges[block.start] = {block.start, start, block.memoryType};
        if (start + len < block.end) _ranges[start + len] = {start + len, block.end, block.memoryType};
    }

    bool Query(uint64_t offset, DirectMemoryBlock* block) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _ranges.upper_bound(offset);
        if (it == _ranges.begin()) return false;
        --it;
        if (offset >= it->second.end) return false;
        *block = it->second;
        return true;
    }

    size_t FreeRun(uint64_t offset, uint64_t limit) {
        std::lock_guard<std::mutex> lock(_mutex);
        uint64_t cur = offset & ~static_cast<uint64_t>(PS5_PAGE_SIZE - 1);
        size_t run = 0;
        while (cur + PS5_PAGE_SIZE <= limit && cur + PS5_PAGE_SIZE <= DIRECT_MEMORY_SIZE
               && _isFree(cur, PS5_PAGE_SIZE)) {
            cur += PS5_PAGE_SIZE;
            run += PS5_PAGE_SIZE;
        }
        return run;
    }

private:
    bool _isFree(uint64_t offset, size_t len) const {
        size_t first = offset / PS5_PAGE_SIZE;
        size_t count = len / PS5_PAGE_SIZE;
        for (size_t i = 0; i < count; ++i)
            if (_used[first + i]) return false;
        return true;
    }

    void _mark(uint64_t offset, size_t len, bool used) {
        size_t first = offset / PS5_PAGE_SIZE;
        size_t count = len / PS5_PAGE_SIZE;
        for (size_t i = 0; i < count; ++i)
            _used[first + i] = used;
    }

    std::mutex _mutex;
    bool _used[NUM_PAGES] = {};
    std::map<uint64_t, DirectMemoryBlock> _ranges;
};

int DirectMemoryAlloc(int64_t searchStart, int64_t searchEnd, size_t len, size_t alignment, int memoryType, int64_t* physOut) {
    return PhysicalMemoryPool::Instance().Alloc(searchStart, searchEnd, len, alignment, memoryType, physOut);
}

void DirectMemoryFree(int64_t start, size_t len) {
    PhysicalMemoryPool::Instance().Free(static_cast<uint64_t>(start), len);
}

bool DirectMemoryQueryBlock(uint64_t offset, DirectMemoryBlock* block) {
    return PhysicalMemoryPool::Instance().Query(offset, block);
}

size_t DirectMemoryFreeRun(uint64_t offset, uint64_t limit) {
    return PhysicalMemoryPool::Instance().FreeRun(offset, limit);
}
