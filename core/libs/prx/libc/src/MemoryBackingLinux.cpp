#include <cstdlib>
#include "prx/libc/include/MemoryBackingPlatform.hpp"
#include <cerrno>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

namespace GuestMemoryBacking::Platform {
namespace {

constexpr std::uintptr_t kArenaBase = 0x1000000000;
constexpr std::size_t kArenaSize = 0x3f000000000;
constexpr int kReservedFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;

void check(bool success, const char* operation) {
    if (!success) throw std::system_error(errno, std::generic_category(), operation);
}

void checkMapping(void* result, const char* operation, std::uint64_t address, std::size_t bytes) {
    if (result != MAP_FAILED) return;
    char message[128];
    std::snprintf(message, sizeof(message), "%s at 0x%llx (0x%zx bytes)", operation, static_cast<unsigned long long>(address), bytes);
    check(false, message);
}

// Returns a range to the reserved, inaccessible state.
void reserve(std::uint64_t address, std::size_t bytes) {
    checkMapping(mmap(reinterpret_cast<void*>(address), bytes, PROT_NONE, kReservedFlags | MAP_FIXED, -1, 0), "mmap guest reservation", address, bytes);
}

// The guest address space: a reserved host region with a free list, plus fixed claims outside it.
class AddressSpace {
public:
    std::uint64_t Claim(std::uint64_t address, std::size_t bytes, std::size_t alignment, bool fixed) {
        std::lock_guard lock(_mutex);
        _ensure();
        if (fixed) {
            if (address >= _base && address < _end) {
                if (bytes > _end - address || !_takeFree(address, bytes)) return 0;
                return address;
            }
            // Outside the arena: claim host address space that nothing else uses.
            void* reservation = mmap(reinterpret_cast<void*>(address), bytes, PROT_NONE, kReservedFlags | MAP_FIXED_NOREPLACE, -1, 0);
            if (reservation == MAP_FAILED) return 0;
            if (reinterpret_cast<std::uint64_t>(reservation) != address) {
                munmap(reservation, bytes);
                return 0;
            }
            _outside.emplace(address, address + bytes);
            return address;
        }
        const auto start = address < _base ? _base : address;
        for (auto it = _free.upper_bound(start) == _free.begin() ? _free.begin() : std::prev(_free.upper_bound(start)); it != _free.end(); ++it) {
            const auto first = it->first < start ? start : it->first;
            const auto aligned = (first + alignment - 1) & ~(static_cast<std::uint64_t>(alignment) - 1);
            if (aligned < first || aligned >= it->second || it->second - aligned < bytes) continue;
            _takeFree(aligned, bytes);
            return aligned;
        }
        return 0;
    }

    bool Free(std::uint64_t address, std::size_t bytes) {
        std::lock_guard lock(_mutex);
        _ensure();
        if (address >= _base && address < _end) {
            auto it = _free.upper_bound(address);
            if (it == _free.begin()) return false;
            --it;
            return address + bytes <= it->second;
        }
        return false;
    }

    void Release(std::uint64_t address, std::size_t bytes) {
        std::lock_guard lock(_mutex);
        _ensure();
        if (address < _base || address >= _end) {
            // Outside claims are released whole or split like any mapping.
            check(munmap(reinterpret_cast<void*>(address), bytes) == 0, "munmap outside guest range");
            _removeOutside(address, address + bytes);
            return;
        }
        reserve(address, bytes);
        std::uint64_t start = address;
        std::uint64_t finish = address + bytes;
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
    std::uint64_t _base = 0;
    std::uint64_t _end = 0;
    std::map<std::uint64_t, std::uint64_t> _free;
    std::map<std::uint64_t, std::uint64_t> _outside;

    bool _takeFree(std::uint64_t address, std::size_t bytes) {
        auto it = _free.upper_bound(address);
        if (it == _free.begin()) return false;
        --it;
        const auto start = it->first;
        const auto finish = it->second;
        if (address + bytes > finish) return false;
        _free.erase(it);
        if (start < address) _free.emplace(start, address);
        if (address + bytes < finish) _free.emplace(address + bytes, finish);
        return true;
    }

    void _removeOutside(std::uint64_t first, std::uint64_t last) {
        std::map<std::uint64_t, std::uint64_t> kept;
        for (const auto& [begin, end] : _outside) {
            if (end <= first || begin >= last) {
                kept.emplace(begin, end);
                continue;
            }
            if (begin < first) kept.emplace(begin, first);
            if (end > last) kept.emplace(last, end);
        }
        _outside.swap(kept);
    }

    void _ensure() {
        if (_initialized) return;
        void* reservation = mmap(reinterpret_cast<void*>(kArenaBase), kArenaSize, PROT_NONE, kReservedFlags | MAP_FIXED_NOREPLACE, -1, 0);
        if (reservation == MAP_FAILED) reservation = mmap(nullptr, kArenaSize, PROT_NONE, kReservedFlags, -1, 0);
        checkMapping(reservation, "mmap guest address space", kArenaBase, kArenaSize);
        _base = reinterpret_cast<std::uint64_t>(reservation);
        _end = _base + kArenaSize;
        _free.emplace(_base, _end);
        // Segments keep their descriptors open for later views.
        rlimit files{};
        if (getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur < files.rlim_max) {
            files.rlim_cur = files.rlim_max;
            setrlimit(RLIMIT_NOFILE, &files);
        }
        _initialized = true;
    }
};

AddressSpace& space() {
    static auto* instance = new AddressSpace;
    return *instance;
}

}

// Guest memory reserves the console's whole address space in shared segments. A core dump reads every
// page of shared mappings, allocating the ones never touched, so dumping a crashed title could need
// more memory than the host has; dumps leave shared memory out (ANYPS5_FULL_COREDUMP=1 keeps it).
void excludeSharedMemoryFromCoreDumps() {
    static const bool done = [] {
        if (std::getenv("ANYPS5_FULL_COREDUMP") != nullptr) return true;
        std::FILE* filter = std::fopen("/proc/self/coredump_filter", "r+");
        if (filter == nullptr) return true;
        unsigned long bits = 0;
        if (std::fscanf(filter, "%lx", &bits) == 1) {
            constexpr unsigned long sharedAnonymous = 1ul << 1u;
            constexpr unsigned long sharedHuge = 1ul << 6u;
            std::rewind(filter);
            std::fprintf(filter, "0x%lx", bits & ~(sharedAnonymous | sharedHuge));
        }
        std::fclose(filter);
        return true;
    }();
    static_cast<void>(done);
}

Segment CreateSegment(std::size_t bytes) {
    if (bytes == 0 || bytes > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) throw std::overflow_error("guest segment size");
    excludeSharedMemoryFromCoreDumps();
    const int descriptor = memfd_create("AnyPS5 guest memory", MFD_CLOEXEC);
    check(descriptor >= 0, "memfd_create guest segment");
    if (ftruncate(descriptor, static_cast<off_t>(bytes)) != 0) {
        const auto error = errno;
        close(descriptor);
        errno = error;
        check(false, "ftruncate guest segment");
    }
    void* alias = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, descriptor, 0);
    if (alias == MAP_FAILED) {
        const auto error = errno;
        close(descriptor);
        errno = error;
        check(false, "mmap guest segment alias");
    }
    return {static_cast<std::uintptr_t>(descriptor), alias, bytes};
}

void DestroySegment(const Segment& segment) {
    check(munmap(segment.alias, segment.bytes) == 0, "munmap guest segment alias");
    check(close(static_cast<int>(segment.handle)) == 0, "close guest segment");
}

std::uint64_t ClaimRange(std::uint64_t address, std::size_t bytes, std::size_t alignment, bool fixed) {
    return space().Claim(address, bytes, alignment, fixed);
}

bool RangeFree(std::uint64_t address, std::size_t bytes) {
    return space().Free(address, bytes);
}

void ReleaseRange(std::uint64_t address, std::size_t bytes) {
    space().Release(address, bytes);
}

void MapView(std::uint64_t address, std::size_t bytes, const Segment& segment, std::uint64_t offset, int protection) {
    if (offset > segment.bytes || bytes > segment.bytes - offset) throw std::out_of_range("guest view exceeds its segment");
    checkMapping(mmap(reinterpret_cast<void*>(address), bytes, protection, MAP_SHARED | MAP_FIXED, static_cast<int>(segment.handle), static_cast<off_t>(offset)), "mmap guest view", address, bytes);
}

void UnmapView(std::uint64_t address, std::size_t bytes) {
    reserve(address, bytes);
}

void Protect(std::uint64_t address, std::size_t bytes, int protection) {
    check(mprotect(reinterpret_cast<void*>(address), bytes, protection) == 0, "mprotect guest range");
}

}
