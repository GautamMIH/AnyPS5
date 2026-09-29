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
#include <array>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
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

namespace {

// Asynchronous userfaultfd write-protection (Linux 6.7+): writes to protected pages are resolved by
// the kernel without a fault handler, and PAGEMAP_SCAN reports and re-protects the written ones.
// User-mode-only registration needs no privileges; kernel writes (read(2) into a page) still mark
// pages written. Views are registered as they are mapped, and unpopulated pages start protected.
struct WriteWatch {
    int userfault = -1;
    int pagemap = -1;
    bool available = false;
};

WriteWatch& writeWatch() {
    static WriteWatch* watch = [] {
        auto* result = new WriteWatch;
        if (std::getenv("ANYPS5_NO_WRITE_WATCH") != nullptr) return result;
        result->userfault = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
        if (result->userfault < 0) return result;
        uffdio_api api{};
        api.api = UFFD_API;
        api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED | UFFD_FEATURE_WP_HUGETLBFS_SHMEM;
        result->pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        result->available = ioctl(result->userfault, UFFDIO_API, &api) == 0 && result->pagemap >= 0;
        if (!result->available) std::fprintf(stderr, "[memory] userfaultfd write-protect is unavailable; guest writes are not watched\n");
        return result;
    }();
    return *watch;
}

void watchView(std::uint64_t address, std::size_t bytes) {
    auto& watch = writeWatch();
    if (!watch.available) return;
    uffdio_register registration{};
    registration.range.start = address;
    registration.range.len = bytes;
    registration.mode = UFFDIO_REGISTER_MODE_WP;
    if (ioctl(watch.userfault, UFFDIO_REGISTER, &registration) != 0) {
        std::fprintf(stderr, "[memory] cannot watch guest writes at 0x%llx+0x%zx (errno %d); the write watch is off\n", static_cast<unsigned long long>(address), bytes, errno);
        watch.available = false;
    }
}

}

bool WriteWatchAvailable() {
    return writeWatch().available;
}

bool CollectWrites(std::uint64_t address, std::size_t bytes, WrittenRangeVisitor visit, void* context) {
    auto& watch = writeWatch();
    if (!watch.available || visit == nullptr) return false;
    std::array<page_region, 64> regions{};
    auto cursor = address;
    const auto end = address + bytes;
    while (cursor < end) {
        pm_scan_arg scan{};
        scan.size = sizeof(scan);
        scan.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
        scan.start = cursor;
        scan.end = end;
        scan.vec = reinterpret_cast<std::uint64_t>(regions.data());
        scan.vec_len = regions.size();
        scan.category_mask = PAGE_IS_WRITTEN;
        scan.return_mask = PAGE_IS_WRITTEN;
        const long count = ioctl(watch.pagemap, PAGEMAP_SCAN, &scan);
        if (count < 0) return false;
        for (long index = 0; index < count; ++index) visit(context, regions[static_cast<std::size_t>(index)].start, regions[static_cast<std::size_t>(index)].end);
        // walk_end is where the scan stopped (the end, or where the region buffer filled up).
        if (scan.walk_end <= cursor) return false;
        cursor = scan.walk_end;
    }
    return true;
}

void MapView(std::uint64_t address, std::size_t bytes, const Segment& segment, std::uint64_t offset, int protection) {
    if (offset > segment.bytes || bytes > segment.bytes - offset) throw std::out_of_range("guest view exceeds its segment");
    checkMapping(mmap(reinterpret_cast<void*>(address), bytes, protection, MAP_SHARED | MAP_FIXED, static_cast<int>(segment.handle), static_cast<off_t>(offset)), "mmap guest view", address, bytes);
    watchView(address, bytes);
}

void UnmapView(std::uint64_t address, std::size_t bytes) {
    reserve(address, bytes);
}

void Protect(std::uint64_t address, std::size_t bytes, int protection) {
    check(mprotect(reinterpret_cast<void*>(address), bytes, protection) == 0, "mprotect guest range");
}

}
