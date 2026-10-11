#include <cerrno>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/File/include/FileErrors.hpp"
#include "prx/libkernel/File/include/File.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include "prx/libkernel/File/include/DirectoryDescriptor.hpp"
#include "prx/libkernel/File/include/NativeStat.hpp"
#include "prx/libkernel/File/include/FileLock.hpp"
#include "prx/libkernel/File/include/RandomDevice.hpp"
#include "prx/libkernel/Socket/include/SocketRuntime.hpp"
#include "prx/libc/include/GuestMemoryTracking.hpp"
#include <cstdarg>
#include <deque>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/utime.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#endif

// FreeBSD struct iovec.
struct KernelIovec {
    void* base;
    std::size_t length;
};

// FreeBSD struct statfs (upstream 9a3e55b8e).
struct KernelStatfs {
    std::uint32_t f_version;
    std::uint32_t f_type;
    std::uint64_t f_flags;
    std::uint64_t f_bsize;
    std::uint64_t f_iosize;
    std::uint64_t f_blocks;
    std::uint64_t f_bfree;
    std::int64_t f_bavail;
    std::uint64_t f_files;
    std::int64_t f_ffree;
    std::uint64_t f_syncwrites;
    std::uint64_t f_asyncwrites;
    std::uint64_t f_syncreads;
    std::uint64_t f_asyncreads;
    std::uint64_t f_spare[10];
    std::uint32_t f_namemax;
    std::uint32_t f_owner;
    std::int32_t f_fsid[2];
    char f_charspare[80];
    char f_fstypename[16];
    char f_mntfromname[88];
    char f_mntonname[88];
};
static_assert(sizeof(KernelStatfs) == 472);
static_assert(offsetof(KernelStatfs, f_bavail) == 48);
static_assert(offsetof(KernelStatfs, f_namemax) == 184);
static_assert(offsetof(KernelStatfs, f_fstypename) == 280);
static_assert(offsetof(KernelStatfs, f_mntonname) == 384);

namespace {

constexpr int kIovMax = 1024;

constexpr std::size_t kBsdDirentHeader = 8;
constexpr std::size_t kHostBufferSize = 32768;
constexpr int kErrorFault = 14;
constexpr int kErrorInvalid = 22;

std::size_t bsdRecordLength(std::size_t nameLength) {
    return (kBsdDirentHeader + nameLength + 1 + 3) & ~static_cast<std::size_t>(3);
}

int readDirectory(int fd, char* buf, int nbytes) {
#if defined(__linux__)
    if (buf == nullptr) return FileErrors::SceBsd(kErrorFault);
    if (nbytes <= 0) return FileErrors::SceBsd(kErrorInvalid);
    std::vector<char> host(kHostBufferSize);
    const off_t start = ::lseek(fd, 0, SEEK_CUR);
    if (start < 0) return FileErrors::Sce(errno);
    const long received = ::syscall(SYS_getdents64, fd, host.data(), host.size());
    if (received < 0) return FileErrors::Sce(errno);
    std::size_t written = 0;
    off_t position = start;
    for (long offset = 0; offset < received;) {
        const char* record = host.data() + offset;
        std::uint64_t inode = 0;
        std::int64_t next = 0;
        std::uint16_t hostLength = 0;
        std::memcpy(&inode, record, 8);
        std::memcpy(&next, record + 8, 8);
        std::memcpy(&hostLength, record + 16, 2);
        const auto type = static_cast<std::uint8_t>(record[18]);
        const char* name = record + 19;
        const std::size_t nameLength = std::strlen(name);
        const std::size_t length = bsdRecordLength(nameLength);
        if (nameLength > 255 || written + length > static_cast<std::size_t>(nbytes)) {
            if (written == 0) {
                // Nothing fits: the call fails without moving the directory position.
                if (::lseek(fd, start, SEEK_SET) < 0) return FileErrors::Sce(errno);
                return FileErrors::SceBsd(kErrorInvalid);
            }
            if (::lseek(fd, position, SEEK_SET) < 0) return FileErrors::Sce(errno);
            break;
        }
        char* out = buf + written;
        std::memset(out, 0, length);
        const auto fileNumber = static_cast<std::uint32_t>(inode);
        const auto recordLength = static_cast<std::uint16_t>(length);
        std::memcpy(out, &fileNumber, 4);
        std::memcpy(out + 4, &recordLength, 2);
        out[6] = static_cast<char>(type);
        out[7] = static_cast<char>(nameLength);
        std::memcpy(out + 8, name, nameLength);
        written += length;
        offset += hostLength;
        position = static_cast<off_t>(next);
    }
    return static_cast<int>(written);
#elif defined(_WIN32)
    if (buf == nullptr) return FileErrors::SceBsd(kErrorFault);
    if (nbytes <= 0) return FileErrors::SceBsd(kErrorInvalid);
    // Directories open as stand-in descriptors on Windows (see Open.cpp).
    const int written = File::ReadDirectoryDescriptor(fd, buf, nbytes);
    return written < 0 ? FileErrors::SceBsd(20) : written;
#else
    (void)fd;
    (void)buf;
    (void)nbytes;
    NotImplemented_nid_no_patch("sceKernelGetdents");
    return 0;
#endif
}

constexpr int kErrorBadDescriptor = 9;
constexpr std::uint32_t kStatfsVersion = 0x20030518;
constexpr std::uint64_t kMountReadOnly = 0x1;
constexpr std::uint64_t kMountNoExec = 0x4;
constexpr std::uint64_t kMountNoSuid = 0x8;

#ifdef _WIN32
extern "C" _invalid_parameter_handler _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler);
void ignoreDescriptorParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {}
template <typename TCall> int withoutParameterHandler(TCall call) {
    const auto previous = _set_thread_local_invalid_parameter_handler(ignoreDescriptorParameter);
    const int result = call();
    _set_thread_local_invalid_parameter_handler(previous);
    return result;
}
void rejectDirectoryDuplicate(int descriptor, const char* function) {
    if (File::DirectoryDescriptorPath(descriptor))
        throw std::runtime_error(std::string(function) + ": duplicating a directory descriptor is not supported on Windows");
}
#else
// The guest's struct statfs for a host descriptor; a BSD error number on failure (upstream 9a3e55b8e).
int nativeFstatfs(int descriptor, KernelStatfs* fs) {
    constexpr long PipeFilesystem = 0x50495045;
    struct stat file{};
    if (::fstat(descriptor, &file) != 0) return FileErrors::ToBsd(errno);
    if (S_ISSOCK(file.st_mode)) return kErrorInvalid;
    if (S_ISFIFO(file.st_mode)) {
        struct statfs kind{};
        if (::fstatfs(descriptor, &kind) == 0 && static_cast<long>(kind.f_type) == PipeFilesystem) return kErrorInvalid;
    }
    struct statvfs host{};
    if (::fstatvfs(descriptor, &host) != 0) return FileErrors::ToBsd(errno);
    if (host.f_flag & ST_RDONLY) fs->f_flags |= kMountReadOnly;
    if (host.f_flag & ST_NOSUID) fs->f_flags |= kMountNoSuid;
    if (host.f_flag & ST_NOEXEC) fs->f_flags |= kMountNoExec;
    fs->f_bsize = host.f_frsize != 0 ? host.f_frsize : host.f_bsize;
    fs->f_iosize = host.f_bsize;
    fs->f_blocks = host.f_blocks;
    fs->f_bfree = host.f_bfree;
    fs->f_bavail = static_cast<std::int64_t>(host.f_bavail);
    fs->f_files = host.f_files;
    fs->f_ffree = static_cast<std::int64_t>(host.f_favail);
    fs->f_namemax = static_cast<std::uint32_t>(host.f_namemax);
    return 0;
}
#endif

}

#ifdef _WIN32
bool DescriptorIsOpen_nid_no_patch(int descriptor) {
    return descriptor >= 0 && withoutParameterHandler([descriptor] { return ::_get_osfhandle(descriptor) == -1 ? -1 : 0; }) == 0;
}
#else
bool DescriptorIsOpen_nid_no_patch(int descriptor) {
    return descriptor >= 0 && ::fcntl(descriptor, F_GETFD) >= 0;
}
#endif

extern "C" {

int APS5_VABI sceKernelGetdents(int fd, char* buf, int nbytes) {
    return readDirectory(fd, buf, nbytes);
}

int APS5_VABI sceKernelGetdirentries(int fd, char* buf, int nbytes, int64_t* basep) {
#if defined(_WIN32)
    if (basep) *basep = 0;
#elif defined(__linux__)
    if (basep) {
        const off_t position = ::lseek(fd, 0, SEEK_CUR);
        if (position < 0) return FileErrors::Sce(errno);
        *basep = position;
    }
#endif
    return readDirectory(fd, buf, nbytes);
}

int APS5_VABI getdirentries_nid_postfix(int fd, char* buf, int nbytes, int64_t* basep) {
    const int result = sceKernelGetdirentries(fd, buf, nbytes, basep);
    return result < 0 ? FileErrors::PosixBsd(result & 0xffff) : result;
}

int APS5_VABI getdents_nid_postfix(int fd, char* buf, int nbytes) {
    const int result = sceKernelGetdents(fd, buf, nbytes);
    return result < 0 ? FileErrors::PosixBsd(result & 0xffff) : result;
}

int APS5_VABI lstat_nid_postfix(const char* path, FileStat* sb) {
    if (path == nullptr || sb == nullptr) return FileErrors::PosixBsd(kErrorFault);
    if (*path == '\0') return FileErrors::PosixBsd(2);
    GuestMemoryTracking::GuestMemoryTrackingResolve_nid_postfix(reinterpret_cast<std::uint64_t>(sb), sizeof(*sb), true);
    if (!File::FillLinkStat(ResolvePath_nid_no_patch(path), sb)) return FileErrors::Posix(errno);
    return 0;
}

}

// Underscore-prefixed POSIX aliases and file calls some titles import (from upstream).
extern "C" int APS5_VABI close_nid_postfix(int descriptor);

extern "C" {

int APS5_VABI dup_nid_postfix(int d) {
    if (d >= GuestSockets::FirstDescriptor) return GuestSockets::Duplicate(d);
    if (d < 0) return FileErrors::PosixBsd(kErrorBadDescriptor);
#ifdef _WIN32
    rejectDirectoryDuplicate(d, __func__);
    const int duplicate = withoutParameterHandler([d] { return ::_dup(d); });
#else
    const int duplicate = ::dup(d);
#endif
    if (duplicate < 0) return FileErrors::Posix(errno);
    if (duplicate >= GuestSockets::FirstDescriptor)
        throw std::runtime_error(std::string(__func__) + ": host descriptor reached the guest socket range");
    if (File::IsRandomDevice(d)) File::RememberRandomDevice(duplicate);
    return duplicate;
}

int APS5_VABI dup2_nid_postfix(int from, int to) {
    if (from < 0 || to < 0) return FileErrors::PosixBsd(kErrorBadDescriptor);
    const bool socketFrom = from >= GuestSockets::FirstDescriptor;
    const bool socketTo = to >= GuestSockets::FirstDescriptor;
    if (socketFrom && socketTo) return GuestSockets::DuplicateTo(from, to);
    if (socketFrom) {
        if (!GuestSockets::IsOpen(from)) return FileErrors::PosixBsd(kErrorBadDescriptor);
        throw std::runtime_error(std::string(__func__) + ": moving a guest socket onto host descriptor " + std::to_string(to) + " is not supported");
    }
    if (socketTo)
        throw std::runtime_error(std::string(__func__) + ": moving host descriptor " + std::to_string(from) + " into the guest socket range is not supported");
    const bool sourceIsRandom = File::IsRandomDevice(from);
#ifdef _WIN32
    if (!DescriptorIsOpen_nid_no_patch(from)) return FileErrors::PosixBsd(kErrorBadDescriptor);
    if (from == to) return to;
    rejectDirectoryDuplicate(from, __func__);
    if (withoutParameterHandler([from, to] { return ::_dup2(from, to); }) != 0) return FileErrors::Posix(errno);
    File::ForgetDirectoryDescriptor(to);
    File::ForgetFileLock(to);
    File::ForgetRandomDevice(to);
    if (sourceIsRandom) File::RememberRandomDevice(to);
    return to;
#else
    if (from == to) {
        if (::fcntl(from, F_GETFL) < 0) return FileErrors::PosixBsd(kErrorBadDescriptor);
        return to;
    }
    const int result = ::dup2(from, to);
    if (result < 0) return FileErrors::Posix(errno);
    File::ForgetRandomDevice(to);
    if (sourceIsRandom) File::RememberRandomDevice(to);
    return result;
#endif
}

int APS5_VABI _fstatfs_nid_postfix(int d, KernelStatfs* buf) {
    if (d >= GuestSockets::FirstDescriptor) return FileErrors::PosixBsd(GuestSockets::IsOpen(d) ? kErrorInvalid : kErrorBadDescriptor);
    if (d < 0) return FileErrors::PosixBsd(kErrorBadDescriptor);
#ifdef _WIN32
    (void)buf;
    throw std::runtime_error("_fstatfs: not implemented on Windows in this branch");
#else
    KernelStatfs result{};
    result.f_version = kStatfsVersion;
    if (const int error = nativeFstatfs(d, &result)) return FileErrors::PosixBsd(error);
    if (buf == nullptr) return FileErrors::PosixBsd(kErrorFault);
    GuestMemoryTracking::GuestMemoryTrackingResolve_nid_postfix(reinterpret_cast<std::uint64_t>(buf), sizeof(result), true);
    std::memcpy(buf, &result, sizeof(result));
    return 0;
#endif
}

int APS5_VABI _close_nid_postfix(int descriptor) {
    return close_nid_postfix(descriptor);
}

extern "C" int APS5_VABI open_nid_postfix(const char* path, int flags, int mode);

int APS5_VABI _open_nid_postfix(const char* path, int flags, ...) {
    std::uint16_t mode = 0;
    if (flags & SCE_KERNEL_O_CREAT) {
#ifdef _WIN32
        __builtin_sysv_va_list arguments;
        __builtin_sysv_va_start(arguments, flags);
        mode = static_cast<std::uint16_t>(__builtin_va_arg(arguments, int));
        __builtin_sysv_va_end(arguments);
#else
        std::va_list arguments;
        va_start(arguments, flags);
        mode = static_cast<std::uint16_t>(va_arg(arguments, int));
        va_end(arguments);
#endif
    }
    // POSIX: -1 with errno, not an SCE error code (upstream).
    return open_nid_postfix(path, flags, mode);
}

// A socket descriptor reports failures as -1 with errno, as read and write do (upstream).
std::int64_t APS5_VABI _read_nid_postfix(int descriptor, void* buffer, std::size_t count) {
    if (descriptor >= GuestSockets::FirstDescriptor) return GuestSockets::Read(descriptor, buffer, count);
    return sceKernelRead(descriptor, buffer, count);
}

std::int64_t APS5_VABI _write_nid_postfix(int descriptor, const void* buffer, std::size_t count) {
    if (descriptor >= GuestSockets::FirstDescriptor) return GuestSockets::Write(descriptor, buffer, count);
    return sceKernelWrite(descriptor, buffer, count);
}

// File metadata on the host file the guest path resolves to (from upstream a4e755c).
int APS5_VABI sceKernelChmod_nid_postfix(const char* path, std::uint16_t mode) {
    if (path == nullptr) return FileErrors::Sce(EFAULT);
    std::error_code error;
    const auto native = ResolvePath_nid_no_patch(path);
    std::filesystem::permissions(native, static_cast<std::filesystem::perms>(mode & 07777u), std::filesystem::perm_options::replace, error);
    if (error) return FileErrors::Sce(error.value());
    RecordWrittenPath_nid_no_patch(native);
    return 0;
}

int APS5_VABI sceKernelTruncate_nid_postfix(const char* path, std::int64_t length) {
    if (path == nullptr) return FileErrors::Sce(EFAULT);
    if (length < 0) return FileErrors::Sce(EINVAL);
    const auto native = ResolvePath_nid_no_patch(path);
    std::error_code error;
    if (!std::filesystem::exists(native, error)) return FileErrors::Sce(ENOENT);
    std::filesystem::resize_file(native, static_cast<std::uintmax_t>(length), error);
    if (error) return FileErrors::Sce(error.value());
    RecordWrittenPath_nid_no_patch(native);
    return 0;
}

int APS5_VABI sceKernelUtimes_nid_postfix(const char* path, const KernelTimeval* times) {
    if (path == nullptr) return FileErrors::Sce(EFAULT);
    if (times != nullptr) {
        for (int i = 0; i < 2; ++i)
            if (times[i].tv_usec < 0 || times[i].tv_usec >= 1000000) return FileErrors::SceBsd(kErrorInvalid);
    }
    const auto native = ResolvePath_nid_no_patch(path);
#ifdef _WIN32
    struct _utimbuf values{};
    if (times != nullptr) values = {static_cast<time_t>(times[0].tv_sec), static_cast<time_t>(times[1].tv_sec)};
    if (::_wutime(native.wstring().c_str(), times != nullptr ? &values : nullptr) != 0) return FileErrors::Sce(errno);
#else
    struct timeval values[2]{};
    if (times != nullptr) {
        values[0] = {static_cast<time_t>(times[0].tv_sec), static_cast<suseconds_t>(times[0].tv_usec)};
        values[1] = {static_cast<time_t>(times[1].tv_sec), static_cast<suseconds_t>(times[1].tv_usec)};
    }
    if (::utimes(native.c_str(), times != nullptr ? values : nullptr) != 0) return FileErrors::Sce(errno);
#endif
    return 0;
}

}

// Vectored I/O, pipe and fsync (from upstream ab98cd7f and 7789c0a5).
namespace {

int checkIovecs(const KernelIovec* iov, const int iovcnt) {
    if (iovcnt < 0 || iovcnt > kIovMax) return FileErrors::SceBsd(kErrorInvalid);
    if (iov == nullptr && iovcnt != 0) return FileErrors::SceBsd(kErrorFault);
    return 0;
}

// As for any system call on guest memory: tracked pages are resolved first (see Open.cpp).
void resolveIovecs(const KernelIovec* iov, const int iovcnt, const bool writable) {
    for (int index = 0; index < iovcnt; ++index) {
        if (iov[index].base != nullptr && iov[index].length != 0)
            GuestMemoryTracking::GuestMemoryTrackingResolve_nid_postfix(reinterpret_cast<std::uint64_t>(iov[index].base), iov[index].length, writable);
    }
}

#ifndef _WIN32
static_assert(sizeof(KernelIovec) == sizeof(struct iovec));
static_assert(offsetof(KernelIovec, base) == offsetof(struct iovec, iov_base));
static_assert(offsetof(KernelIovec, length) == offsetof(struct iovec, iov_len));

const struct iovec* nativeIovecs(const KernelIovec* iov) {
    return reinterpret_cast<const struct iovec*>(iov);
}

std::int64_t sceResult64(const std::int64_t result) {
    return result < 0 ? FileErrors::Sce(errno) : result;
}

std::int64_t readRandomIovecs(int d, const KernelIovec* iov, int iovcnt) {
    std::int64_t total = 0;
    for (int index = 0; index < iovcnt; ++index) {
        File::ReadRandomDevice(d, iov[index].base, iov[index].length);
        total += static_cast<std::int64_t>(iov[index].length);
    }
    return total;
}
#endif

}

extern "C" int APS5_VABI sceKernelFsync(int fd);

extern "C" {

// The write throttling counters (upstream): nothing is throttled.
int APS5_VABI sceKernelWriteThrottlingStatus(std::uint64_t* status) {
    if (status == nullptr) throw std::invalid_argument("sceKernelWriteThrottlingStatus: status is null");
    status[0] = std::numeric_limits<std::uint32_t>::max();
    status[1] = 0;
    status[2] = 0;
    status[3] = 0;
    return 0;
}

#ifdef _WIN32

std::int64_t APS5_VABI sceKernelReadv(int, const KernelIovec*, int) {
    throw std::runtime_error(std::string(__func__) + ": not implemented on Windows");
}

std::int64_t APS5_VABI sceKernelWritev(int, const KernelIovec*, int) {
    throw std::runtime_error(std::string(__func__) + ": not implemented on Windows");
}

std::int64_t APS5_VABI sceKernelPreadv(int, const KernelIovec*, int, std::int64_t) {
    throw std::runtime_error(std::string(__func__) + ": not implemented on Windows");
}

std::int64_t APS5_VABI sceKernelPwritev(int, const KernelIovec*, int, std::int64_t) {
    throw std::runtime_error(std::string(__func__) + ": not implemented on Windows");
}

#else

std::int64_t APS5_VABI sceKernelReadv(int d, const KernelIovec* iov, int iovcnt) {
    if (const int error = checkIovecs(iov, iovcnt)) return error;
    resolveIovecs(iov, iovcnt, true);
    if (File::IsRandomDevice(d)) return readRandomIovecs(d, iov, iovcnt);
    return sceResult64(static_cast<std::int64_t>(::readv(d, nativeIovecs(iov), iovcnt)));
}

std::int64_t APS5_VABI sceKernelWritev(int d, const KernelIovec* iov, int iovcnt) {
    if (const int error = checkIovecs(iov, iovcnt)) return error;
    resolveIovecs(iov, iovcnt, false);
    return sceResult64(static_cast<std::int64_t>(::writev(d, nativeIovecs(iov), iovcnt)));
}

std::int64_t APS5_VABI sceKernelPreadv(int d, const KernelIovec* iov, int iovcnt, std::int64_t offset) {
    if (const int error = checkIovecs(iov, iovcnt)) return error;
    if (offset < 0) return FileErrors::SceBsd(kErrorInvalid);
    resolveIovecs(iov, iovcnt, true);
    if (File::IsRandomDevice(d)) return readRandomIovecs(d, iov, iovcnt);
    return sceResult64(static_cast<std::int64_t>(::preadv(d, nativeIovecs(iov), iovcnt, static_cast<off_t>(offset))));
}

std::int64_t APS5_VABI sceKernelPwritev(int d, const KernelIovec* iov, int iovcnt, std::int64_t offset) {
    if (const int error = checkIovecs(iov, iovcnt)) return error;
    if (offset < 0) return FileErrors::SceBsd(kErrorInvalid);
    resolveIovecs(iov, iovcnt, false);
    return sceResult64(static_cast<std::int64_t>(::pwritev(d, nativeIovecs(iov), iovcnt, static_cast<off_t>(offset))));
}

#endif

std::int64_t APS5_VABI preadv_nid_postfix(int d, const KernelIovec* iov, int iovcnt, std::int64_t offset) {
    const auto result = sceKernelPreadv(d, iov, iovcnt, offset);
    return result < 0 ? FileErrors::PosixBsd(static_cast<int>(result) & 0xffff) : result;
}

std::int64_t APS5_VABI pwritev_nid_postfix(int d, const KernelIovec* iov, int iovcnt, std::int64_t offset) {
    const auto result = sceKernelPwritev(d, iov, iovcnt, offset);
    return result < 0 ? FileErrors::PosixBsd(static_cast<int>(result) & 0xffff) : result;
}

int APS5_VABI pipe_nid_postfix(int* descriptors) {
    if (descriptors == nullptr) return FileErrors::PosixBsd(kErrorFault);
    GuestMemoryTracking::GuestMemoryTrackingResolve_nid_postfix(reinterpret_cast<std::uint64_t>(descriptors), 2 * sizeof(int), true);
    int native[2];
#ifdef _WIN32
    const int result = ::_pipe(native, 4096, _O_BINARY);
#else
    const int result = ::pipe(native);
#endif
    if (result != 0) return FileErrors::Posix(errno);
    std::memcpy(descriptors, native, sizeof(native));
    return 0;
}

int APS5_VABI fsync_nid_postfix(int fd) {
    const int result = sceKernelFsync(fd);
    return result < 0 ? FileErrors::PosixBsd(result & 0xffff) : 0;
}

int APS5_VABI sceKernelFdatasync(int fd) {
    if (fd >= GuestSockets::FirstDescriptor) return FileErrors::SceBsd(GuestSockets::IsOpen(fd) ? kErrorInvalid : kErrorBadDescriptor);
#ifdef _WIN32
    if (::_commit(fd) != 0) return FileErrors::Sce(errno);
#else
    if (::fdatasync(fd) != 0) return FileErrors::Sce(errno);
#endif
    return 0;
}

int APS5_VABI fdatasync_nid_postfix(int fd) {
    const int result = sceKernelFdatasync(fd);
    return result < 0 ? FileErrors::PosixBsd(result & 0xffff) : 0;
}

}
