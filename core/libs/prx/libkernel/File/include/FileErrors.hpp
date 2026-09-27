#ifndef CORE_LIBS_PRX_LIBKERNEL_FILE_FILEERRORS_HPP
#define CORE_LIBS_PRX_LIBKERNEL_FILE_FILEERRORS_HPP

#include <cerrno>

namespace FileErrors {

inline constexpr int kBsdInvalid = 22;
inline constexpr int kBsdIo = 5;

inline int ToBsd(const int nativeError) {
    switch (nativeError) {
    case EAGAIN: return 35;
    case EDEADLK: return 11;
    case ENAMETOOLONG: return 63;
    case ENOLCK: return 77;
    case ENOSYS: return 78;
    case ENOTEMPTY: return 66;
    case ELOOP: return 62;
    case EOVERFLOW: return 84;
    case EOPNOTSUPP: return 45;
    case ETIMEDOUT: return 60;
    case ECONNREFUSED: return 61;
    case ECONNRESET: return 54;
    case EADDRINUSE: return 48;
    case EINPROGRESS: return 36;
    case EALREADY: return 37;
    case EHOSTUNREACH: return 65;
    case ENETUNREACH: return 51;
    case ENOTCONN: return 57;
    case EISCONN: return 56;
    case ECONNABORTED: return 53;
    case ENOTSOCK: return 38;
    case EAFNOSUPPORT: return 47;
    default: return nativeError > 0 && nativeError < 35 ? nativeError : kBsdIo;
    }
}

inline int Sce(const int nativeError) {
    return static_cast<int>(0x80020000u | static_cast<unsigned>(ToBsd(nativeError)));
}

inline int SceBsd(const int bsdError) {
    return static_cast<int>(0x80020000u | static_cast<unsigned>(bsdError));
}

inline int Posix(const int nativeError) {
    errno = ToBsd(nativeError);
    return -1;
}

inline int PosixBsd(const int bsdError) {
    errno = bsdError;
    return -1;
}

}

#endif
