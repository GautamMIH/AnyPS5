#include "prx/libkernel/File/include/FileFlags.hpp"
#include "prx/libkernel/File/include/FileErrors.hpp"
#include "prx/libkernel/File/include/NativeStat.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Socket/include/SocketRuntime.hpp"
#include "prx/libkernel/File/include/File.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "SceTypes.hpp"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/utime.h>
// Guest files never use descriptors 0-2 (see the Linux NativeOpen).
static int NativeOpen(const std::filesystem::path& p, int nativeFlags, int mode) {
    int fd = ::_wopen(p.wstring().c_str(), nativeFlags, mode);
    while (fd >= 0 && fd < 3) {
        const int moved = ::_dup(fd);
        const int error = errno;
        ::_close(fd);
        errno = error;
        fd = moved;
    }
    return fd;
}
static std::int64_t NativeLseek(int fd, std::int64_t offset, int whence) {
    return ::_lseeki64(fd, offset, whence);
}
static std::int64_t NativeRead(int fd, void* buf, std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
        errno = EINVAL;
        return -1;
    }
    return ::_read(fd, buf, static_cast<unsigned int>(n));
}
static std::int64_t NativeWrite(int fd, const void* buf, std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
        errno = EINVAL;
        return -1;
    }
    return ::_write(fd, buf, static_cast<unsigned int>(n));
}
static std::int64_t NativePread(int fd, void* buf, std::size_t n, std::int64_t offset) {
    const auto previous = ::_lseeki64(fd, 0, SEEK_CUR);
    if (previous < 0 || ::_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    const auto result = NativeRead(fd, buf, n);
    const int error = errno;
    ::_lseeki64(fd, previous, SEEK_SET);
    errno = error;
    return result;
}
static std::int64_t NativePwrite(int fd, const void* buf, std::size_t n, std::int64_t offset) {
    const auto previous = ::_lseeki64(fd, 0, SEEK_CUR);
    if (previous < 0 || ::_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    const auto result = NativeWrite(fd, buf, n);
    const int error = errno;
    ::_lseeki64(fd, previous, SEEK_SET);
    errno = error;
    return result;
}
static int NativeClose(int fd) { return fd >= 0 && fd < 3 ? 0 : ::_close(fd); }
static int NativeUnlink(const std::filesystem::path& p) { return ::_wunlink(p.wstring().c_str()); }
static int NativeMkdir(const std::filesystem::path& p, int) { return ::_wmkdir(p.wstring().c_str()); }
static int NativeRmdir(const std::filesystem::path& p) { return ::_wrmdir(p.wstring().c_str()); }
static int NativeRename(const std::filesystem::path& from, const std::filesystem::path& to) { return ::_wrename(from.wstring().c_str(), to.wstring().c_str()); }
static int NativeChmod(const std::filesystem::path& p, int mode) { return ::_wchmod(p.wstring().c_str(), mode & (_S_IREAD | _S_IWRITE)); }
static int NativeFchmod(int, int) { return 0; }
static int NativeFsync(int fd) { return ::_commit(fd); }
static int NativeFtruncate(int fd, std::int64_t length) { const auto error = ::_chsize_s(fd, length); if (error != 0) { errno = error; return -1; } return 0; }
static int NativeFlock(int, int) { return 0; }
static int NativeUtimes(const std::filesystem::path& p, const KernelTimeval* times) {
    if (times == nullptr) return ::_wutime(p.wstring().c_str(), nullptr);
    struct _utimbuf value{static_cast<time_t>(times[0].tv_sec), static_cast<time_t>(times[1].tv_sec)};
    return ::_wutime(p.wstring().c_str(), &value);
}
static int MapFlags(int sceFlags) {
    int f = 0;
    const int acc = sceFlags & SCE_KERNEL_O_ACCMODE;
    if (acc == SCE_KERNEL_O_RDONLY) f |= _O_RDONLY;
    else if (acc == SCE_KERNEL_O_WRONLY) f |= _O_WRONLY;
    else if (acc == SCE_KERNEL_O_RDWR) f |= _O_RDWR;
    else return -1;
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= _O_APPEND;
    if (sceFlags & SCE_KERNEL_O_CREAT) f |= _O_CREAT;
    if (sceFlags & SCE_KERNEL_O_TRUNC) f |= _O_TRUNC;
    if (sceFlags & SCE_KERNEL_O_EXCL) f |= _O_EXCL;
    f |= _O_BINARY;
    return f;
}
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
// Guest files never use descriptors 0-2: games treat 0 as "no file" and close it (Unity closes
// descriptor 0 at start), so the host's standard streams stay open and a file never lands there.
static int NativeOpen(const std::filesystem::path& p, int nativeFlags, int mode) {
    const int fd = ::open(p.c_str(), nativeFlags, static_cast<mode_t>(mode));
    if (fd < 0 || fd >= 3) return fd;
    const int moved = ::fcntl(fd, (nativeFlags & O_CLOEXEC) != 0 ? F_DUPFD_CLOEXEC : F_DUPFD, 3);
    const int error = errno;
    ::close(fd);
    errno = error;
    return moved;
}
static std::int64_t NativeLseek(int fd, std::int64_t offset, int whence) {
    return ::lseek(fd, static_cast<off_t>(offset), whence);
}
static std::int64_t NativeRead(int fd, void* buf, std::size_t n) { return ::read(fd, buf, n); }
static std::int64_t NativeWrite(int fd, const void* buf, std::size_t n) { return ::write(fd, buf, n); }
static std::int64_t NativePread(int fd, void* buf, std::size_t n, std::int64_t offset) { return ::pread(fd, buf, n, static_cast<off_t>(offset)); }
static std::int64_t NativePwrite(int fd, const void* buf, std::size_t n, std::int64_t offset) { return ::pwrite(fd, buf, n, static_cast<off_t>(offset)); }
// Closing 0-2 leaves the host's standard streams open (guest files never use them).
static int NativeClose(int fd) { return fd >= 0 && fd < 3 ? 0 : ::close(fd); }
static int NativeUnlink(const std::filesystem::path& p) { return ::unlink(p.c_str()); }
static int NativeMkdir(const std::filesystem::path& p, int mode) { return ::mkdir(p.c_str(), static_cast<mode_t>(mode)); }
static int NativeRmdir(const std::filesystem::path& p) { return ::rmdir(p.c_str()); }
static int NativeRename(const std::filesystem::path& from, const std::filesystem::path& to) { return ::rename(from.c_str(), to.c_str()); }
static int NativeChmod(const std::filesystem::path& p, int mode) { return ::chmod(p.c_str(), static_cast<mode_t>(mode)); }
static int NativeFchmod(int fd, int mode) { return ::fchmod(fd, static_cast<mode_t>(mode)); }
static int NativeFsync(int fd) { return ::fsync(fd); }
static int NativeFtruncate(int fd, std::int64_t length) { return ::ftruncate(fd, static_cast<off_t>(length)); }
static int NativeFlock(int fd, int operation) {
    constexpr int bsdShared = 1;
    constexpr int bsdExclusive = 2;
    constexpr int bsdNonBlocking = 4;
    constexpr int bsdUnlock = 8;
    int native = 0;
    if (operation & bsdShared) native |= LOCK_SH;
    if (operation & bsdExclusive) native |= LOCK_EX;
    if (operation & bsdNonBlocking) native |= LOCK_NB;
    if (operation & bsdUnlock) native |= LOCK_UN;
    return ::flock(fd, native);
}
static int NativeUtimes(const std::filesystem::path& p, const KernelTimeval* times) {
    if (times == nullptr) return ::utimes(p.c_str(), nullptr);
    const struct timeval values[2] = {{static_cast<time_t>(times[0].tv_sec), static_cast<suseconds_t>(times[0].tv_usec)}, {static_cast<time_t>(times[1].tv_sec), static_cast<suseconds_t>(times[1].tv_usec)}};
    return ::utimes(p.c_str(), values);
}
static int MapFlags(int sceFlags) {
    int f = 0;
    const int acc = sceFlags & SCE_KERNEL_O_ACCMODE;
    if (acc == SCE_KERNEL_O_RDONLY) f |= O_RDONLY;
    else if (acc == SCE_KERNEL_O_WRONLY) f |= O_WRONLY;
    else if (acc == SCE_KERNEL_O_RDWR) f |= O_RDWR;
    else return -1;
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= O_APPEND;
    if (sceFlags & SCE_KERNEL_O_CREAT) f |= O_CREAT;
    if (sceFlags & SCE_KERNEL_O_TRUNC) f |= O_TRUNC;
    if (sceFlags & SCE_KERNEL_O_EXCL) f |= O_EXCL;
    if (sceFlags & SCE_KERNEL_O_SYNC) f |= O_SYNC;
    if (sceFlags & SCE_KERNEL_O_NONBLOCK) f |= O_NONBLOCK;
    if (sceFlags & SCE_KERNEL_O_DIRECTORY) f |= O_DIRECTORY;
    return f | O_CLOEXEC;
}
#endif

namespace {

constexpr int kErrorInvalid = 22;
constexpr int kErrorFault = 14;

// ANYPS5_TRACE_FILES=1 logs every guest path lookup with its host path and outcome.
bool traceFiles() {
    static const bool enabled = [] {
        const char* value = std::getenv("ANYPS5_TRACE_FILES");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

// ANYPS5_TRACE_FILES=2 also logs every descriptor read, seek and close (descriptor, offset, size,
// result and host thread), to find descriptors whose file position several threads share.
bool traceDescriptors() {
    static const bool enabled = [] {
        const char* value = std::getenv("ANYPS5_TRACE_FILES");
        return value != nullptr && value[0] >= '2' && value[0] <= '9';
    }();
    return enabled;
}

std::int64_t traceDescriptor(const char* operation, int d, std::int64_t offset, std::uint64_t bytes, std::int64_t result) {
    if (traceDescriptors()) {
        const int error = errno;
        std::fprintf(stderr, "[AnyPS5 fd] %s fd=%d offset=%lld bytes=%llu = %lld thread=%ld\n", operation, d, static_cast<long long>(offset), static_cast<unsigned long long>(bytes), static_cast<long long>(result), static_cast<long>(gettid()));
        errno = error;
    }
    return result;
}

void trace(const char* operation, const char* path, const std::filesystem::path& host, int result) {
    if (!traceFiles()) return;
    const int error = errno;
    std::fprintf(stderr, "[AnyPS5 file] %s %s -> %s = %d%s%s\n", operation, path, host.string().c_str(), result,
        result < 0 ? " " : "", result < 0 ? std::strerror(error) : "");
    errno = error;
}

int openFile(const char* path, int flags, int mode) {
    if (path == nullptr) {
        errno = EFAULT;
        return -1;
    }
    const int nativeFlags = MapFlags(flags);
    if (nativeFlags < 0) {
        errno = EINVAL;
        return -1;
    }
    const auto host = ResolvePath_nid_no_patch(path);
    const int result = NativeOpen(host, nativeFlags, mode);
    trace("open", path, host, result);
    return result;
}

int sceResult(const int result) {
    return result < 0 ? FileErrors::Sce(errno) : result;
}

std::int64_t sceResult64(const std::int64_t result) {
    return result < 0 ? FileErrors::Sce(errno) : result;
}

int posixResult(const int result) {
    return result < 0 ? FileErrors::Posix(errno) : result;
}

std::int64_t posixResult64(const std::int64_t result) {
    return result < 0 ? FileErrors::Posix(errno) : result;
}

int statPath(const char* path, FileStat* sb) {
    if (path == nullptr || sb == nullptr) {
        errno = EFAULT;
        return -1;
    }
    const auto host = ResolvePath_nid_no_patch(path);
    const int error = File::FillFileStat(host, sb);
    if (error != 0) {
        errno = error;
        trace("stat", path, host, -1);
        return -1;
    }
    trace("stat", path, host, 0);
    return 0;
}

int statDescriptor(int d, FileStat* sb) {
    if (sb == nullptr) {
        errno = EFAULT;
        return -1;
    }
    const int error = File::FillFileStatFromDescriptor(d, sb);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

int renamePath(const char* from, const char* to) {
    if (from == nullptr || to == nullptr) {
        errno = EFAULT;
        return -1;
    }
    return NativeRename(ResolvePath_nid_no_patch(from), ResolvePath_nid_no_patch(to));
}

}

extern "C" {

int APS5_VABI sceKernelOpen(const char* path, int flags, std::uint16_t mode) {
    return sceResult(openFile(path, flags, mode));
}

int APS5_VABI sceKernelClose(int d) {
    return sceResult(static_cast<int>(traceDescriptor("close", d, -1, 0, NativeClose(d))));
}

std::int64_t APS5_VABI sceKernelRead(int d, void* buf, std::size_t nbytes) {
    if (buf == nullptr && nbytes != 0) return FileErrors::SceBsd(kErrorFault);
    return sceResult64(traceDescriptor("read", d, -1, nbytes, NativeRead(d, buf, nbytes)));
}

std::int64_t APS5_VABI sceKernelWrite(int d, const void* buf, std::size_t nbytes) {
    if (buf == nullptr && nbytes != 0) return FileErrors::SceBsd(kErrorFault);
    return sceResult64(NativeWrite(d, buf, nbytes));
}

std::int64_t APS5_VABI sceKernelLseek(int d, std::int64_t offset, int whence) {
    if (whence < 0 || whence > 2) return FileErrors::SceBsd(kErrorInvalid);
    return sceResult64(traceDescriptor(whence == 0 ? "seek-set" : whence == 1 ? "seek-cur" : "seek-end", d, offset, 0, NativeLseek(d, offset, whence)));
}

std::int64_t APS5_VABI sceKernelPread(int d, void* buf, std::size_t nbytes, std::int64_t offset) {
    if (buf == nullptr && nbytes != 0) return FileErrors::SceBsd(kErrorFault);
    return sceResult64(traceDescriptor("pread", d, offset, nbytes, NativePread(d, buf, nbytes, offset)));
}

std::int64_t APS5_VABI sceKernelPwrite(int d, const void* buf, std::size_t nbytes, std::int64_t offset) {
    if (buf == nullptr && nbytes != 0) return FileErrors::SceBsd(kErrorFault);
    return sceResult64(NativePwrite(d, buf, nbytes, offset));
}

int APS5_VABI sceKernelStat(const char* path, FileStat* sb) {
    return sceResult(statPath(path, sb));
}

int APS5_VABI sceKernelFstat(int d, FileStat* sb) {
    return sceResult(statDescriptor(d, sb));
}

int APS5_VABI sceKernelCheckReachability(const char* path) {
    FileStat sb{};
    return sceResult(statPath(path, &sb));
}

int APS5_VABI sceKernelUnlink(const char* path) {
    if (path == nullptr) return FileErrors::SceBsd(kErrorFault);
    return sceResult(NativeUnlink(ResolvePath_nid_no_patch(path)));
}

int APS5_VABI sceKernelMkdir(const char* path, uint16_t mode) {
    if (path == nullptr) return FileErrors::SceBsd(kErrorFault);
    return sceResult(NativeMkdir(ResolvePath_nid_no_patch(path), mode));
}

int APS5_VABI sceKernelRmdir(const char* path) {
    if (path == nullptr) return FileErrors::SceBsd(kErrorFault);
    return sceResult(NativeRmdir(ResolvePath_nid_no_patch(path)));
}

int APS5_VABI sceKernelRename(const char* from, const char* to) {
    return sceResult(renamePath(from, to));
}

int APS5_VABI sceKernelFsync(int fd) {
    return sceResult(NativeFsync(fd));
}

int APS5_VABI sceKernelFtruncate(int fd, std::int64_t length) {
    return sceResult(NativeFtruncate(fd, length));
}

int APS5_VABI sceKernelChmod(const char* path, uint16_t mode) {
    if (path == nullptr) return FileErrors::SceBsd(kErrorFault);
    return sceResult(NativeChmod(ResolvePath_nid_no_patch(path), mode));
}

int APS5_VABI sceKernelFchmod(int fd, uint16_t mode) {
    return sceResult(NativeFchmod(fd, mode));
}

int APS5_VABI sceKernelUtimes(const char* path, const KernelTimeval* times) {
    if (path == nullptr) return FileErrors::SceBsd(kErrorFault);
    return sceResult(NativeUtimes(ResolvePath_nid_no_patch(path), times));
}

int APS5_VABI open_nid_postfix(const char* path, int flags, int mode) {
    return posixResult(openFile(path, flags, mode));
}

int APS5_VABI close_nid_postfix(int d) {
    if (d >= GuestSockets::FirstDescriptor) return GuestSockets::Close(d);
    return posixResult(static_cast<int>(traceDescriptor("close", d, -1, 0, NativeClose(d))));
}

int64_t APS5_VABI read_nid_postfix(int d, void* buf, uint64_t nbytes) {
    return posixResult64(traceDescriptor("read", d, -1, nbytes, NativeRead(d, buf, nbytes)));
}

int64_t APS5_VABI write_nid_postfix(int d, const void* buf, uint64_t nbytes) {
    return posixResult64(NativeWrite(d, buf, nbytes));
}

int64_t APS5_VABI pread_nid_postfix(int d, void* buf, size_t nbytes, int64_t offset) {
    return posixResult64(traceDescriptor("pread", d, offset, nbytes, NativePread(d, buf, nbytes, offset)));
}

int64_t APS5_VABI pwrite_nid_disambig1_nid_postfix(int d, const void* buf, size_t nbytes, int64_t offset) {
    return posixResult64(NativePwrite(d, buf, nbytes, offset));
}

int64_t APS5_VABI lseek_nid_postfix(int d, int64_t offset, int whence) {
    if (whence < 0 || whence > 2) return FileErrors::PosixBsd(kErrorInvalid);
    return posixResult64(traceDescriptor(whence == 0 ? "seek-set" : whence == 1 ? "seek-cur" : "seek-end", d, offset, 0, NativeLseek(d, offset, whence)));
}

int APS5_VABI stat_nid_postfix(const char* path, FileStat* sb) {
    return posixResult(statPath(path, sb));
}

int64_t APS5_VABI fstat_nid_disambig1_nid_postfix(int d, FileStat* sb) {
    return posixResult(statDescriptor(d, sb));
}

int APS5_VABI ftruncate_nid_postfix(int d, int64_t length) {
    return posixResult(NativeFtruncate(d, length));
}

int APS5_VABI mkdir_nid_postfix(const char* path, uint16_t mode) {
    if (path == nullptr) return FileErrors::PosixBsd(kErrorFault);
    return posixResult(NativeMkdir(ResolvePath_nid_no_patch(path), mode));
}

int APS5_VABI rmdir_nid_postfix(const char* path) {
    if (path == nullptr) return FileErrors::PosixBsd(kErrorFault);
    return posixResult(NativeRmdir(ResolvePath_nid_no_patch(path)));
}

int APS5_VABI unlink_nid_postfix(const char* path) {
    if (path == nullptr) return FileErrors::PosixBsd(kErrorFault);
    return posixResult(NativeUnlink(ResolvePath_nid_no_patch(path)));
}

int APS5_VABI rename_nid_postfix(const char* from, const char* to) {
    return posixResult(renamePath(from, to));
}

int APS5_VABI chmod_nid_postfix(const char* path, int mode) {
    if (path == nullptr) return FileErrors::PosixBsd(kErrorFault);
    return posixResult(NativeChmod(ResolvePath_nid_no_patch(path), mode));
}

int APS5_VABI fchmod_nid_postfix(int d, int mode) {
    return posixResult(NativeFchmod(d, mode));
}

int APS5_VABI flock_nid_postfix(int d, int operation) {
    return posixResult(NativeFlock(d, operation));
}

int APS5_VABI utimes_nid_postfix(const char* path, const KernelTimeval* times) {
    if (path == nullptr) return FileErrors::PosixBsd(kErrorFault);
    return posixResult(NativeUtimes(ResolvePath_nid_no_patch(path), times));
}

int APS5_VABI futimes_nid_postfix(int d, const KernelTimeval* times) {
#ifdef _WIN32
    (void)d;
    (void)times;
    NotImplemented_nid_no_patch(__func__);
    return 0;
#else
    if (times == nullptr) return posixResult(::futimes(d, nullptr));
    const struct timeval values[2] = {{static_cast<time_t>(times[0].tv_sec), static_cast<suseconds_t>(times[0].tv_usec)}, {static_cast<time_t>(times[1].tv_sec), static_cast<suseconds_t>(times[1].tv_usec)}};
    return posixResult(::futimes(d, values));
#endif
}

}
