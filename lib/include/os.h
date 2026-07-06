#pragma once

#include "ns_type.h"

typedef struct os_date {
    i32 year;
    i32 month;
    i32 day;
    i32 hour;
    i32 minute;
    i32 second;
    i32 millisecond;
    i32 microsecond;
    i32 nanosecond;
    i32 utc_offset_minutes;
} os_date;

f64 os_time(void);
u64 os_time_ms(void);
u64 os_time_us(void);
u64 os_time_ns(void);
os_date *os_date_now(void);
const char *os_time_string(void);
const char *os_date_string(void);
const char *os_locale_date_string(void);
const char *os_locale_date_time_string(void);
const char *os_open_file_dialog(const char *title);
