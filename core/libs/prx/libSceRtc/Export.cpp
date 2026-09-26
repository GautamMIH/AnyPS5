#include <chrono>
#include <cstdint>
#include <cstddef>
#include <ctime>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr int RTC_ERROR_INVALID_POINTER = static_cast<int>(0x80B50002);
constexpr int RTC_ERROR_INVALID_VALUE = static_cast<int>(0x80B50003);
constexpr int RTC_ERROR_INVALID_YEAR = static_cast<int>(0x80B50008);
constexpr int RTC_ERROR_INVALID_MONTH = static_cast<int>(0x80B50009);
constexpr int RTC_ERROR_INVALID_DAY = static_cast<int>(0x80B5000A);
constexpr int RTC_ERROR_INVALID_HOUR = static_cast<int>(0x80B5000B);
constexpr int RTC_ERROR_INVALID_MINUTE = static_cast<int>(0x80B5000C);
constexpr int RTC_ERROR_INVALID_SECOND = static_cast<int>(0x80B5000D);
constexpr int RTC_ERROR_INVALID_MICROSECOND = static_cast<int>(0x80B5000E);

constexpr std::int64_t kMicrosecondsPerSecond = 1000000;
constexpr std::int64_t kSecondsPerDay = 86400;
constexpr std::int64_t kUnixEpochDays = 719162;
constexpr std::int64_t kWin32EpochDays = 584388;
constexpr std::uint64_t kUnixEpochTick = static_cast<std::uint64_t>(kUnixEpochDays) * kSecondsPerDay * kMicrosecondsPerSecond;

bool isLeapYear(const int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int daysInMonth(const int year, const int month) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && isLeapYear(year) ? 29 : kDays[month - 1];
}

std::int64_t daysFromCivil(std::int64_t year, const unsigned month, const unsigned day) {
    year -= month <= 2;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468 + 719162;
}

void civilFromDays(std::int64_t days, int& year, unsigned& month, unsigned& day) {
    days += 719468 - 719162;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthPrime = (5 * dayOfYear + 2) / 153;
    day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
    month = monthPrime < 10 ? monthPrime + 3 : monthPrime - 9;
    year = static_cast<int>(static_cast<std::int64_t>(yearOfEra) + era * 400 + (month <= 2));
}

int validate(const RtcDateTime* time) {
    if (!time) return RTC_ERROR_INVALID_POINTER;
    if (time->year < 1 || time->year > 9999) return RTC_ERROR_INVALID_YEAR;
    if (time->month < 1 || time->month > 12) return RTC_ERROR_INVALID_MONTH;
    if (time->day < 1 || time->day > daysInMonth(time->year, time->month)) return RTC_ERROR_INVALID_DAY;
    if (time->hour > 23) return RTC_ERROR_INVALID_HOUR;
    if (time->minute > 59) return RTC_ERROR_INVALID_MINUTE;
    if (time->second > 59) return RTC_ERROR_INVALID_SECOND;
    if (time->microsecond > 999999) return RTC_ERROR_INVALID_MICROSECOND;
    return 0;
}

std::uint64_t toTick(const RtcDateTime& time) {
    const std::int64_t days = daysFromCivil(time.year, time.month, time.day);
    const std::int64_t seconds = days * kSecondsPerDay + time.hour * 3600 + time.minute * 60 + time.second;
    return static_cast<std::uint64_t>(seconds * kMicrosecondsPerSecond + time.microsecond);
}

void fromTick(const std::uint64_t tick, RtcDateTime& time) {
    const std::int64_t totalSeconds = static_cast<std::int64_t>(tick / kMicrosecondsPerSecond);
    const std::int64_t days = totalSeconds / kSecondsPerDay;
    const std::int64_t secondOfDay = totalSeconds % kSecondsPerDay;
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civilFromDays(days, year, month, day);
    time.year = static_cast<std::uint16_t>(year);
    time.month = static_cast<std::uint16_t>(month);
    time.day = static_cast<std::uint16_t>(day);
    time.hour = static_cast<std::uint16_t>(secondOfDay / 3600);
    time.minute = static_cast<std::uint16_t>((secondOfDay % 3600) / 60);
    time.second = static_cast<std::uint16_t>(secondOfDay % 60);
    time.microsecond = static_cast<std::uint32_t>(tick % kMicrosecondsPerSecond);
}

std::uint64_t currentTick() {
    const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
    return kUnixEpochTick + static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(sinceEpoch).count());
}

std::int64_t localOffsetSeconds(const std::int64_t unixSeconds) {
    const std::time_t value = static_cast<std::time_t>(unixSeconds);
    std::tm local{};
    localtime_r(&value, &local);
    return local.tm_gmtoff;
}

int addTicks(RtcTick* destination, const RtcTick* source, const std::int64_t delta) {
    if (!destination || !source) return RTC_ERROR_INVALID_POINTER;
    const auto result = static_cast<std::int64_t>(source->tick) + delta;
    if (result < 0) return RTC_ERROR_INVALID_VALUE;
    destination->tick = static_cast<std::uint64_t>(result);
    return 0;
}

}

extern "C" {

int APS5_VABI sceRtcCheckValid(const RtcDateTime* time) {
    return validate(time);
}

int APS5_VABI sceRtcConvertLocalTimeToUtc(const RtcTick* local_time, RtcTick* utc) {
    if (!local_time || !utc) return RTC_ERROR_INVALID_POINTER;
    const std::int64_t unixSeconds = (static_cast<std::int64_t>(local_time->tick) - static_cast<std::int64_t>(kUnixEpochTick)) / kMicrosecondsPerSecond;
    return addTicks(utc, local_time, -localOffsetSeconds(unixSeconds) * kMicrosecondsPerSecond);
}

int APS5_VABI sceRtcConvertUtcToLocalTime(const RtcTick* utc, RtcTick* local_time) {
    if (!utc || !local_time) return RTC_ERROR_INVALID_POINTER;
    const std::int64_t unixSeconds = (static_cast<std::int64_t>(utc->tick) - static_cast<std::int64_t>(kUnixEpochTick)) / kMicrosecondsPerSecond;
    return addTicks(local_time, utc, localOffsetSeconds(unixSeconds) * kMicrosecondsPerSecond);
}

int APS5_VABI sceRtcFormatRFC3339(char* date_time, const RtcTick* utc, int time_zone_minutes) {
 (void)date_time;
 (void)utc;
 (void)time_zone_minutes;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceRtcGetCurrentClock(RtcDateTime* time, int time_zone_minutes) {
    if (!time) return RTC_ERROR_INVALID_POINTER;
    fromTick(currentTick() + static_cast<std::int64_t>(time_zone_minutes) * 60 * kMicrosecondsPerSecond, *time);
    return 0;
}

int APS5_VABI sceRtcGetCurrentClockLocalTime(RtcDateTime* time) {
    if (!time) return RTC_ERROR_INVALID_POINTER;
    const std::uint64_t now = currentTick();
    const std::int64_t unixSeconds = static_cast<std::int64_t>((now - kUnixEpochTick) / kMicrosecondsPerSecond);
    fromTick(now + localOffsetSeconds(unixSeconds) * kMicrosecondsPerSecond, *time);
    return 0;
}

int APS5_VABI sceRtcGetCurrentNetworkTick(RtcTick* tick) {
    if (!tick) return RTC_ERROR_INVALID_POINTER;
    tick->tick = currentTick();
    return 0;
}

int APS5_VABI sceRtcGetCurrentTick(RtcTick* tick) {
    if (!tick) return RTC_ERROR_INVALID_POINTER;
    tick->tick = currentTick();
    return 0;
}

int APS5_VABI sceRtcGetDayOfWeek(int year, int month, int day) {
    if (year < 1 || month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month)) return RTC_ERROR_INVALID_VALUE;
    const std::int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return static_cast<int>((days + 1) % 7);
}

int APS5_VABI sceRtcGetTick(const RtcDateTime* time, RtcTick* tick) {
    if (!tick) return RTC_ERROR_INVALID_POINTER;
    const int error = validate(time);
    if (error != 0) return error;
    tick->tick = toTick(*time);
    return 0;
}

int APS5_VABI sceRtcGetTickResolution(void) {
    return static_cast<int>(kMicrosecondsPerSecond);
}

int APS5_VABI sceRtcGetTime_t(const RtcDateTime* time, int64_t* seconds) {
    if (!seconds) return RTC_ERROR_INVALID_POINTER;
    const int error = validate(time);
    if (error != 0) return error;
    *seconds = static_cast<int64_t>((static_cast<std::int64_t>(toTick(*time)) - static_cast<std::int64_t>(kUnixEpochTick)) / kMicrosecondsPerSecond);
    return 0;
}

int APS5_VABI sceRtcGetWin32FileTime(const RtcDateTime* time, uint64_t* win32_time) {
    if (!win32_time) return RTC_ERROR_INVALID_POINTER;
    const int error = validate(time);
    if (error != 0) return error;
    const std::uint64_t win32Epoch = static_cast<std::uint64_t>(kWin32EpochDays) * kSecondsPerDay * kMicrosecondsPerSecond;
    const std::uint64_t tick = toTick(*time);
    if (tick < win32Epoch) return RTC_ERROR_INVALID_VALUE;
    *win32_time = (tick - win32Epoch) * 10;
    return 0;
}

int APS5_VABI sceRtcIsLeapYear(int year) {
    if (year < 1) return RTC_ERROR_INVALID_YEAR;
    return isLeapYear(year) ? 1 : 0;
}

int APS5_VABI sceRtcParseRFC3339(RtcTick* utc, const char* date_time) {
 (void)utc;
 (void)date_time;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceRtcSetTick(RtcDateTime* time, const RtcTick* tick) {
    if (!time || !tick) return RTC_ERROR_INVALID_POINTER;
    fromTick(tick->tick, *time);
    return 0;
}

int APS5_VABI sceRtcSetTime_t(RtcDateTime* time, int64_t seconds) {
    if (!time) return RTC_ERROR_INVALID_POINTER;
    if (seconds < 0) return RTC_ERROR_INVALID_VALUE;
    fromTick(kUnixEpochTick + static_cast<std::uint64_t>(seconds) * kMicrosecondsPerSecond, *time);
    return 0;
}

int APS5_VABI sceRtcSetWin32FileTime(RtcDateTime* time, uint64_t win32_time) {
    if (!time) return RTC_ERROR_INVALID_POINTER;
    fromTick(static_cast<std::uint64_t>(kWin32EpochDays) * kSecondsPerDay * kMicrosecondsPerSecond + win32_time / 10, *time);
    return 0;
}

int APS5_VABI sceRtcTickAddDays(RtcTick* dst, const RtcTick* src, int32_t days) {
    return addTicks(dst, src, static_cast<std::int64_t>(days) * kSecondsPerDay * kMicrosecondsPerSecond);
}

int APS5_VABI sceRtcTickAddHours(RtcTick* dst, const RtcTick* src, int32_t hours) {
    return addTicks(dst, src, static_cast<std::int64_t>(hours) * 3600 * kMicrosecondsPerSecond);
}

int APS5_VABI sceRtcTickAddMicroseconds(RtcTick* dst, const RtcTick* src, int64_t usec) {
    return addTicks(dst, src, usec);
}

int APS5_VABI sceRtcTickAddMinutes(RtcTick* dst, const RtcTick* src, int64_t minutes) {
    return addTicks(dst, src, minutes * 60 * kMicrosecondsPerSecond);
}

int APS5_VABI sceRtcTickAddSeconds(RtcTick* dst, const RtcTick* src, int64_t seconds) {
    return addTicks(dst, src, seconds * kMicrosecondsPerSecond);
}

int APS5_VABI sceRtcTickAddTicks(RtcTick* dst, const RtcTick* src, int64_t ticks) {
    return addTicks(dst, src, ticks);
}

int APS5_VABI sceRtcTickAddWeeks(RtcTick* dst, const RtcTick* src, int32_t weeks) {
    return addTicks(dst, src, static_cast<std::int64_t>(weeks) * 7 * kSecondsPerDay * kMicrosecondsPerSecond);
}

}
