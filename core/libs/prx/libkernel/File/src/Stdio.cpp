#include <cerrno>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/File/include/FileErrors.hpp"

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

constexpr std::size_t kBsdDirentHeader = 8;
constexpr std::size_t kHostBufferSize = 32768;
constexpr int kErrorInvalid = 22;

std::size_t bsdRecordLength(std::size_t nameLength) {
    return (kBsdDirentHeader + nameLength + 1 + 3) & ~static_cast<std::size_t>(3);
}

int readDirectory(int fd, char* buf, int nbytes) {
#if defined(__linux__)
    if (buf == nullptr || nbytes <= 0) return FileErrors::SceBsd(kErrorInvalid);
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
            if (written == 0) return FileErrors::SceBsd(kErrorInvalid);
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
#if defined(__linux__)
    if (basep) {
        const off_t position = ::lseek(fd, 0, SEEK_CUR);
        if (position < 0) return FileErrors::Sce(errno);
        *basep = position;
    }
#endif
    return readDirectory(fd, buf, nbytes);
}

}
