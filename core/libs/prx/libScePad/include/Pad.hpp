#ifndef CORE_LIBS_PRX_LIBSCEPAD_PAD_HPP
#define CORE_LIBS_PRX_LIBSCEPAD_PAD_HPP

constexpr int PAD_OK = 0;
// Error codes as in shadPS4's pad library.
constexpr int PAD_ERROR_INVALID_ARG = static_cast<int>(0x80920001);
constexpr int PAD_ERROR_INVALID_HANDLE = static_cast<int>(0x80920003);
constexpr int PAD_ERROR_NOT_INITIALIZED = static_cast<int>(0x80920005);
constexpr int PAD_ERROR_NO_HANDLE = static_cast<int>(0x80920008);

constexpr int PAD_PORT_TYPE_STANDARD = 0;
constexpr int PAD_PORT_TYPE_SPECIAL = 2;
constexpr int PAD_PORT_TYPE_REMOTE = 16;
constexpr int PAD_USER_ID_SYSTEM = 0xff;
constexpr int PAD_HANDLE = 1;

constexpr int PAD_CONNECTION_TYPE_LOCAL = 0;
constexpr int PAD_DEVICE_CLASS_STANDARD = 0;

#endif
