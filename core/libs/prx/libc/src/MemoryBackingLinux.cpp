#include "prx/libc/include/MemoryBackingPlatform.hpp"
#include <cerrno>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>
#include <unistd.h>

namespace GuestMemoryBacking::Platform {
namespace {

constexpr std::uintptr_t kArenaBase = 0x1000000000;
constexpr std::size_t kArenaSize = 0x3f000000000;
constexpr int kArenaFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;

void check(bool success, const char* operation) {
    if (!success) throw std::system_error(errno, std::generic_category(), operation);
}

void checkMapping(void* result, const char* operation, const void* address, std::size_t bytes) {
    if (result != MAP_FAILED) return;
    char message[128];
    std::snprintf(message, sizeof(message), "%s at %p (0x%zx bytes)", operation, address, bytes);
    check(false, message);
}

class AddressArena {
public:
    bool Contains(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(_mutex);
        _ensure();
        return address >= _base && address < _end && bytes <= _end - address;
    }

    std::uintptr_t Allocate(std::size_t bytes, std::size_t alignment) {
        std::lock_guard lock(_mutex);
        _ensure();
        for (auto it = _free.begin(); it != _free.end(); ++it) {
            const auto start = it->first;
            const auto finish = it->second;
            const auto aligned = (start + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
            if (aligned < start || aligned >= finish || finish - aligned < bytes) continue;
            _free.erase(it);
            if (aligned > start) _free.emplace(start, aligned);
            if (aligned + bytes < finish) _free.emplace(aligned + bytes, finish);
            return aligned;
        }
        char message[128];
        std::snprintf(message, sizeof(message), "guest address space exhausted (0x%zx bytes, alignment 0x%zx)", bytes, alignment);
        throw std::runtime_error(message);
    }

    void Claim(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(_mutex);
        _ensure();
        auto it = _free.upper_bound(address);
        if (it == _free.begin()) throw std::runtime_error("fixed guest mapping overlaps active guest memory");
        --it;
        const auto start = it->first;
        const auto finish = it->second;
        if (address + bytes > finish) throw std::runtime_error("fixed guest mapping overlaps active guest memory");
        _free.erase(it);
        if (start < address) _free.emplace(start, address);
        if (address + bytes < finish) _free.emplace(address + bytes, finish);
    }

    void Return(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(_mutex);
        checkMapping(mmap(reinterpret_cast<void*>(address), bytes, PROT_NONE, kArenaFlags | MAP_FIXED, -1, 0), "mmap guest address reservation", reinterpret_cast<void*>(address), bytes);
        std::uintptr_t start = address;
        std::uintptr_t finish = address + bytes;
        auto next = _free.lower_bound(start);
        if (next != _free.end() && next->first == finish) {
            finish = next->second;
            next = _free.erase(next);
        }
        if (next != _free.begin()) {
            const auto previous = std::prev(next);
            if (previous->second == start) {
                start = previous->first;
                _free.erase(previous);
            }
        }
        _free.emplace(start, finish);
    }

private:
    std::mutex _mutex;
    bool _initialized = false;
    std::uintptr_t _base = 0;
    std::uintptr_t _end = 0;
    std::map<std::uintptr_t, std::uintptr_t> _free;

    void _ensure() {
        if (_initialized) return;
        void* reservation = mmap(reinterpret_cast<void*>(kArenaBase), kArenaSize, PROT_NONE, kArenaFlags | MAP_FIXED_NOREPLACE, -1, 0);
        if (reservation == MAP_FAILED) reservation = mmap(nullptr, kArenaSize, PROT_NONE, kArenaFlags, -1, 0);
        checkMapping(reservation, "mmap guest address space", reinterpret_cast<void*>(kArenaBase), kArenaSize);
        _base = reinterpret_cast<std::uintptr_t>(reservation);
        _end = _base + kArenaSize;
        _free.emplace(_base, _end);
        _initialized = true;
    }
};

AddressArena& arena() {
    static AddressArena instance;
    return instance;
}

void releaseView(std::uint64_t address, std::size_t bytes) {
    if (arena().Contains(address, bytes)) {
        arena().Return(address, bytes);
        return;
    }
    check(munmap(reinterpret_cast<void*>(address), bytes) == 0, "munmap guest view");
}

}

Mapping Map(void* address, std::size_t bytes, std::size_t alignment, int protection) {
    if (bytes > static_cast<std::size_t>(std::numeric_limits<off_t>::max()) || bytes > std::numeric_limits<std::size_t>::max() - alignment) throw std::overflow_error("guest backing size overflow");
    int descriptor = memfd_create("AnyPS5 guest memory", MFD_CLOEXEC);
    check(descriptor >= 0, "memfd_create guest backing");
    void* alias = MAP_FAILED;
    std::uintptr_t target = 0;
    bool inArena = false;
    bool outsideReserved = false;
    try {
        check(ftruncate(descriptor, static_cast<off_t>(bytes)) == 0, "ftruncate guest backing");
        alias = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
        check(alias != MAP_FAILED, "mmap guest backing alias");
        const auto requested = reinterpret_cast<std::uintptr_t>(address);
        if (address == nullptr) {
            target = arena().Allocate(bytes, alignment);
            inArena = true;
        } else if (arena().Contains(requested, bytes)) {
            arena().Claim(requested, bytes);
            target = requested;
            inArena = true;
        } else {
            void* reservation = mmap(address, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            checkMapping(reservation, "mmap guest backing reservation", address, bytes);
            if (reservation != address) {
                check(munmap(reservation, bytes) == 0, "munmap mismatched guest reservation");
                throw std::runtime_error("fixed guest backing reservation address mismatch");
            }
            target = requested;
            outsideReserved = true;
        }
        void* guest = mmap(reinterpret_cast<void*>(target), bytes, protection, MAP_SHARED | MAP_FIXED, descriptor, 0);
        checkMapping(guest, "mmap guest backing view", reinterpret_cast<void*>(target), bytes);
        const auto closeResult = close(descriptor);
        descriptor = -1;
        check(closeResult == 0, "close guest backing descriptor");
        return {target, bytes, alias, 0};
    } catch (...) {
        if (inArena) arena().Return(target, bytes);
        else if (outsideReserved) check(munmap(reinterpret_cast<void*>(target), bytes) == 0, "munmap failed guest reservation");
        if (alias != MAP_FAILED) check(munmap(alias, bytes) == 0, "munmap failed guest alias");
        if (descriptor >= 0) check(close(descriptor) == 0, "close failed guest backing");
        throw;
    }
}

void Unmap(const Mapping& mapping) {
    releaseView(mapping.address, mapping.bytes);
    check(munmap(mapping.alias, mapping.bytes) == 0, "munmap guest alias");
}

void Deactivate(std::uint64_t address, std::size_t bytes) {
    check(mprotect(reinterpret_cast<void*>(address), bytes, PROT_NONE) == 0, "mprotect guest backing unmap");
}

void Release(std::uint64_t address, std::size_t bytes) {
    releaseView(address, bytes);
}

void ReleaseAlias(const Mapping& mapping) {
    check(munmap(mapping.alias, mapping.bytes) == 0, "munmap guest alias");
}

}
