#include "DirectMemory.hpp"
#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/File/include/SharedMemory.hpp"
#include <limits>
#include <cerrno>
#include <new>
#include <stdexcept>
#include <system_error>
#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

extern "C" int* APS5_VABI __error_nid_postfix();
extern "C" int APS5_VABI sceKernelMlock_nid_postfix(void* address, std::uint64_t length);
extern "C" int APS5_VABI sceKernelMunlock_nid_postfix(void* address, std::uint64_t length);
extern "C" int APS5_VABI sceKernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info, uint64_t info_size);

namespace {
// FreeBSD/PS5 ABI values, independent of the host's errno and mmap constants.
constexpr int GuestInvalid = 22;
constexpr int GuestNoMemory = 12;
constexpr int GuestAccess = 13;
constexpr int GuestNotSupported = 45;
constexpr int GuestBadDescriptor = 9;
constexpr int GuestShared = 0x1;
constexpr int GuestMapNoCoalesce = 0x400000;
constexpr int GuestPrivate = 0x2;
constexpr int GuestAnonymous = 0x1000;
constexpr int GuestMapFixed = 0x10;
constexpr int GuestMapExclusive = 0x4000;
constexpr int GuestMapNoOverwrite = 0x80;
constexpr int GuestSyncAsync = 0x1;
constexpr int GuestSyncInvalidate = 0x2;
constexpr int GuestAdviceCore = 9;
constexpr int GuestAdviceProtect = 10;
constexpr int GuestLockCurrent = 0x1;
constexpr int GuestLockFuture = 0x2;
constexpr std::uintptr_t GuestUserAddressEnd = 0x800000000000;
constexpr int GuestAlignedSuper = 0x1000000;
constexpr std::size_t SuperpageSize = std::size_t{1} << 21;

#if defined(__linux__)
// A private duplicate of a guest descriptor (guest descriptors are host descriptors on Linux).
class ScopedDescriptor {
public:
    explicit ScopedDescriptor(int value) noexcept : descriptor(fcntl(value, F_DUPFD_CLOEXEC, 0)), error(descriptor < 0 ? errno : 0) {}
    ~ScopedDescriptor() {
        const int savedError = errno;
        if (descriptor >= 0) close(descriptor);
        errno = savedError;
    }
    ScopedDescriptor(const ScopedDescriptor&) = delete;
    ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;
    int Get() const noexcept { return descriptor; }
    int Error() const noexcept { return error; }
private:
    int descriptor = -1;
    int error = 0;
};

bool RegularFile(int descriptor) {
    struct stat status{};
    return ::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode);
}
#endif

bool RoundLength(std::size_t length, std::size_t& rounded) {
    constexpr auto mask = PS5_PAGE_SIZE - 1;
    if (length == 0 || length > std::numeric_limits<std::size_t>::max() - mask)
        return false;
    rounded = (length + mask) & ~mask;
    return true;
}

void SetError(int error) {
    *__error_nid_postfix() = error;
}

// SCE kernel errors are 0x8002xxxx with the errno in the low bits.
int GuestErrno(int result) {
    return result & 0xffff;
}

bool Mapped(std::uintptr_t address) {
    VirtualQueryInfo info{};
    return sceKernelVirtualQuery(reinterpret_cast<const void*>(address), 0, &info, sizeof(info)) == 0;
}
}

extern "C" {

void* APS5_VABI mmap_nid_postfix(void* address, std::size_t length, int protection,
                                int flags, int descriptor, std::int64_t offset) noexcept {
    const auto failed = [](int error) -> void* {
        SetError(error);
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(-1));
    };
    std::size_t rounded;
    if (!RoundLength(length, rounded) || (protection & ~7) != 0)
        return failed(GuestInvalid);
    if ((flags & (GuestMapExclusive | GuestMapFixed)) == GuestMapExclusive) return failed(GuestInvalid);
    const int mappingFlags = flags & ~GuestAlignedSuper;
    const bool fixedAnonymous = (mappingFlags & ~(GuestMapFixed | GuestMapExclusive)) == (GuestPrivate | GuestAnonymous)
                                && (mappingFlags & GuestMapFixed) != 0;
    if (mappingFlags == (GuestPrivate | GuestAnonymous) || fixedAnonymous) {
        if (descriptor != -1 || offset != 0) return failed(GuestInvalid);
        const auto start = reinterpret_cast<std::uintptr_t>(address);
        // The guest memory backing checks that a fixed range lies in the guest address space.
        if (fixedAnonymous && ((start & (PS5_PAGE_SIZE - 1)) != 0 ||
            rounded > std::numeric_limits<std::uintptr_t>::max() - start)) return failed(GuestInvalid);
        // A fixed mapping must lie in the application's address area (shadPS4's USER_MIN..USER_MAX
        // end, upstream's guest arena); outside it the kernel reports EINVAL.
        constexpr std::uintptr_t appAreaStart = 0x200000000ull;
        constexpr std::uintptr_t appAreaEnd = 0xFC00000000ull;
        if (fixedAnonymous && (start < appAreaStart || start >= appAreaEnd || rounded > appAreaEnd - start)) return failed(GuestInvalid);
        // MAP_EXCL: a fixed mapping that must not replace existing mappings (MAP_NO_OVERWRITE).
        const bool exclusive = fixedAnonymous && (mappingFlags & GuestMapExclusive) != 0;
        const int placement = fixedAnonymous ? GuestMapFixed | (exclusive ? GuestMapNoOverwrite : 0) : 0;
        void* mapped = fixedAnonymous ? address : nullptr;
        const auto alignment = !fixedAnonymous && (flags & GuestAlignedSuper) != 0 ? SuperpageSize : PS5_PAGE_SIZE;
        try {
            const auto result = DoMapAnon(&mapped, rounded, protection, placement, alignment);
            if (result != 0) return failed(GuestErrno(result));
            return mapped;
        } catch (const std::invalid_argument&) {
            return failed(fixedAnonymous ? GuestInvalid : GuestNoMemory);
        } catch (const std::bad_alloc&) {
            return failed(GuestNoMemory);
        } catch (const std::exception&) {
            return failed(GuestNoMemory);
        }
    }
    // Shared-memory objects and regular files (upstream bc71ffb43), as GuestMemoryBacking views of
    // the host object.
    const int kind = mappingFlags & (GuestShared | GuestPrivate);
    const int placement = mappingFlags & (GuestMapFixed | GuestMapNoOverwrite | GuestMapNoCoalesce);
    if ((mappingFlags & GuestAnonymous) != 0 ||
        (kind != GuestShared && kind != GuestPrivate) ||
        (mappingFlags & ~(GuestShared | GuestPrivate | GuestMapFixed | GuestMapNoOverwrite | GuestMapNoCoalesce)) != 0)
        return failed(GuestNotSupported);
    if (descriptor < 0) return failed(GuestBadDescriptor);
    if (offset < 0 || (static_cast<std::uint64_t>(offset) & (PS5_PAGE_SIZE - 1)) != 0 ||
        rounded > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
        static_cast<std::uint64_t>(offset) > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - rounded)
        return failed(GuestInvalid);
    if ((placement & GuestMapFixed) != 0 && (reinterpret_cast<std::uintptr_t>(address) & (PS5_PAGE_SIZE - 1)) != 0) return failed(GuestInvalid);
#if defined(__linux__)
    // The object is pinned first: a descriptor the guest closes or reuses meanwhile does not change
    // what is mapped.
    const ScopedDescriptor pinned(descriptor);
    if (pinned.Get() < 0) return failed(pinned.Error() == EBADF ? GuestBadDescriptor : GuestInvalid);
    const int descriptorFlags = fcntl(pinned.Get(), F_GETFL);
    if (descriptorFlags < 0) return failed(GuestBadDescriptor);
    const int accessMode = descriptorFlags & O_ACCMODE;
    if ((protection & 1) != 0 && accessMode == O_WRONLY) return failed(GuestAccess);
    if (kind == GuestShared && (protection & 2) != 0 && accessMode == O_RDONLY) return failed(GuestAccess);
    if (!File::IsSharedMemoryDescriptor_nid_no_patch(pinned.Get()) && !RegularFile(pinned.Get())) return failed(GuestNotSupported);
    const auto object = static_cast<std::uintptr_t>(pinned.Get());
    // A shared view of an object opened read-only stays read-only (mprotect reports EACCES).
    const int maxProtection = kind == GuestShared && accessMode == O_RDONLY ? 0x5 : -1;
#else
    // Windows guest descriptors are CRT descriptors without a file mapping object to show.
    (void)placement;
    return failed(GuestNotSupported);
#endif
#if defined(__linux__)
    void* mapped = address;
    try {
        const auto result = DoMapObject(&mapped, rounded, protection, placement, object, static_cast<std::uint64_t>(offset), kind == GuestPrivate, maxProtection);
        if (result != 0) return failed(GuestErrno(result));
        return mapped;
    } catch (const std::system_error& error) {
        const int hostError = error.code().value();
        if (hostError == ENOTSUP || hostError == EOPNOTSUPP) return failed(GuestNotSupported);
        if (hostError == EACCES || hostError == EPERM) return failed(GuestAccess);
        if (hostError == EBADF) return failed(GuestBadDescriptor);
        return failed(GuestNoMemory);
    } catch (const std::invalid_argument&) {
        return failed(GuestInvalid);
    } catch (const std::exception&) {
        return failed(GuestNoMemory);
    }
#endif
}

int APS5_VABI munmap_nid_postfix(void* address, std::size_t length) noexcept {
    const auto failed = [](int error) {
        SetError(error);
        return -1;
    };
    std::size_t rounded;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (!RoundLength(length, rounded) || start == 0 ||
        (start & (PS5_PAGE_SIZE - 1)) != 0 ||
        rounded > std::numeric_limits<std::uintptr_t>::max() - start)
        return failed(GuestInvalid);
    try {
        if (const auto result = DoMunmap(address, rounded); result != 0) return failed(GuestErrno(result));
        return 0;
    } catch (const std::bad_alloc&) {
        return failed(GuestNoMemory);
    } catch (const std::exception&) {
        // The allocation tracker rejects foreign, pinned, and noncontiguous ranges.
        return failed(GuestInvalid);
    }
}

int APS5_VABI mprotect_nid_postfix(void* address, std::size_t length, int protection) noexcept {
    const auto failed = [](int error) {
        SetError(error);
        return -1;
    };
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (length == 0) return 0;
    if (start == 0 || length > std::numeric_limits<std::uintptr_t>::max() - start)
        return failed(GuestInvalid);
    try {
        if (const auto result = DoMprotect(address, length, protection); result != 0) return failed(GuestErrno(result));
        return 0;
    } catch (const std::bad_alloc&) {
        return failed(GuestNoMemory);
    } catch (const std::system_error& error) {
        const int hostError = error.code().value();
        return failed(hostError == EACCES || hostError == EPERM ? GuestAccess : GuestInvalid);
    } catch (const std::exception&) {
        return failed(GuestInvalid);
    }
}

int APS5_VABI mlock_nid_postfix(const void* address, std::size_t length) {
    const int result = sceKernelMlock_nid_postfix(const_cast<void*>(address), length);
    if (result == 0) return 0;
    SetError(result & 0xffff);
    return -1;
}

int APS5_VABI munlock_nid_postfix(const void* address, std::size_t length) {
    const int result = sceKernelMunlock_nid_postfix(const_cast<void*>(address), length);
    if (result == 0) return 0;
    SetError(result & 0xffff);
    return -1;
}

int APS5_VABI mlockall_nid_postfix(int flags) {
    if (flags == 0 || (flags & ~(GuestLockCurrent | GuestLockFuture)) != 0) {
        SetError(GuestInvalid);
        return -1;
    }
    if ((flags & GuestLockFuture) != 0) throw std::runtime_error("mlockall: MCL_FUTURE is not implemented: later mappings are not locked");
    throw std::runtime_error("mlockall: MCL_CURRENT is not implemented: the process also holds host stacks, heaps, modules and driver mappings");
}

int APS5_VABI msync_nid_postfix(void* address, std::size_t length, int flags) {
    const auto failed = [](int error) {
        SetError(error);
        return -1;
    };
    constexpr std::uintptr_t mask = PS5_PAGE_SIZE - 1;
    const auto first = reinterpret_cast<std::uintptr_t>(address);
    const auto start = first & ~mask;
    const std::uintptr_t size = (length + (first - start) + mask) & ~mask;
    const auto end = start + size;
    if (end < start) return failed(GuestInvalid);
    if ((flags & (GuestSyncAsync | GuestSyncInvalidate)) == (GuestSyncAsync | GuestSyncInvalidate)) return failed(GuestInvalid);
    if (start == end ? !Mapped(start) : !GuestRangeMapped(start, end)) return failed(GuestNoMemory);
    if ((flags & GuestSyncInvalidate) != 0) throw std::runtime_error("msync: MS_INVALIDATE is not implemented");
    return 0;
}

int APS5_VABI madvise_nid_postfix(void* address, std::size_t length, int advice) {
    if (advice == GuestAdviceProtect) return 0;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (advice < 0 || advice > GuestAdviceCore || start > GuestUserAddressEnd || length > GuestUserAddressEnd - start) {
        SetError(GuestInvalid);
        return -1;
    }
    return 0;
}

}
