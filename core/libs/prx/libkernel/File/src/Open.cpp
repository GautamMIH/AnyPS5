#include "prx/libkernel/File/include/FileFlags.hpp"
#include "prx/libkernel/File/include/NativeStat.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/FilesystemError.hpp"
#include "prx/libc/include/GuestMemoryTracking.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include "prx/libkernel/File/include/File.hpp"
#include "prx/libkernel/File/include/SharedMemory.hpp"
#include "prx/libkernel/File/include/DirectoryDescriptor.hpp"
#include "prx/libkernel/File/include/DescriptorControl.hpp"
#include "prx/libkernel/File/include/FileLock.hpp"
#include "prx/libkernel/Socket/include/SocketRuntime.hpp"
#include "prx/libkernel/File/include/RandomDevice.hpp"
#include "prx/libkernel/File/include/HostStreams.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"
#include "SceTypes.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

// A system call reading or writing guest memory cannot fault into the page-tracking handler (the
// kernel returns EFAULT on a protected page), so tracked ranges are resolved first, as a CPU
// access would resolve them: GPU-resident surfaces are written back (and invalidated on writes).
static void resolveGuestAccess(const void* buffer, std::size_t bytes, bool writable) {
    if (buffer != nullptr && bytes != 0) GuestMemoryTracking::GuestMemoryTrackingResolve_nid_postfix(reinterpret_cast<std::uint64_t>(buffer), bytes, writable);
}

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
static int NativeOpen(const std::filesystem::path& p, int nativeFlags, std::uint16_t mode) {
    const int access = nativeFlags & (_O_RDONLY | _O_WRONLY | _O_RDWR);
    const DWORD desired = access == _O_RDONLY ? GENERIC_READ : access == _O_WRONLY ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE;
    DWORD disposition = OPEN_EXISTING;
    if (nativeFlags & _O_CREAT) disposition = (nativeFlags & _O_EXCL) ? CREATE_NEW : (nativeFlags & _O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else if (nativeFlags & _O_TRUNC) disposition = TRUNCATE_EXISTING;
    const DWORD attributes = (nativeFlags & _O_CREAT) && !(mode & _S_IWRITE) ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const auto handle = CreateFileW(p.c_str(), desired, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security, disposition, attributes, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = WindowsOpenError_nid_no_patch(GetLastError());
        return -1;
    }
    int descriptor = ::_open_osfhandle(reinterpret_cast<std::intptr_t>(handle), nativeFlags & (_O_APPEND | _O_RDONLY | _O_BINARY));
    if (descriptor < 0) CloseHandle(handle);
    // Guest files never use descriptors 0-2 (see the Linux NativeOpen).
    while (descriptor >= 0 && descriptor < 3) {
        const int moved = ::_dup(descriptor);
        const int error = errno;
        ::_close(descriptor);
        errno = error;
        descriptor = moved;
    }
    return descriptor;
}
static std::int64_t NativeLseek(int fd, std::int64_t offset, int whence) {
    const auto positioned = File::LockPositionedTransfer(fd);
    const auto result = File::SeekSharedMemory_nid_no_patch(fd, offset, whence);
    if (result != -2) return result;
    const auto position = ::_lseeki64(fd, offset, whence);
    if (position < 0 || GetFileType(reinterpret_cast<HANDLE>(::_get_osfhandle(fd))) != FILE_TYPE_PIPE) return position;
    errno = ESPIPE;
    return -1;
}
static int NativeRead(int fd, void* buf, std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
        throw std::runtime_error("sceKernelRead: nbytes exceeds platform limit");
    }
    resolveGuestAccess(buf, n, true);
    const auto positioned = File::LockPositionedTransfer(fd);
    const int result = ::_read(fd, buf, static_cast<unsigned int>(n));
    if (result < 0 && _doserrno == ERROR_NO_DATA) errno = EAGAIN;
    if (result == 0 && File::DirectoryDescriptorPath(fd)) {
        errno = EISDIR;
        return -1;
    }
    return result;
}
static int NativeWrite(int fd, const void* buf, std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
        throw std::runtime_error("sceKernelWrite: nbytes exceeds platform limit");
    }
    resolveGuestAccess(buf, n, false);
    const auto positioned = File::LockPositionedTransfer(fd);
    const auto shared = File::WriteSharedMemory_nid_no_patch(fd, buf, n);
    if (shared != -2) return static_cast<int>(shared);
    const int result = ::_write(fd, buf, static_cast<unsigned int>(n));
    if (result < 0 && errno == ENOSPC && _doserrno == 0 && ::GetFileType(reinterpret_cast<HANDLE>(::_get_osfhandle(fd))) == FILE_TYPE_PIPE) errno = EAGAIN;
    return result;
}
extern "C" _invalid_parameter_handler _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler);
static void IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {}
// Closing 0-2 leaves the host's standard streams open (guest files never use them).
static int NativeClose(int fd) {
    if (fd >= 0 && fd < 3) return 0;
    const auto previous = _set_thread_local_invalid_parameter_handler(IgnoreInvalidParameter);
    const int result = ::_close(fd);
    _set_thread_local_invalid_parameter_handler(previous);
    return result;
}
static int NativeUnlink(const std::filesystem::path& p) {
    const auto path = p.wstring();
    struct _stat64 status{};
    const bool unlocked = ::_wstat64(path.c_str(), &status) == 0 && (status.st_mode & _S_IFMT) == _S_IFREG &&
        !(status.st_mode & _S_IWRITE) && ::_wchmod(path.c_str(), _S_IREAD | _S_IWRITE) == 0;
    if (::_wunlink(path.c_str()) == 0) return 0;
    const int error = errno;
    if (unlocked) ::_wchmod(path.c_str(), _S_IREAD);
    errno = error;
    return -1;
}
static int MapFlags(int sceFlags) {
    int f = 0;
    const int acc = sceFlags & SCE_KERNEL_O_ACCMODE;
    if (acc == SCE_KERNEL_O_RDONLY) f |= _O_RDONLY;
    else if (acc == SCE_KERNEL_O_WRONLY) f |= _O_WRONLY;
    else if (acc == SCE_KERNEL_O_RDWR) f |= _O_RDWR;
    else throw std::invalid_argument("sceKernelOpen: invalid access mode");
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= _O_APPEND;
    if (sceFlags & SCE_KERNEL_O_CREAT) f |= _O_CREAT;
    if (sceFlags & SCE_KERNEL_O_TRUNC) f |= _O_TRUNC;
    if (sceFlags & SCE_KERNEL_O_EXCL) f |= _O_EXCL;
    f |= _O_BINARY;
    return f;
}
#else
#include <fcntl.h>
#include <unistd.h>
// Guest files never use descriptors 0-2: games treat 0 as "no file" and close it (Unity closes
// descriptor 0 at start), so the host's standard streams stay open and a file never lands there.
static int NativeOpen(const std::filesystem::path& p, int nativeFlags, std::uint16_t mode) {
    const int fd = ::open(p.c_str(), nativeFlags, static_cast<mode_t>(mode));
    if (fd < 0 || fd >= 3) return fd;
    const int moved = ::fcntl(fd, (nativeFlags & O_CLOEXEC) != 0 ? F_DUPFD_CLOEXEC : F_DUPFD, 3);
    const int error = errno;
    ::close(fd);
    errno = error;
    return moved;
}
static std::int64_t NativeLseek(int fd, std::int64_t offset, int whence) {
    const auto result = File::SeekSharedMemory_nid_no_patch(fd, offset, whence);
    return result == -2 ? ::lseek(fd, static_cast<off_t>(offset), whence) : result;
}
static std::int64_t NativeRead(int fd, void* buf, std::size_t n) {
    resolveGuestAccess(buf, n, true);
    return ::read(fd, buf, n);
}
static std::int64_t NativeWrite(int fd, const void* buf, std::size_t n) {
    resolveGuestAccess(buf, n, false);
    const auto result = File::WriteSharedMemory_nid_no_patch(fd, buf, n);
    return result == -2 ? ::write(fd, buf, n) : result;
}
// Closing 0-2 leaves the host's standard streams open (guest files never use them).
static int NativeClose(int fd) { return fd >= 0 && fd < 3 ? 0 : ::close(fd); }
static int NativeUnlink(const std::filesystem::path& p) {
    return ::unlink(p.c_str());
}
static int MapFlags(int sceFlags) {
    int f = 0;
    const int acc = sceFlags & SCE_KERNEL_O_ACCMODE;
    if (acc == SCE_KERNEL_O_RDONLY) f |= O_RDONLY;
    else if (acc == SCE_KERNEL_O_WRONLY) f |= O_WRONLY;
    else if (acc == SCE_KERNEL_O_RDWR) f |= O_RDWR;
    else throw std::invalid_argument("sceKernelOpen: invalid access mode");
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= O_APPEND;
    if (sceFlags & SCE_KERNEL_O_CREAT) f |= O_CREAT;
    if (sceFlags & SCE_KERNEL_O_TRUNC) f |= O_TRUNC;
    if (sceFlags & SCE_KERNEL_O_EXCL) f |= O_EXCL;
    if (sceFlags & SCE_KERNEL_O_SYNC) f |= O_SYNC;
    if (sceFlags & SCE_KERNEL_O_NONBLOCK) f |= O_NONBLOCK;
    if (sceFlags & SCE_KERNEL_O_DIRECTORY) f |= O_DIRECTORY;
    // Host-side only: guest descriptors are not inherited by host helper processes (the guest's own
    // close-on-exec state is tracked by DescriptorControl).
    return f | O_CLOEXEC;
}
#endif

extern "C" int* APS5_VABI __error_nid_postfix();
extern "C" int APS5_VABI fcntl_nid_postfix(int descriptor, int command, ...);
static constexpr int GuestSetFlags = 4;

static int SceErrorFromErrno(int error) {
    constexpr int GuestEio = 5;
    const int guest = error > 0 && error <= 34 ? error : GuestEio;
    return static_cast<int>(0x80020000u | static_cast<unsigned>(guest));
}

namespace {

// ANYPS5_TRACE_FILES=1 logs every guest path open with its host path and outcome.
bool traceFiles() {
    static const bool enabled = [] {
        const char* value = std::getenv("ANYPS5_TRACE_FILES");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

// ANYPS5_TRACE_FILES=2 also logs every descriptor read, seek and close (descriptor, offset, size and
// result), to find descriptors whose file position several threads share.
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
        std::fprintf(stderr, "[AnyPS5 fd] %s fd=%d offset=%lld bytes=%llu = %lld\n", operation, d, static_cast<long long>(offset), static_cast<unsigned long long>(bytes), static_cast<long long>(result));
        errno = error;
    }
    return result;
}

int traceOpen(const std::filesystem::path& host, int result) {
    if (traceFiles()) {
        const int error = errno;
        std::fprintf(stderr, "[AnyPS5 file] open %s = %d%s%s\n", host.string().c_str(), result, result < 0 ? " " : "", result < 0 ? std::strerror(error) : "");
        errno = error;
    }
    return result;
}

}

static int SceIoErrorFromErrno(int error) {
    int guest = SceErrorFromErrno(error) & 0xffff;
    if (error == EAGAIN) guest = 35;
#ifdef EDEADLK
    else if (error == EDEADLK) guest = 11;
#endif
#ifdef EDQUOT
    else if (error == EDQUOT) guest = 69;
#endif
#ifdef EOVERFLOW
    else if (error == EOVERFLOW) guest = 84;
#endif
    return static_cast<int>(0x80020000u | static_cast<unsigned>(guest));
}

static int OpenNative(const std::filesystem::path& native, int flags, std::uint16_t mode) {
#ifdef _WIN32
    if ((flags & (SCE_KERNEL_O_DIRECTORY | SCE_KERNEL_O_CREAT)) == SCE_KERNEL_O_DIRECTORY) {
        std::error_code error;
        const auto status = std::filesystem::status(native, error);
        if (std::filesystem::exists(status) && !std::filesystem::is_directory(status)) return SceErrorFromErrno(ENOTDIR);
    }
#endif
    if ((flags & SCE_KERNEL_O_NOFOLLOW) != 0 && (flags & (SCE_KERNEL_O_CREAT | SCE_KERNEL_O_EXCL)) != (SCE_KERNEL_O_CREAT | SCE_KERNEL_O_EXCL)) {
        constexpr int GuestEmlink = 31;
        std::error_code error;
        if (std::filesystem::is_symlink(native, error)) return SceErrorFromErrno(GuestEmlink);
    }
    int fd = traceOpen(native, NativeOpen(native, MapFlags(flags), mode));
#ifdef _WIN32
    if (fd < 0 && (errno != ENOENT || !native.has_filename())) {
        auto directory = native;
        while (!directory.has_filename() && directory.has_relative_path()) directory = directory.parent_path();
        std::error_code error;
        if (std::filesystem::is_directory(directory, error)) {
            if ((flags & (SCE_KERNEL_O_CREAT | SCE_KERNEL_O_EXCL)) == (SCE_KERNEL_O_CREAT | SCE_KERNEL_O_EXCL)) errno = EEXIST;
            else if ((flags & SCE_KERNEL_O_ACCMODE) != SCE_KERNEL_O_RDONLY || (flags & (SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC))) errno = EISDIR;
            else fd = File::OpenDirectoryDescriptor(directory);
        }
    }
#endif
    if (fd < 0) {
        return SceErrorFromErrno(errno);
    }
    if ((flags & SCE_KERNEL_O_ACCMODE) != SCE_KERNEL_O_RDONLY || (flags & (SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC)))
        RecordWrittenPath_nid_no_patch(native);
    File::RememberDescriptor(fd, flags);
    return fd;
}

extern "C" {

int APS5_VABI sceKernelOpen(const char* path, int flags, std::uint16_t mode) {
    if (path != nullptr && *path == '\0') return SCE_KERNEL_ERROR_ENOENT;
    APS5_LOG_OUT("path=%s flags=0x%X nativeFlags=0x%X mode=0%o", path, flags, MapFlags(flags), mode);
    if (path != nullptr && File::IsRandomDevicePath(path)) {
        if ((flags & SCE_KERNEL_O_ACCMODE) != SCE_KERNEL_O_RDONLY) throw std::runtime_error(std::string(__func__) + ": writing to " + path + " is not implemented");
        const int fd = File::OpenRandomDevice();
        if (fd < 0) return SceErrorFromErrno(errno);
        File::RememberDescriptor(fd, flags);
        return fd;
    }
    std::filesystem::path native;
    if (const int error = ResolvePathError_nid_no_patch(path, native)) return SceKernelError(error);
    if ((flags & (SCE_KERNEL_O_ACCMODE | SCE_KERNEL_O_CREAT)) == SCE_KERNEL_O_RDONLY) native = File::PathOrRelinkedModule(native);
    return OpenNative(native, flags, mode);
}

int OpenInDirectory_nid_no_patch(int directory, const char* path, int flags, std::uint16_t mode) {
    int error = 0;
    const char* base = GetDirectoryDescriptorPath_nid_no_patch(directory, &error);
    if (base == nullptr) return SceErrorFromErrno(error);
    return OpenNative(std::filesystem::path(reinterpret_cast<const char8_t*>(base)) / std::filesystem::path(reinterpret_cast<const char8_t*>(path)), flags, mode);
}

int APS5_VABI sceKernelClose(int d) {
    if (File::IsHostStream(d)) return SCE_KERNEL_ERROR_EBADF;
    if (GuestSockets::IsOpen(d)) return GuestSockets::Close(d) == 0 ? 0 : SceKernelError(*__error_nid_postfix());
    traceDescriptor("close", d, -1, 0, 0);
    // Guest files never use 0-2; closing them leaves the host's standard streams open.
    if (d >= 0 && d < 3) return 0;
    EqueueDescriptorClosed(d);
    File::ForgetRandomDevice(d);
    File::ForgetDescriptor(d);
    ForgetJitSharedMemory(d);
#ifdef _WIN32
    File::ForgetDirectoryDescriptor(d);
    File::ForgetFileLock(d);
#endif
    if (NativeClose(d) != 0) {
        if (errno == EBADF) return SCE_KERNEL_ERROR_EBADF;
        throw std::runtime_error(std::string(__func__) + ": close failed, fd=" + std::to_string(d) + ", errno=" + std::to_string(errno));
    }
    return 0;
}

std::int64_t APS5_VABI sceKernelRead(int d, void* buf, std::size_t nbytes) {
    if (GuestSockets::IsOpen(d)) {
        const auto n = GuestSockets::Read(d, buf, nbytes);
        return n < 0 ? SceKernelError(*__error_nid_postfix()) : n;
    }
    if (buf == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": buf is null");
    }
    if (File::IsRandomDevice(d)) {
        resolveGuestAccess(buf, nbytes, true);
        if (File::ReadRandomDevice(d, buf, nbytes)) return static_cast<std::int64_t>(nbytes);
    }
    auto n = traceDescriptor("read", d, -1, nbytes, NativeRead(d, buf, nbytes));
    if (n < 0) return SceIoErrorFromErrno(errno);
    return static_cast<std::int64_t>(n);
}

std::int64_t APS5_VABI sceKernelWrite(int d, const void* buf, std::size_t nbytes) {
    if (GuestSockets::IsOpen(d)) {
        const auto n = GuestSockets::Write(d, buf, nbytes);
        return n < 0 ? SceKernelError(*__error_nid_postfix()) : n;
    }
    if (buf == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": buf is null");
    }
    auto n = NativeWrite(d, buf, nbytes);
    if (n < 0) return SceIoErrorFromErrno(errno);
    return static_cast<std::int64_t>(n);
}

std::int64_t APS5_VABI sceKernelLseek(int d, std::int64_t offset, int whence) {
    if (GuestSockets::IsOpen(d)) return SCE_KERNEL_ERROR_ESPIPE;
    if (whence < 0 || whence > 2) {
        throw std::invalid_argument(std::string(__func__) + ": invalid whence=" + std::to_string(whence));
    }
#ifdef _WIN32
    if (const auto directory = File::SeekDirectoryDescriptor(d, offset, whence)) {
        if (*directory < 0) return SceIoErrorFromErrno(errno);
        return *directory;
    }
#endif
    std::int64_t result = traceDescriptor(whence == 0 ? "seek-set" : whence == 1 ? "seek-cur" : "seek-end", d, offset, 0, NativeLseek(d, offset, whence));
    if (result < 0) return SceIoErrorFromErrno(errno);
    return result;
}

int APS5_VABI sceKernelStat(const char* path, FileStat* sb) {
    if (path == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": path is null");
    }
    if (*path == '\0') return SceErrorFromErrno(2);
    std::filesystem::path native;
    if (const int error = ResolvePathError_nid_no_patch(path, native)) return SceKernelError(error);
    native = File::PathOrRelinkedModule(native);
    std::error_code error;
    constexpr int GuestEnotdir = 20;
    constexpr int GuestEfault = 14;
    const auto status = std::filesystem::status(native, error);
    if (error == std::errc::not_a_directory) return SceErrorFromErrno(GuestEnotdir);
    if (!std::filesystem::exists(status)) {
        return SceErrorFromErrno(2);
    }
    const std::string_view guestPath(path);
    if (!guestPath.empty() && guestPath.back() == '/' && !std::filesystem::is_directory(status)) return SceErrorFromErrno(GuestEnotdir);
    if (sb == nullptr) return SceErrorFromErrno(GuestEfault);
    resolveGuestAccess(sb, sizeof(*sb), true);
    if (const int failure = File::FillFileStat(native, sb)) return SceErrorFromErrno(failure);
    return 0;
}

int APS5_VABI sceKernelUnlink(const char* path) {
    if (path == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": path is null");
    }
    if (*path == '\0') return SceErrorFromErrno(2);
    std::filesystem::path native;
    if (const int error = ResolvePathError_nid_no_patch(path, native)) return SceKernelError(error);
    constexpr int GuestEperm = 1;
    std::error_code error;
    if (std::filesystem::is_directory(std::filesystem::symlink_status(native, error))) return SceErrorFromErrno(GuestEperm);
    if (NativeUnlink(native) != 0) {
        return SceErrorFromErrno(errno);
    }
    RecordWrittenPath_nid_no_patch(native);
    return 0;
}

int APS5_VABI sceKernelFcntl(int d, int command, ...) {
    const auto type = File::FcntlArgumentOf(command);
    std::intptr_t argument = 0;
    if (type != File::FcntlArgument::None) {
#ifdef _WIN32
        __builtin_sysv_va_list arguments;
        __builtin_sysv_va_start(arguments, command);
        argument = type == File::FcntlArgument::Int ? __builtin_va_arg(arguments, int)
                                                    : reinterpret_cast<std::intptr_t>(__builtin_va_arg(arguments, void*));
        __builtin_sysv_va_end(arguments);
#else
        std::va_list arguments;
        va_start(arguments, command);
        argument = type == File::FcntlArgument::Int ? va_arg(arguments, int)
                                                    : reinterpret_cast<std::intptr_t>(va_arg(arguments, void*));
        va_end(arguments);
#endif
    }
    int result;
    if (GuestSockets::IsOpen(d)) {
        if (command == GuestSetFlags) result = fcntl_nid_postfix(d, command, static_cast<int>(argument));
        else result = fcntl_nid_postfix(d, command);
    } else {
        result = File::Fcntl(d, command, argument);
    }
    return result < 0 ? SceKernelError(*__error_nid_postfix()) : result;
}

}
