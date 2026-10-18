#pragma once

#include "ns_type.h"

typedef enum os_platform_kind {
    OS_PLATFORM_UNKNOWN = 0,
    OS_PLATFORM_MACOS = 1,
    OS_PLATFORM_IOS = 2,
    OS_PLATFORM_VISIONOS = 3,
    OS_PLATFORM_WINDOWS = 4,
    OS_PLATFORM_LINUX = 5,
} os_platform_kind;

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

typedef struct os_lock os_lock;
typedef struct os_semaphore os_semaphore;

i32 os_platform(void);
i32 os_cpu_count(void);
u64 os_process_memory(void);
u64 os_memory_total(void);
os_lock *os_lock_create(void);
i32 os_lock_acquire(os_lock *lock);
i32 os_lock_try_acquire(os_lock *lock);
i32 os_lock_release(os_lock *lock);
void os_lock_destroy(os_lock *lock);
os_semaphore *os_semaphore_create(i32 initial_count);
os_semaphore *os_semaphore_create_bounded(i32 initial_count, i32 max_count);
i32 os_semaphore_wait(os_semaphore *semaphore);
i32 os_semaphore_try_wait(os_semaphore *semaphore);
i32 os_semaphore_signal(os_semaphore *semaphore);
void os_semaphore_destroy(os_semaphore *semaphore);

f64 os_time(void);
u64 os_time_ms(void);
u64 os_time_us(void);
u64 os_time_ns(void);
os_date *os_date_now(void);
const char *os_time_string(void);
const char *os_date_string(void);
const char *os_locale_date_string(void);
const char *os_locale_date_time_string(void);
void os_vibrate(f64 intensity, f64 duration);
void os_impact_prepare(void);
void os_impact(void);
ns_bool os_motion_start(f64 hz);
void os_motion_stop(void);
ns_bool os_motion_attitude(f64 *values);
ns_bool os_file_text_metrics(const char *path, i64 *values);
i64 os_file_size(const char *path);
const char *os_file_hash(const char *path);
const char *os_text_hash(const char *text);
i32 os_git_status(void);
const char *os_git_revision(const char *path);
const char *os_git_base(const char *path, i32 max_bytes);
const char *os_git_diff_text(const char *before, const char *after, i32 max_bytes);
// Newline-separated paths relative to `path`. Empty with status 0 is clean.
const char *os_git_changes(const char *path);
// The same listing with each line led by "M ", "A " or "D ".
const char *os_git_change_kinds(const char *path);
const char *os_git_run(const char *folder, const char *args);
i32 os_git_progress(void);
const char *os_read_file(const char *path);
const char *os_read_file_part(const char *path, i64 offset, i64 size);
i32 os_write_file_atomic(const char *path, const char *text);
i64 os_read_file_bytes(const char *path, u8 *data, i32 capacity);
i32 os_write_file_bytes_atomic(const char *path, const u8 *data, i32 size);
const char *os_app_data_dir(const char *app_name);
i32 os_dir_scan(const char *path);
const char *os_entry_name(i32 index);
const char *os_entry_path(i32 index);
i32 os_entry_depth(i32 index);
i32 os_entry_parent(i32 index);
i32 os_entry_is_dir(i32 index);
i32 os_watch_start(const char *path);
i32 os_watch_poll(void);
void os_watch_stop(void);
const char *os_open_file_dialog(const char *title);
const char *os_save_file_dialog(const char *title, const char *suggested_name);
const char *os_open_folder_dialog(const char *title);
const char *os_cwd(void);
const char *os_env(const char *name);
// NS_PATCH_VERSION as published by the host (doc/patch.md), 0 when unset.
i32 os_patch_version(void);
// NS_PATCH_LABEL as published by the host, "" when unset.
const char *os_patch_label(void);
i32 os_make_dirs(const char *path);
// Rename or move a file or directory without replacing an existing target.
// Returns 1 on success, 0 on failure.
i32 os_rename_path(const char *from_path, const char *to_path);
i32 os_launch_ns_project(const char *folder, const char *entry);

// Interactive shell over a pseudo-terminal, rooted at `folder`.
// os_pty_open starts the user's $SHELL as a login shell and returns a handle
// above zero, or 0 when no terminal can be created on this platform.
// os_pty_read returns up to `capacity` bytes of output as a string (empty when
// none arrived), and os_pty_write sends input. Both are non-blocking.
// os_pty_running reports 1 while the shell runs and 0 after it exits.
// os_pty_resize changes the reported rows/columns; os_pty_close ends the shell.
i32 os_pty_open(const char *folder, i32 cols, i32 rows);
const char *os_pty_read(i32 handle, i32 capacity);
i32 os_pty_write(i32 handle, const char *data);
i32 os_pty_resize(i32 handle, i32 cols, i32 rows);
i32 os_pty_running(i32 handle);
void os_pty_close(i32 handle);

// Start `<exe> profile --live-port <port> [entry]` in `folder`. Returns the
// child process id, or 0 when the process could not be started.
i32 os_launch_ns_profile(const char *exe, const char *folder, const char *entry, i32 port);
// 1 while the child started above is still running; 0 once it has exited.
i32 os_process_alive(i32 pid);
// Ask that child to terminate. Returns 1 when the request was delivered.
i32 os_process_stop(i32 pid);

i32 os_share_file(const char *path);
