#include <cstdint>
#include <cstddef>
#include <ctime>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

#include "prx/libc/include/General.hpp"

namespace {

// The guest's struct tm holds only the nine standard int fields (tm_sec through tm_isdst); the
// host's adds tm_gmtoff and tm_zone after them. Guest structures are converted field by field so host
// time functions never read or write past the guest's structure.
constexpr std::size_t kGuestTmFields = 9;
static_assert(offsetof(std::tm, tm_isdst) == (kGuestTmFields - 1) * sizeof(int));

std::tm toHost(const std::tm* guest) {
    std::tm host{};
    std::memcpy(&host, guest, kGuestTmFields * sizeof(int));
#ifndef _WIN32
    // %Z and %z describe the host's time zone.
    tzset();
    host.tm_zone = tzname[host.tm_isdst > 0 ? 1 : 0];
    host.tm_gmtoff = -timezone + (host.tm_isdst > 0 ? 3600 : 0);
#endif
    return host;
}

void toGuest(const std::tm& host, std::tm* guest) {
    std::memcpy(guest, &host, kGuestTmFields * sizeof(int));
}

}

extern "C" {

std::tm* APS5_VABI localtime_s_nid_postfix(const int64_t* timer, std::tm* result);
std::tm* APS5_VABI gmtime_s_nid_postfix(const int64_t* timer, std::tm* result);

int64_t APS5_VABI libc_time_nid_postfix(int64_t* timer) {
    std::time_t t = std::time(nullptr);
    if (timer != nullptr) *timer = static_cast<int64_t>(t);
    return static_cast<int64_t>(t);
}

int64_t APS5_VABI time_nid_postfix(int64_t* timer) {
    return libc_time_nid_postfix(timer);
}

int64_t APS5_VABI _Xtime_get_ticks_nid_postfix() {
#ifdef _WIN32
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t t = ((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000ULL;
    return static_cast<int64_t>(t / 10);
#else
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
#endif
}

double APS5_VABI libc_difftime_nid_postfix(int64_t time1, int64_t time0) {
    return std::difftime(static_cast<std::time_t>(time1), static_cast<std::time_t>(time0));
}

double APS5_VABI difftime_nid_postfix(int64_t time1, int64_t time0) {
    return std::difftime(static_cast<std::time_t>(time1), static_cast<std::time_t>(time0));
}

std::tm* APS5_VABI libc_gmtime_nid_postfix(const int64_t* timer) {
    static thread_local std::tm result;
    return gmtime_s_nid_postfix(timer, &result);
}

std::tm* APS5_VABI libc_localtime_nid_postfix(const int64_t* timer) {
    static thread_local std::tm result;
    return localtime_s_nid_postfix(timer, &result);
}

std::tm* APS5_VABI localtime_nid_postfix(const int64_t* timer) {
    return libc_localtime_nid_postfix(timer);
}

std::tm* APS5_VABI localtime_s_nid_postfix(const int64_t* timer, std::tm* result) {
    if (timer == nullptr || result == nullptr) return nullptr;
    const std::time_t t = static_cast<std::time_t>(*timer);
    std::tm converted{};
#ifdef _WIN32
    if (localtime_s(&converted, &t) != 0) return nullptr;
#else
    if (localtime_r(&t, &converted) == nullptr) return nullptr;
#endif
    toGuest(converted, result);
    return result;
}

std::tm* APS5_VABI gmtime_s_nid_postfix(const int64_t* timer, std::tm* result) {
    if (timer == nullptr || result == nullptr) return nullptr;
    const std::time_t t = static_cast<std::time_t>(*timer);
    std::tm converted{};
#ifdef _WIN32
    if (gmtime_s(&converted, &t) != 0) return nullptr;
#else
    if (gmtime_r(&t, &converted) == nullptr) return nullptr;
#endif
    toGuest(converted, result);
    return result;
}

int64_t APS5_VABI libc_mktime_nid_postfix(std::tm* timeptr) {
    // mktime normalizes the fields it is given; only the guest's fields are written back.
    std::tm host = toHost(timeptr);
    const auto result = static_cast<int64_t>(std::mktime(&host));
    toGuest(host, timeptr);
    return result;
}

int64_t APS5_VABI mktime_nid_postfix(std::tm* timeptr) {
    return libc_mktime_nid_postfix(timeptr);
}

size_t APS5_VABI libc_strftime_nid_postfix(char* str, size_t count, const char* format, const std::tm* timeptr) {
    const std::tm host = toHost(timeptr);
    return std::strftime(str, count, format, &host);
}

char* APS5_VABI asctime_nid_postfix(const std::tm* timeptr) {
    const std::tm host = toHost(timeptr);
    return std::asctime(&host);
}

size_t APS5_VABI strftime_nid_postfix(char* str, size_t count, const char* format, const std::tm* timeptr) {
    return libc_strftime_nid_postfix(str, count, format, timeptr);
}

}
