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
#include "prx/libc/include/GuestMemoryTracking.hpp"
#include <cstdarg>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/utime.h>
#else
#include <sys/time.h>
#include <sys/uio.h>
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

}

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

}

// Underscore-prefixed POSIX aliases and file calls some titles import (from upstream).
extern "C" int APS5_VABI close_nid_postfix(int descriptor);

extern "C" {

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

std::int64_t APS5_VABI _read_nid_postfix(int descriptor, void* buffer, std::size_t count) {
    return sceKernelRead(descriptor, buffer, count);
}

std::int64_t APS5_VABI _write_nid_postfix(int descriptor, const void* buffer, std::size_t count) {
    return sceKernelWrite(descriptor, buffer, count);
}

// File metadata on the host file the guest path resolves to (from upstream a4e755c).
int APS5_VABI sceKernelChmod_nid_postfix(const char* path, std::uint16_t mode) {
    if (path == nullptr) return FileErrors::Sce(EFAULT);
    std::error_code error;
    std::filesystem::permissions(ResolvePath_nid_no_patch(path), static_cast<std::filesystem::perms>(mode & 07777u), std::filesystem::perm_options::replace, error);
    return error ? FileErrors::Sce(error.value()) : 0;
}

int APS5_VABI sceKernelTruncate_nid_postfix(const char* path, std::int64_t length) {
    if (path == nullptr) return FileErrors::Sce(EFAULT);
    if (length < 0) return FileErrors::Sce(EINVAL);
    const auto native = ResolvePath_nid_no_patch(path);
    std::error_code error;
    if (!std::filesystem::exists(native, error)) return FileErrors::Sce(ENOENT);
    std::filesystem::resize_file(native, static_cast<std::uintmax_t>(length), error);
    return error ? FileErrors::Sce(error.value()) : 0;
}

int APS5_VABI sceKernelUtimes_nid_postfix(const char* path, const KernelTimeval* times) {
    if (path == nullptr) return FileErrors::Sce(EFAULT);
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
#endif

}

extern "C" int APS5_VABI sceKernelFsync(int fd);

extern "C" {

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
    return sceResult64(static_cast<std::int64_t>(::preadv(d, nativeIovecs(iov), iovcnt, static_cast<off_t>(offset))));
}

std::int64_t APS5_VABI sceKernelPwritev(int d, const KernelIovec* iov, int iovcnt, std::int64_t offset) {
    if (const int error = checkIovecs(iov, iovcnt)) return error;
    if (offset < 0) return FileErrors::SceBsd(kErrorInvalid);
    resolveIovecs(iov, iovcnt, false);
    return sceResult64(static_cast<std::int64_t>(::pwritev(d, nativeIovecs(iov), iovcnt, static_cast<off_t>(offset))));
}

#endif

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

}
