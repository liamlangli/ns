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
i64 os_file_size(const char *path);
const char *os_read_file(const char *path);
const char *os_read_file_part(const char *path, i64 offset, i64 size);
i32 os_dir_scan(const char *path);
const char *os_entry_name(i32 index);
const char *os_entry_path(i32 index);
i32 os_entry_depth(i32 index);
i32 os_entry_parent(i32 index);
i32 os_entry_is_dir(i32 index);
const char *os_open_file_dialog(const char *title);
const char *os_open_folder_dialog(const char *title);
const char *os_cwd(void);
const char *os_env(const char *name);
i32 os_make_dirs(const char *path);
i32 os_launch_ns_project(const char *folder, const char *entry);
