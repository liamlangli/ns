#include "os.h"
#include "ns_os.h"

#include <locale.h>
#include <time.h>

#ifdef NS_WIN
#include <windows.h>
#endif

static u64 os_epoch_ns(void) {
#ifdef NS_WIN
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER ticks;
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    return (ticks.QuadPart - 116444736000000000ULL) * 100ULL;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
#endif
}

static void os_localtime(time_t seconds, struct tm *out) {
#ifdef NS_WIN
    localtime_s(out, &seconds);
#else
    localtime_r(&seconds, out);
#endif
}

#if !defined(__APPLE__) && !defined(__linux__)
static void os_gmtime(time_t seconds, struct tm *out) {
#ifdef NS_WIN
    gmtime_s(out, &seconds);
#else
    gmtime_r(&seconds, out);
#endif
}
#endif

static i32 os_utc_offset_minutes(time_t seconds, const struct tm *local) {
#if defined(__APPLE__) || defined(__linux__)
    ns_unused(seconds);
    return (i32)(local->tm_gmtoff / 60);
#else
    struct tm utc;
    os_gmtime(seconds, &utc);
    return (i32)difftime(mktime((struct tm *)local), mktime(&utc)) / 60;
#endif
}

static const char *os_strftime_now(const char *fmt) {
    static char buf[128];
    time_t seconds = (time_t)(os_epoch_ns() / 1000000000ULL);
    struct tm local;
    os_localtime(seconds, &local);
    setlocale(LC_TIME, "");
    size_t len = strftime(buf, sizeof(buf), fmt, &local);
    if (len == 0) buf[0] = '\0';
    return buf;
}

f64 os_time(void) {
    return (f64)os_epoch_ns() / 1000000000.0;
}

u64 os_time_ms(void) {
    return os_epoch_ns() / 1000000ULL;
}

u64 os_time_us(void) {
    return os_epoch_ns() / 1000ULL;
}

u64 os_time_ns(void) {
    return os_epoch_ns();
}

os_date *os_date_now(void) {
    u64 ns = os_epoch_ns();
    time_t seconds = (time_t)(ns / 1000000000ULL);
    i32 subsecond_ns = (i32)(ns % 1000000000ULL);
    struct tm local;
    os_localtime(seconds, &local);

    os_date *date = (os_date *)ns_malloc(sizeof(os_date));
    if (date == NULL) return NULL;
    date->year = local.tm_year + 1900;
    date->month = local.tm_mon + 1;
    date->day = local.tm_mday;
    date->hour = local.tm_hour;
    date->minute = local.tm_min;
    date->second = local.tm_sec;
    date->millisecond = subsecond_ns / 1000000;
    date->microsecond = (subsecond_ns / 1000) % 1000;
    date->nanosecond = subsecond_ns % 1000;
    date->utc_offset_minutes = os_utc_offset_minutes(seconds, &local);
    return date;
}

const char *os_time_string(void) {
    return os_strftime_now("%H:%M:%S");
}

const char *os_date_string(void) {
    return os_strftime_now("%Y-%m-%d");
}

const char *os_locale_date_string(void) {
    return os_strftime_now("%x");
}

const char *os_locale_date_time_string(void) {
    return os_strftime_now("%c");
}

static char *os_read_buffer = NULL;

static const char *os_take_read_buffer(ns_str data) {
    free(os_read_buffer);
    os_read_buffer = NULL;
    if (data.data == ns_null) return "";
    os_read_buffer = data.data;
    return os_read_buffer;
}

i64 os_file_size(const char *path) {
    if (!path) return -1;
    return ns_os_file_size(ns_str_cstr((char *)path));
}

const char *os_read_file(const char *path) {
    if (!path) return "";
    return os_take_read_buffer(ns_os_read_file(ns_str_cstr((char *)path)));
}

const char *os_read_file_part(const char *path, i64 offset, i64 size) {
    if (!path) return "";
    return os_take_read_buffer(ns_os_read_file_part(ns_str_cstr((char *)path), offset, size));
}
