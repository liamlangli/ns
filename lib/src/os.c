#include "os.h"
#include "ns_os.h"
#include "ns_sha256_core.h"

#include <locale.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#if TARGET_OS_OSX
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif
#endif

#ifdef NS_WIN
#include <windows.h>
#include <direct.h>
#include <psapi.h>
#else
#include <dirent.h>
#include <regex.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

#define OS_MAX_ENTRIES 4096
#define OS_MAX_PATH 4096
#define OS_MAX_NAME 256

typedef struct os_file_entry {
    char name[OS_MAX_PATH];
    char path[OS_MAX_PATH];
    i32 depth;
    i32 parent;
    i32 is_dir;
} os_file_entry;

typedef struct os_scan_child {
    char name[OS_MAX_NAME];
    i32 is_dir;
} os_scan_child;

static os_file_entry os_entries[OS_MAX_ENTRIES];
static i32 os_entry_count = 0;

i32 os_platform(void) {
#if defined(__APPLE__) && defined(TARGET_OS_VISION) && TARGET_OS_VISION
    return OS_PLATFORM_VISIONOS;
#elif defined(__APPLE__) && TARGET_OS_IOS
    return OS_PLATFORM_IOS;
#elif defined(__APPLE__) && TARGET_OS_OSX
    return OS_PLATFORM_MACOS;
#elif defined(NS_WIN) || defined(_WIN32)
    return OS_PLATFORM_WINDOWS;
#elif defined(NS_LINUX) || defined(__linux__)
    return OS_PLATFORM_LINUX;
#else
    return OS_PLATFORM_UNKNOWN;
#endif
}

i32 os_cpu_count(void) {
#ifdef NS_WIN
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count > 0 && count <= INT_MAX ? (i32)count : 1;
#else
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 && count <= INT_MAX ? (i32)count : 1;
#endif
}

u64 os_process_memory(void) {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return (u64)info.resident_size;
#elif defined(NS_WIN)
    // Resolve from kernel32 to avoid adding a psapi link dependency.
    typedef BOOL (WINAPI *memory_info_fn)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    memory_info_fn query = (memory_info_fn)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    PROCESS_MEMORY_COUNTERS info = {0};
    if (!query || !query(GetCurrentProcess(), &info, sizeof(info))) return 0;
    return (u64)info.WorkingSetSize;
#elif defined(__linux__)
    FILE *file = fopen("/proc/self/statm", "r");
    if (!file) return 0;
    unsigned long long pages = 0;
    int read = fscanf(file, "%*u %llu", &pages);
    fclose(file);
    long page_size = sysconf(_SC_PAGESIZE);
    return read == 1 && page_size > 0 ? (u64)pages * (u64)page_size : 0;
#else
    return 0;
#endif
}

u64 os_memory_total(void) {
#if defined(__APPLE__)
    u64 bytes = 0;
    size_t size = sizeof(bytes);
    return sysctlbyname("hw.memsize", &bytes, &size, NULL, 0) == 0 ? bytes : 0;
#elif defined(NS_WIN)
    MEMORYSTATUSEX info = {0};
    info.dwLength = sizeof(info);
    return GlobalMemoryStatusEx(&info) ? (u64)info.ullTotalPhys : 0;
#elif defined(__linux__)
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    return pages > 0 && page_size > 0 ? (u64)pages * (u64)page_size : 0;
#else
    return 0;
#endif
}

struct os_lock {
#ifdef NS_WIN
    SRWLOCK native;
#else
    pthread_mutex_t native;
#endif
};

struct os_semaphore {
#ifdef NS_WIN
    SRWLOCK lock;
    CONDITION_VARIABLE condition;
#else
    pthread_mutex_t lock;
    pthread_cond_t condition;
#endif
    i32 count;
    i32 max_count;
};

os_lock *os_lock_create(void) {
    os_lock *lock = (os_lock *)malloc(sizeof(os_lock));
    if (!lock) return NULL;
#ifdef NS_WIN
    InitializeSRWLock(&lock->native);
#else
    if (pthread_mutex_init(&lock->native, NULL) != 0) {
        free(lock);
        return NULL;
    }
#endif
    return lock;
}

i32 os_lock_acquire(os_lock *lock) {
    if (!lock) return 0;
#ifdef NS_WIN
    AcquireSRWLockExclusive(&lock->native);
    return 1;
#else
    return pthread_mutex_lock(&lock->native) == 0;
#endif
}

i32 os_lock_try_acquire(os_lock *lock) {
    if (!lock) return 0;
#ifdef NS_WIN
    return TryAcquireSRWLockExclusive(&lock->native) != 0;
#else
    return pthread_mutex_trylock(&lock->native) == 0;
#endif
}

i32 os_lock_release(os_lock *lock) {
    if (!lock) return 0;
#ifdef NS_WIN
    ReleaseSRWLockExclusive(&lock->native);
    return 1;
#else
    return pthread_mutex_unlock(&lock->native) == 0;
#endif
}

void os_lock_destroy(os_lock *lock) {
    if (!lock) return;
#ifndef NS_WIN
    pthread_mutex_destroy(&lock->native);
#endif
    free(lock);
}

os_semaphore *os_semaphore_create_bounded(i32 initial_count, i32 max_count) {
    if (initial_count < 0 || max_count <= 0 || initial_count > max_count) return NULL;
    os_semaphore *semaphore = (os_semaphore *)malloc(sizeof(os_semaphore));
    if (!semaphore) return NULL;
#ifdef NS_WIN
    InitializeSRWLock(&semaphore->lock);
    InitializeConditionVariable(&semaphore->condition);
#else
    if (pthread_mutex_init(&semaphore->lock, NULL) != 0) {
        free(semaphore);
        return NULL;
    }
    if (pthread_cond_init(&semaphore->condition, NULL) != 0) {
        pthread_mutex_destroy(&semaphore->lock);
        free(semaphore);
        return NULL;
    }
#endif
    semaphore->count = initial_count;
    semaphore->max_count = max_count;
    return semaphore;
}

os_semaphore *os_semaphore_create(i32 initial_count) {
    return os_semaphore_create_bounded(initial_count, INT_MAX);
}

i32 os_semaphore_wait(os_semaphore *semaphore) {
    if (!semaphore) return 0;
#ifdef NS_WIN
    AcquireSRWLockExclusive(&semaphore->lock);
    while (semaphore->count == 0) {
        SleepConditionVariableSRW(&semaphore->condition, &semaphore->lock, INFINITE, 0);
    }
    semaphore->count--;
    ReleaseSRWLockExclusive(&semaphore->lock);
    return 1;
#else
    if (pthread_mutex_lock(&semaphore->lock) != 0) return 0;
    while (semaphore->count == 0) {
        if (pthread_cond_wait(&semaphore->condition, &semaphore->lock) != 0) {
            pthread_mutex_unlock(&semaphore->lock);
            return 0;
        }
    }
    semaphore->count--;
    return pthread_mutex_unlock(&semaphore->lock) == 0;
#endif
}

i32 os_semaphore_try_wait(os_semaphore *semaphore) {
    if (!semaphore) return 0;
#ifdef NS_WIN
    AcquireSRWLockExclusive(&semaphore->lock);
#else
    if (pthread_mutex_lock(&semaphore->lock) != 0) return 0;
#endif
    i32 acquired = semaphore->count > 0;
    if (acquired) semaphore->count--;
#ifdef NS_WIN
    ReleaseSRWLockExclusive(&semaphore->lock);
#else
    pthread_mutex_unlock(&semaphore->lock);
#endif
    return acquired;
}

i32 os_semaphore_signal(os_semaphore *semaphore) {
    if (!semaphore) return 0;
#ifdef NS_WIN
    AcquireSRWLockExclusive(&semaphore->lock);
#else
    if (pthread_mutex_lock(&semaphore->lock) != 0) return 0;
#endif
    i32 signalled = semaphore->count < semaphore->max_count;
    if (signalled) {
        semaphore->count++;
#ifdef NS_WIN
        WakeConditionVariable(&semaphore->condition);
#else
        pthread_cond_signal(&semaphore->condition);
#endif
    }
#ifdef NS_WIN
    ReleaseSRWLockExclusive(&semaphore->lock);
#else
    pthread_mutex_unlock(&semaphore->lock);
#endif
    return signalled;
}

void os_semaphore_destroy(os_semaphore *semaphore) {
    if (!semaphore) return;
#ifndef NS_WIN
    pthread_cond_destroy(&semaphore->condition);
    pthread_mutex_destroy(&semaphore->lock);
#endif
    free(semaphore);
}

#define OS_MAX_IGNORE_RULES 256

typedef struct os_ignore_rule {
    char pattern[OS_MAX_PATH];
    i32 negated;
    i32 directory_only;
    i32 anchored;
    i32 has_slash;
} os_ignore_rule;

static os_ignore_rule os_ignore_rules[OS_MAX_IGNORE_RULES];
static i32 os_ignore_rule_count = 0;
static char os_scan_root[OS_MAX_PATH];

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

    // Per-thread ring, not an allocation nobody frees; see ui_result_rect.
    static _Thread_local os_date ring[16];
    static _Thread_local u32 next;
    os_date *date = &ring[next++ % 16];
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

#if !defined(__APPLE__)
void os_vibrate(f64 intensity, f64 duration) {
    ns_unused(intensity);
    ns_unused(duration);
}

void os_impact_prepare(void) {}
void os_impact(void) {}

ns_bool os_motion_start(f64 hz) {
    ns_unused(hz);
    return false;
}

void os_motion_stop(void) {}

ns_bool os_motion_attitude(f64 *values) {
    ns_unused(values);
    return false;
}
#endif

static _Thread_local char *os_read_buffer = NULL;
#ifndef NS_WIN
static pthread_key_t os_read_buffer_key;
static pthread_once_t os_read_buffer_once = PTHREAD_ONCE_INIT;
static void os_read_buffer_cleanup(void *data) { free(data); }
static void os_read_buffer_key_init(void) { pthread_key_create(&os_read_buffer_key, os_read_buffer_cleanup); }
#endif

static const char *os_take_read_buffer(ns_str data) {
    free(os_read_buffer);
    os_read_buffer = NULL;
#ifndef NS_WIN
    pthread_once(&os_read_buffer_once, os_read_buffer_key_init);
    pthread_setspecific(os_read_buffer_key, NULL);
#endif
    if (data.data == ns_null) return "";
    os_read_buffer = data.data;
#ifndef NS_WIN
    pthread_setspecific(os_read_buffer_key, os_read_buffer);
#endif
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

i32 os_write_file_atomic(const char *path, const char *text) {
    if (!path || !path[0] || !text) return 0;
    return os_write_file_bytes_atomic(path, (const u8 *)text, (i32)strlen(text));
}

i64 os_read_file_bytes(const char *path, u8 *data, i32 capacity) {
    if (!path || !path[0] || !data || capacity < 0) return -1;
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    size_t read_size = fread(data, 1, (size_t)capacity, file);
    if (read_size < (size_t)capacity && ferror(file)) {
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) return -1;
    return (i64)read_size;
}

i32 os_write_file_bytes_atomic(const char *path, const u8 *data, i32 size) {
    if (!path || !path[0] || (!data && size > 0) || size < 0) return 0;
    char temporary[OS_MAX_PATH];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary)) return 0;
    FILE *file = fopen(temporary, "wb");
    if (!file) return 0;
    size_t len = (size_t)size;
    ns_bool ok = fwrite(data, 1, len, file) == len;
    if (fflush(file) != 0) ok = false;
#if !defined(NS_WIN)
    if (ok && fsync(fileno(file)) != 0) ok = false;
#endif
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        remove(temporary);
        return 0;
    }
#ifdef NS_WIN
    remove(path);
#endif
    if (rename(temporary, path) != 0) {
        remove(temporary);
        return 0;
    }
    return 1;
}

const char *os_app_data_dir(const char *app_name) {
    static char path[OS_MAX_PATH];
    const char *name = app_name && app_name[0] ? app_name : "ns";
#ifdef NS_WIN
    const char *root = getenv("LOCALAPPDATA");
    if (!root || !root[0]) root = getenv("USERPROFILE");
    snprintf(path, sizeof(path), "%s\\%s", root ? root : ".", name);
#elif defined(__APPLE__)
    const char *root = getenv("HOME");
    snprintf(path, sizeof(path), "%s/Library/Application Support/%s", root ? root : ".", name);
#else
    const char *root = getenv("XDG_DATA_HOME");
    if (root && root[0]) snprintf(path, sizeof(path), "%s/%s", root, name);
    else snprintf(path, sizeof(path), "%s/.local/share/%s", getenv("HOME") ? getenv("HOME") : ".", name);
#endif
    os_make_dirs(path);
    return path;
}

const char *os_documents_dir(const char *app_name) {
    static char path[OS_MAX_PATH];
    const char *name = app_name && app_name[0] ? app_name : "ns";
#ifdef NS_WIN
    const char *root = getenv("USERPROFILE");
    snprintf(path, sizeof(path), "%s\\Documents\\%s", root ? root : ".", name);
#elif defined(__APPLE__) && (TARGET_OS_IOS || (defined(TARGET_OS_VISION) && TARGET_OS_VISION))
    // The sandbox Documents folder is already the app's own, and is what the
    // Files app shows for it.
    ns_unused(name);
    const char *root = getenv("HOME");
    snprintf(path, sizeof(path), "%s/Documents", root ? root : ".");
#else
    const char *root = getenv("HOME");
    snprintf(path, sizeof(path), "%s/Documents/%s", root ? root : ".", name);
#endif
    os_make_dirs(path);
    return path;
}

const char *os_cwd(void) {
    static char path[OS_MAX_PATH];
#ifdef NS_WIN
    if (!_getcwd(path, sizeof(path))) path[0] = '\0';
#else
    if (!getcwd(path, sizeof(path))) path[0] = '\0';
#endif
    return path;
}

const char *os_env(const char *name) {
    if (!name || !name[0]) return "";
    const char *value = getenv(name);
    return value ? value : "";
}

i32 os_patch_version(void) {
    const char *text = getenv("NS_PATCH_VERSION");
    if (!text || !text[0]) return 0;
    i64 value = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9' || value > 0x7fffffff / 10) return 0;
        value = value * 10 + (*p - '0');
    }
    return value <= 0x7fffffff ? (i32)value : 0;
}

const char *os_patch_label(void) {
    return os_env("NS_PATCH_LABEL");
}

i32 os_make_dirs(const char *path) {
    if (!path || !path[0]) return 0;
    char buf[OS_MAX_PATH];
    snprintf(buf, sizeof(buf), "%s", path);
    size_t n = strlen(buf);
    while (n > 1 && (buf[n - 1] == '/' || buf[n - 1] == '\\')) buf[--n] = '\0';
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        char saved = *p;
        *p = '\0';
#ifdef NS_WIN
        if (!(strlen(buf) == 2 && buf[1] == ':')) _mkdir(buf);
#else
        mkdir(buf, 0755);
#endif
        *p = saved;
    }
#ifdef NS_WIN
    if (_mkdir(buf) == 0 || errno == EEXIST) return 1;
#else
    if (mkdir(buf, 0755) == 0 || errno == EEXIST) return 1;
#endif
    return 0;
}

// Rename a file or directory, refusing to replace an existing target.
i32 os_rename_path(const char *from_path, const char *to_path) {
    if (!from_path || !from_path[0] || !to_path || !to_path[0]) return 0;
#ifdef NS_WIN
    return MoveFileExA(from_path, to_path, MOVEFILE_WRITE_THROUGH) ? 1 : 0;
#else
    return rename(from_path, to_path) == 0 ? 1 : 0;
#endif
}

static i32 os_glob_match(const char *pattern, const char *text) {
    while (*pattern) {
        if (*pattern == '*') {
            i32 recursive = pattern[1] == '*';
            pattern++;
            if (recursive) {
                while (*pattern == '*') pattern++;
            }
            if (!*pattern) return recursive || strchr(text, '/') == NULL;
            while (*text && (recursive || *text != '/')) {
                if (os_glob_match(pattern, text)) return 1;
                text++;
            }
            return os_glob_match(pattern, text);
        }
        if (*pattern == '?') {
            if (!*text) return 0;
            pattern++;
            text++;
            continue;
        }
        if (*pattern != *text) return 0;
        pattern++;
        text++;
    }
    return *text == '\0';
}

static i32 os_pattern_matches_component(const char *pattern, const char *path) {
    const char *part = path;
    for (;;) {
        const char *slash = strchr(part, '/');
        size_t len = slash ? (size_t)(slash - part) : strlen(part);
        char component[OS_MAX_PATH];
        if (len >= sizeof(component)) len = sizeof(component) - 1;
        memcpy(component, part, len);
        component[len] = '\0';
        if (os_glob_match(pattern, component)) return 1;
        if (!slash) break;
        part = slash + 1;
    }
    return 0;
}

static void os_load_gitignore(const char *root) {
    os_ignore_rule_count = 0;
    snprintf(os_scan_root, sizeof(os_scan_root), "%s", root);
    char path[OS_MAX_PATH];
#ifdef NS_WIN
    snprintf(path, sizeof(path), "%s\\.gitignore", root);
#else
    snprintf(path, sizeof(path), "%s/.gitignore", root);
#endif
    FILE *file = fopen(path, "rb");
    if (!file) return;
    char line[OS_MAX_PATH];
    while (os_ignore_rule_count < OS_MAX_IGNORE_RULES && fgets(line, sizeof(line), file)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';
        char *start = line;
        while (*start == ' ' || *start == '\t') start++;
        if (!*start || *start == '#') continue;
        os_ignore_rule *rule = &os_ignore_rules[os_ignore_rule_count];
        memset(rule, 0, sizeof(*rule));
        if (*start == '!') {
            rule->negated = 1;
            start++;
        }
        if (*start == '/') {
            rule->anchored = 1;
            start++;
        }
        n = strlen(start);
        if (n && start[n - 1] == '/') {
            rule->directory_only = 1;
            start[--n] = '\0';
        }
        if (!*start) continue;
        snprintf(rule->pattern, sizeof(rule->pattern), "%s", start);
        rule->has_slash = strchr(start, '/') != NULL || strchr(start, '\\') != NULL;
        for (char *p = rule->pattern; *p; p++) if (*p == '\\') *p = '/';
        os_ignore_rule_count++;
    }
    fclose(file);
}

// Lens rules are POSIX extended regular expressions over root-relative paths.
#ifndef NS_WIN
static regex_t os_lens_ignore_rules[OS_MAX_IGNORE_RULES];
static i32 os_lens_ignore_count;

static void os_load_lens_ignore(const char *root) {
    for (i32 i = 0; i < os_lens_ignore_count; i++) regfree(&os_lens_ignore_rules[i]);
    os_lens_ignore_count = 0;
    char path[OS_MAX_PATH];
    snprintf(path, sizeof(path), "%s/bin/ns_lens/ignore", root);
    FILE *file = fopen(path, "rb");
    if (!file) return;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    i32 number = 0;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        number++;
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = 0;
        if (!length || line[0] == '#') continue;
        if (os_lens_ignore_count >= OS_MAX_IGNORE_RULES) {
            fprintf(stderr, "%s:%d: too many ignore rules\n", path, number);
            break;
        }
        int status = regcomp(&os_lens_ignore_rules[os_lens_ignore_count], line, REG_EXTENDED | REG_NOSUB);
        if (status) {
            char error[256];
            regerror(status, &os_lens_ignore_rules[os_lens_ignore_count], error, sizeof(error));
            fprintf(stderr, "%s:%d: %s\n", path, number, error);
        } else {
            os_lens_ignore_count++;
        }
    }
    free(line);
    fclose(file);
}
#endif

static i32 os_path_ignored(const char *relative, i32 is_dir) {
    if (strcmp(relative, ".git") == 0 || strncmp(relative, ".git/", 5) == 0) return 1;
#ifndef NS_WIN
    for (i32 i = 0; i < os_lens_ignore_count; i++) {
        if (regexec(&os_lens_ignore_rules[i], relative, 0, NULL, 0) == 0) return 1;
    }
#endif
    i32 ignored = 0;
    for (i32 i = 0; i < os_ignore_rule_count; i++) {
        os_ignore_rule *rule = &os_ignore_rules[i];
        if (rule->directory_only && !is_dir) continue;
        i32 matched;
        if (rule->has_slash || rule->anchored) matched = os_glob_match(rule->pattern, relative);
        else matched = os_pattern_matches_component(rule->pattern, relative);
        if (matched) ignored = !rule->negated;
    }
    return ignored;
}

static void os_relative_path(const char *path, char *out, size_t out_size) {
    size_t root_len = strlen(os_scan_root);
    const char *relative = path;
    if (strncmp(path, os_scan_root, root_len) == 0) {
        relative = path + root_len;
        while (*relative == '/' || *relative == '\\') relative++;
    }
    snprintf(out, out_size, "%s", relative);
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
}

static i32 os_scan_child_compare(const void *lhs, const void *rhs) {
    const os_scan_child *a = lhs;
    const os_scan_child *b = rhs;
    if (a->is_dir != b->is_dir) return b->is_dir - a->is_dir;
    return strcmp(a->name, b->name);
}

static i32 os_entry_push(const char *name, const char *path, i32 depth, i32 parent, i32 is_dir) {
    if (os_entry_count >= OS_MAX_ENTRIES) return -1;
    i32 index = os_entry_count++;
    os_file_entry *entry = &os_entries[index];
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    snprintf(entry->path, sizeof(entry->path), "%s", path);
    entry->depth = depth;
    entry->parent = parent;
    entry->is_dir = is_dir;
    return index;
}

#ifdef NS_WIN
static void os_dir_scan_recursive(const char *root, i32 depth, i32 parent) {
    char pattern[OS_MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", root);
    WIN32_FIND_DATAA found;
    HANDLE handle = FindFirstFileA(pattern, &found);
    if (handle == INVALID_HANDLE_VALUE) return;

    os_scan_child *children = NULL;
    i32 count = 0;
    do {
        if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0 || strcmp(found.cFileName, ".DS_Store") == 0) continue;
        os_scan_child *grown = realloc(children, sizeof(os_scan_child) * (size_t)(count + 1));
        if (!grown) break;
        children = grown;
        snprintf(children[count].name, sizeof(children[count].name), "%s", found.cFileName);
        children[count].is_dir = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                                 (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        count++;
    } while (FindNextFileA(handle, &found));
    FindClose(handle);
    qsort(children, (size_t)count, sizeof(os_scan_child), os_scan_child_compare);

    for (i32 i = 0; i < count && os_entry_count < OS_MAX_ENTRIES; i++) {
        char path[OS_MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", root, children[i].name);
        char relative[OS_MAX_PATH];
        os_relative_path(path, relative, sizeof(relative));
        if (strcmp(relative, "bin/ns_lens") == 0 || strncmp(relative, "bin/ns_lens/", 12) == 0) continue;
        if (os_path_ignored(relative, children[i].is_dir)) continue;
        i32 index = os_entry_push(children[i].name, path, depth, parent, children[i].is_dir);
        if (index >= 0 && children[i].is_dir) os_dir_scan_recursive(path, depth + 1, index);
    }
    free(children);
}
#else
static void os_dir_scan_recursive(const char *root, i32 depth, i32 parent) {
    DIR *dir = opendir(root);
    if (!dir) return;

    os_scan_child *children = NULL;
    i32 count = 0;
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0 || strcmp(item->d_name, ".DS_Store") == 0) continue;
        char path[OS_MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", root, item->d_name);
        struct stat info;
        if (lstat(path, &info) != 0) continue;
        os_scan_child *grown = realloc(children, sizeof(os_scan_child) * (size_t)(count + 1));
        if (!grown) break;
        children = grown;
        snprintf(children[count].name, sizeof(children[count].name), "%s", item->d_name);
        children[count].is_dir = S_ISDIR(info.st_mode);
        count++;
    }
    closedir(dir);
    qsort(children, (size_t)count, sizeof(os_scan_child), os_scan_child_compare);

    for (i32 i = 0; i < count && os_entry_count < OS_MAX_ENTRIES; i++) {
        char path[OS_MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", root, children[i].name);
        char relative[OS_MAX_PATH];
        os_relative_path(path, relative, sizeof(relative));
        if (strcmp(relative, "bin/ns_lens") == 0 || strncmp(relative, "bin/ns_lens/", 12) == 0) continue;
        if (os_path_ignored(relative, children[i].is_dir)) continue;
        i32 index = os_entry_push(children[i].name, path, depth, parent, children[i].is_dir);
        if (index >= 0 && children[i].is_dir) os_dir_scan_recursive(path, depth + 1, index);
    }
    free(children);
}
#endif

i32 os_dir_scan(const char *path) {
    os_entry_count = 0;
    if (!path || !path[0]) return -1;
#ifdef NS_WIN
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return -1;
#else
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISDIR(info.st_mode)) return -1;
#endif
    os_load_gitignore(path);
#ifndef NS_WIN
    os_load_lens_ignore(path);
#endif
    os_dir_scan_recursive(path, 0, -1);
    return os_entry_count;
}

// Child names of one directory, one per line, in no particular order; "" for
// an empty, missing or unreadable directory. Valid until the next call.
const char *os_dir_list(const char *path) {
    static char *text = NULL;
    size_t used = 0, cap = 0;
    free(text);
    text = NULL;
    if (!path || !path[0]) return "";
#ifdef NS_WIN
    char pattern[OS_MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA found;
    HANDLE handle = FindFirstFileA(pattern, &found);
    if (handle == INVALID_HANDLE_VALUE) return "";
    do {
        const char *name = found.cFileName;
#else
    DIR *dir = opendir(path);
    if (!dir) return "";
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        const char *name = item->d_name;
#endif
        if (!strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '\n')) continue;
        size_t n = strlen(name);
        if (used + n + 2 > cap) {
            size_t grown_cap = cap ? cap * 2 : 1024;
            while (grown_cap < used + n + 2) grown_cap *= 2;
            char *grown = realloc(text, grown_cap);
            if (!grown) break;
            text = grown;
            cap = grown_cap;
        }
        memcpy(text + used, name, n);
        used += n;
        text[used++] = '\n';
        text[used] = 0;
#ifdef NS_WIN
    } while (FindNextFileA(handle, &found));
    FindClose(handle);
#else
    }
    closedir(dir);
#endif
    return text ? text : "";
}

static os_file_entry *os_entry_at(i32 index) {
    if (index < 0 || index >= os_entry_count) return NULL;
    return &os_entries[index];
}

const char *os_entry_name(i32 index) {
    os_file_entry *entry = os_entry_at(index);
    return entry ? entry->name : "";
}

const char *os_entry_path(i32 index) {
    os_file_entry *entry = os_entry_at(index);
    return entry ? entry->path : "";
}

i32 os_entry_depth(i32 index) {
    os_file_entry *entry = os_entry_at(index);
    return entry ? entry->depth : -1;
}

i32 os_entry_parent(i32 index) {
    os_file_entry *entry = os_entry_at(index);
    return entry ? entry->parent : -1;
}

i32 os_entry_is_dir(i32 index) {
    os_file_entry *entry = os_entry_at(index);
    return entry ? entry->is_dir : 0;
}

// ---------------------------------------------------------------------------
// Profiled child processes.
//
// The live profiler viewer owns its TCP listener and starts the program it
// measures itself, so Run / Restart / Stop stay inside the viewer window. The
// child is `<exe> profile --live-port <port> [entry]` running in `folder`.

#ifdef NS_WIN
#define OS_CHILD_CAP 8
typedef struct os_child {
    i32 pid;
    HANDLE handle;
} os_child;
static os_child os_children[OS_CHILD_CAP];

static void os_child_remember(i32 pid, HANDLE handle) {
    for (i32 i = 0; i < OS_CHILD_CAP; i++) {
        if (os_children[i].pid == 0) {
            os_children[i].pid = pid;
            os_children[i].handle = handle;
            return;
        }
    }
    CloseHandle(handle);
}

static os_child *os_child_find(i32 pid) {
    for (i32 i = 0; i < OS_CHILD_CAP; i++) {
        if (os_children[i].pid == pid) return &os_children[i];
    }
    return NULL;
}
#else
#include <signal.h>
#include <sys/wait.h>
#endif

i32 os_launch_ns_profile(const char *exe, const char *folder, const char *entry, i32 port) {
    if (!exe || !exe[0] || port <= 0 || port > 65535) return 0;
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", port);
    const char *dir = (folder && folder[0]) ? folder : ".";
#ifdef NS_WIN
    char command[8192];
    if (entry && entry[0]) {
        snprintf(command, sizeof(command), "\"%s\" profile --live-port %s \"%s\"", exe, port_text, entry);
    } else {
        snprintf(command, sizeof(command), "\"%s\" profile --live-port %s", exe, port_text);
    }
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessA(exe, command, NULL, NULL, FALSE, 0, NULL, dir, &startup, &process)) return 0;
    CloseHandle(process.hThread);
    os_child_remember((i32)process.dwProcessId, process.hProcess);
    return (i32)process.dwProcessId;
#else
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        // The viewer's listening socket must not survive into the target: an
        // inherited listener keeps the port bound after the viewer is gone.
        for (int fd = 3; fd < 256; fd++) close(fd);
        if (chdir(dir) != 0) _exit(127);
        if (entry && entry[0]) {
            execl(exe, exe, "profile", "--live-port", port_text, entry, (char *)NULL);
        } else {
            execl(exe, exe, "profile", "--live-port", port_text, (char *)NULL);
        }
        _exit(127);
    }
    return (i32)pid;
#endif
}

i32 os_process_alive(i32 pid) {
    if (pid <= 0) return 0;
#ifdef NS_WIN
    os_child *child = os_child_find(pid);
    if (!child) return 0;
    if (WaitForSingleObject(child->handle, 0) == WAIT_TIMEOUT) return 1;
    CloseHandle(child->handle);
    child->pid = 0;
    child->handle = NULL;
    return 0;
#else
    // Reap here as well: a child that exited stays a zombie until it is waited
    // for, and a zombie still answers kill(pid, 0).
    int status = 0;
    pid_t done = waitpid((pid_t)pid, &status, WNOHANG);
    if (done == (pid_t)pid) return 0;
    if (done < 0) return kill((pid_t)pid, 0) == 0 ? 1 : 0;
    return 1;
#endif
}

i32 os_process_stop(i32 pid) {
    if (pid <= 0) return 0;
#ifdef NS_WIN
    os_child *child = os_child_find(pid);
    if (!child) return 0;
    if (!TerminateProcess(child->handle, 0)) return 0;
    return 1;
#else
    if (kill((pid_t)pid, SIGTERM) != 0) return 0;
    return 1;
#endif
}

#if !defined(__APPLE__)
i32 os_share_file(const char *path) { ns_unused(path); return 0; }
#endif

// Start another instance of this program, detached, the same way it was
// started: the executable with the original arguments, so an interpreted
// program reruns `ns run <entry>` and a native build relaunches itself. The
// new instance reads `arg` as its first program argument (NS_ARGC = 1,
// NS_ARG0 = arg) and starts in the shell's directory ($PWD) when one is known.
#if defined(__APPLE__) && !TARGET_OS_OSX
i32 os_launch_self(const char *arg) {
    ns_unused(arg);
    return 0;
}
#elif defined(NS_WIN)
i32 os_launch_self(const char *arg) {
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) return 0;
    char command[8192];
    snprintf(command, sizeof(command), "%s", GetCommandLineA());
    char old_argc[64];
    char old_arg[OS_MAX_PATH];
    DWORD argc_len = GetEnvironmentVariableA("NS_ARGC", old_argc, sizeof(old_argc));
    DWORD arg_len = GetEnvironmentVariableA("NS_ARG0", old_arg, sizeof(old_arg));
    SetEnvironmentVariableA("NS_ARGC", "1");
    SetEnvironmentVariableA("NS_ARG0", arg ? arg : "");
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    BOOL launched = CreateProcessA(exe, command, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &startup, &process);
    SetEnvironmentVariableA("NS_ARGC", argc_len > 0 && argc_len < (DWORD)sizeof(old_argc) ? old_argc : NULL);
    SetEnvironmentVariableA("NS_ARG0", arg_len > 0 && arg_len < (DWORD)sizeof(old_arg) ? old_arg : NULL);
    if (!launched) return 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 1;
}
#else
extern char **environ;

i32 os_launch_self(const char *arg) {
    static char exe[OS_MAX_PATH];
#if defined(__APPLE__)
    uint32_t exe_size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &exe_size) != 0) return 0;
    i32 argc = *_NSGetArgc();
    char **source = *_NSGetArgv();
#else
    ssize_t exe_len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (exe_len <= 0) return 0;
    exe[exe_len] = '\0';
    static char cmdline[65536];
    i32 cmd_len = 0;
    i32 fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) {
        ssize_t got = read(fd, cmdline, sizeof(cmdline) - 1);
        if (got > 0) cmd_len = (i32)got;
        close(fd);
    }
    cmdline[cmd_len] = '\0';
    char *split[256];
    i32 argc = 0;
    for (i32 at = 0; at < cmd_len && argc < 255; at += (i32)strlen(cmdline + at) + 1) {
        split[argc++] = cmdline + at;
    }
    char **source = split;
#endif
    if (argc > 255) argc = 255;
    // Everything the child needs is built before fork(): after it only
    // async-signal-safe calls run, since the parent has other threads.
    char *child_argv[256];
    child_argv[0] = exe;
    for (i32 i = 1; i < argc; i++) child_argv[i] = source[i];
    child_argv[argc < 1 ? 1 : argc] = NULL;

    i32 env_count = 0;
    while (environ && environ[env_count]) env_count++;
    char **child_env = (char **)malloc(sizeof(char *) * (size_t)(env_count + 3));
    if (!child_env) return 0;
    static char argc_entry[] = "NS_ARGC=1";
    char *arg_entry = (char *)malloc(strlen(arg ? arg : "") + 9);
    if (!arg_entry) {
        free(child_env);
        return 0;
    }
    sprintf(arg_entry, "NS_ARG0=%s", arg ? arg : "");
    i32 n = 0;
    child_env[n++] = argc_entry;
    child_env[n++] = arg_entry;
    for (i32 i = 0; i < env_count; i++) {
        if (strncmp(environ[i], "NS_ARGC=", 8) == 0 || strncmp(environ[i], "NS_ARG0=", 8) == 0) continue;
        child_env[n++] = environ[i];
    }
    child_env[n] = NULL;

    const char *dir = getenv("PWD");
    struct stat dir_stat;
    if (!dir || stat(dir, &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode)) dir = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        // Detach twice so the new instance outlives this one and is never
        // left as this process's zombie.
        setsid();
        if (fork() != 0) _exit(0);
        for (int fd = 3; fd < 256; fd++) close(fd);
        if (dir) chdir(dir);
        execve(exe, child_argv, child_env);
        _exit(127);
    }
    free(arg_entry);
    free(child_env);
    if (pid < 0) return 0;
    int status = 0;
    waitpid(pid, &status, 0);
    return 1;
}
#endif

// ---------------------------------------------------------------------------
// Interactive shell over a pseudo-terminal.
//
// The window backends only carry keyboard input; a real shell needs a tty for
// its line editor, job control and full-screen programs. Each slot owns a pty
// master plus the shell it started, so a viewer can read output and write key
// bytes without blocking a frame. Only desktop POSIX hosts offer this; other
// targets return a zero handle so callers can hide the feature.

#define OS_PTY_SLOTS 4
#define OS_PTY_READ_MAX 65536

#if defined(NS_WIN) || (defined(__APPLE__) && (TARGET_OS_IOS || (defined(TARGET_OS_VISION) && TARGET_OS_VISION)))

i32 os_pty_open(const char *folder, i32 cols, i32 rows) {
    ns_unused(folder);
    ns_unused(cols);
    ns_unused(rows);
    return 0;
}

const char *os_pty_read(i32 handle, i32 capacity) {
    ns_unused(handle);
    ns_unused(capacity);
    return "";
}

i32 os_pty_write(i32 handle, const char *data) {
    ns_unused(handle);
    ns_unused(data);
    return -1;
}

i32 os_pty_resize(i32 handle, i32 cols, i32 rows) {
    ns_unused(handle);
    ns_unused(cols);
    ns_unused(rows);
    return 0;
}

i32 os_pty_running(i32 handle) {
    ns_unused(handle);
    return 0;
}

void os_pty_close(i32 handle) { ns_unused(handle); }

#else

#if defined(__linux__)
// glibc hides the XSI pty helpers behind feature macros the project does not
// set, so declare the libc symbols this implementation uses explicitly.
extern int posix_openpt(int oflag);
extern int grantpt(int fd);
extern int unlockpt(int fd);
extern char *ptsname(int fd);
#endif

typedef struct os_pty_slot {
    int fd;
    pid_t pid;
    i32 alive;
} os_pty_slot;

static os_pty_slot os_ptys[OS_PTY_SLOTS] = {
    { -1, 0, 0 }, { -1, 0, 0 }, { -1, 0, 0 }, { -1, 0, 0 }
};
static _Thread_local char os_pty_text[OS_PTY_READ_MAX + 1];

static os_pty_slot *os_pty_at(i32 handle) {
    if (handle <= 0 || handle > OS_PTY_SLOTS) return NULL;
    os_pty_slot *slot = &os_ptys[handle - 1];
    return slot->fd >= 0 ? slot : NULL;
}

// Reap the shell the moment it exits. The master can still hold its final
// output; the reader drains that before it closes the handle.
static void os_pty_check(os_pty_slot *slot) {
    if (!slot || !slot->alive || slot->pid <= 0) return;
    int status = 0;
    pid_t done = waitpid(slot->pid, &status, WNOHANG);
    if (done == slot->pid || (done < 0 && errno == ECHILD)) slot->alive = 0;
}

i32 os_pty_open(const char *folder, i32 cols, i32 rows) {
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;
    if (cols > 1000) cols = 1000;
    if (rows > 1000) rows = 1000;

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) return 0;
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);
        return 0;
    }
    // ptsname returns a static buffer; it is read before fork and only the UI
    // thread ever opens a terminal.
    const char *slave_name = ptsname(master);
    if (!slave_name) {
        close(master);
        return 0;
    }

    struct winsize size;
    memset(&size, 0, sizeof(size));
    size.ws_col = (unsigned short)cols;
    size.ws_row = (unsigned short)rows;
    ioctl(master, TIOCSWINSZ, &size);

    const char *shell = getenv("SHELL");
    if (!shell || !shell[0]) {
#if defined(__APPLE__)
        shell = "/bin/zsh";
#else
        shell = "/bin/sh";
#endif
    }
    const char *dir = (folder && folder[0]) ? folder : ".";
    long open_max = sysconf(_SC_OPEN_MAX);
    int close_limit = open_max > 3 && open_max < 4096 ? (int)open_max : 4096;

    // Build the child's environment before forking: setenv is not safe between
    // fork and exec in a host that runs search and Git workers.
    extern char **environ;
    char *child_env[1024];
    int child_env_count = 0;
    for (char **entry = environ; entry && *entry && child_env_count < 1020; entry++) {
        if (strncmp(*entry, "TERM=", 5) == 0) continue;
        if (strncmp(*entry, "COLORTERM=", 10) == 0) continue;
        child_env[child_env_count++] = *entry;
    }
    static char term_env[] = "TERM=xterm-256color";
    static char color_env[] = "COLORTERM=truecolor";
    child_env[child_env_count++] = term_env;
    child_env[child_env_count++] = color_env;
    child_env[child_env_count] = NULL;
    char *child_argv[3];
    child_argv[0] = (char *)shell;
    child_argv[1] = (char *)"-l";
    child_argv[2] = NULL;
    char *fallback_argv[3];
    fallback_argv[0] = (char *)"/bin/sh";
    fallback_argv[1] = (char *)"-l";
    fallback_argv[2] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        return 0;
    }
    if (pid == 0) {
        // The child is a session leader with the pty as its controlling
        // terminal, so job control and SIGWINCH work normally.
        setsid();
        int slave = open(slave_name, O_RDWR);
        if (slave < 0) _exit(127);
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) close(slave);
        close(master);
        // GUI hosts keep many descriptors open; none belong to the shell.
        for (int fd = 3; fd < close_limit; fd++) close(fd);
        if (chdir(dir) != 0) _exit(127);
        execve(shell, child_argv, child_env);
        execve("/bin/sh", fallback_argv, child_env);
        _exit(127);
    }

    int flags = fcntl(master, F_GETFL, 0);
    if (flags >= 0) fcntl(master, F_SETFL, flags | O_NONBLOCK);
    for (i32 i = 0; i < OS_PTY_SLOTS; i++) {
        if (os_ptys[i].fd >= 0) continue;
        os_ptys[i].fd = master;
        os_ptys[i].pid = pid;
        os_ptys[i].alive = 1;
        return i + 1;
    }
    close(master);
    kill(-pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    return 0;
}

const char *os_pty_read(i32 handle, i32 capacity) {
    os_pty_slot *slot = os_pty_at(handle);
    if (!slot) return "";
    if (capacity < 1) capacity = 1;
    if (capacity > OS_PTY_READ_MAX) capacity = OS_PTY_READ_MAX;
    ssize_t got = read(slot->fd, os_pty_text, (size_t)capacity);
    if (got > 0) {
        os_pty_text[got] = '\0';
        return os_pty_text;
    }
    os_pty_text[0] = '\0';
    if (got == 0) {
        slot->alive = 0;
        return "";
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        os_pty_check(slot);
        return "";
    }
    // EIO on macOS and EOF on Linux both mean the slave side is gone.
    slot->alive = 0;
    return "";
}

i32 os_pty_write(i32 handle, const char *data) {
    os_pty_slot *slot = os_pty_at(handle);
    if (!slot || !data) return -1;
    size_t length = strlen(data);
    if (length == 0) return 0;
    size_t written = 0;
    for (int attempt = 0; attempt < 64 && written < length; attempt++) {
        ssize_t got = write(slot->fd, data + written, length - written);
        if (got > 0) {
            written += (size_t)got;
            continue;
        }
        if (got < 0 && (errno == EINTR)) continue;
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        break;
    }
    return (i32)written;
}

i32 os_pty_resize(i32 handle, i32 cols, i32 rows) {
    os_pty_slot *slot = os_pty_at(handle);
    if (!slot) return 0;
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;
    if (cols > 1000) cols = 1000;
    if (rows > 1000) rows = 1000;
    struct winsize size;
    memset(&size, 0, sizeof(size));
    size.ws_col = (unsigned short)cols;
    size.ws_row = (unsigned short)rows;
    return ioctl(slot->fd, TIOCSWINSZ, &size) == 0 ? 1 : 0;
}

i32 os_pty_running(i32 handle) {
    os_pty_slot *slot = os_pty_at(handle);
    if (!slot) return 0;
    os_pty_check(slot);
    return slot->alive ? 1 : 0;
}

void os_pty_close(i32 handle) {
    os_pty_slot *slot = os_pty_at(handle);
    if (!slot) return;
    pid_t pid = slot->pid;
    close(slot->fd);
    slot->fd = -1;
    slot->pid = 0;
    slot->alive = 0;
    if (pid <= 0) return;
    // Reap first: after a child is gone its pid may belong to another process
    // group, so a signal must not be sent in its name.
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) != 0) return;
    // Hang up the whole session, then make sure it is reaped: a zombie shell
    // outlives the window otherwise.
    kill(-pid, SIGHUP);
    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(-pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
}

#endif

// Bounded native services used by code atlases and other local tools.
static _Thread_local char os_hash_hex[65];
const char *os_text_hash(const char *text) {
    if (!text) return "";
    u8 digest[32];
    ns_sha256_core_hash(text, strlen(text), digest);
    ns_sha256_core_hex(digest, os_hash_hex);
    return os_hash_hex;
}

// Line widths past the histogram share its last bucket; a percentile landing
// there reports the longest line instead.
#define OS_TEXT_WIDTH_BUCKETS 1024

static void os_text_width_count(u32 *widths, i64 columns, i64 *filled) {
    if (columns <= 0) return;
    widths[columns < OS_TEXT_WIDTH_BUCKETS ? columns : OS_TEXT_WIDTH_BUCKETS]++;
    (*filled)++;
}

// Extra rows of every line at least OS_TEXT_WIDTH_BUCKETS wide when lines
// wrap at `wrap` columns. Those widths are not in the histogram, so reread.
static int os_text_long_rows(FILE *file, i64 wrap, i64 *rows) {
    if (fseek(file, 0, SEEK_SET) != 0) return 0;
    i64 columns = 0;
    u8 data[65536];
    size_t count;
    while ((count = fread(data, 1, sizeof(data), file)) > 0) {
        for (size_t i = 0; i < count; i++) {
            if (data[i] == '\n') {
                if (columns >= OS_TEXT_WIDTH_BUCKETS) *rows += (columns - 1) / wrap;
                columns = 0;
            } else if (data[i] != '\r') columns++;
        }
    }
    if (columns >= OS_TEXT_WIDTH_BUCKETS) *rows += (columns - 1) / wrap;
    return !ferror(file);
}

ns_bool os_file_text_metrics(const char *path, i64 *values) {
    if (!values) return 0;
    values[0] = -1;
    values[1] = 0;
    values[2] = 0;
    values[3] = 0;
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) return 0;
    i64 lines = 1, columns = 0, longest = 0, filled = 0;
    u32 widths[OS_TEXT_WIDTH_BUCKETS + 1] = {0};
    u8 data[65536];
    size_t count;
    while ((count = fread(data, 1, sizeof(data), file)) > 0) {
        for (size_t i = 0; i < count; i++) {
            if (data[i] == '\n') {
                if (columns > longest) longest = columns;
                os_text_width_count(widths, columns, &filled);
                columns = 0;
                lines++;
            } else if (data[i] != '\r') columns++;
        }
    }
    if (ferror(file)) { fclose(file); return 0; }
    if (columns > longest) longest = columns;
    os_text_width_count(widths, columns, &filled);
    i64 typical = 0, need = (filled * 9 + 9) / 10, seen = 0;
    for (i64 w = 1; w <= OS_TEXT_WIDTH_BUCKETS && filled > 0; w++) {
        seen += widths[w];
        if (seen >= need) {
            typical = w < OS_TEXT_WIDTH_BUCKETS ? w : longest;
            break;
        }
    }
    // A line of w columns wrapped at `typical` takes ceil(w / typical) rows.
    i64 rows = lines;
    if (typical > 0) {
        for (i64 w = typical + 1; w < OS_TEXT_WIDTH_BUCKETS; w++) rows += (i64)widths[w] * ((w - 1) / typical);
        if (widths[OS_TEXT_WIDTH_BUCKETS] > 0 && !os_text_long_rows(file, typical, &rows)) {
            fclose(file);
            return 0;
        }
    }
    fclose(file);
    values[0] = lines;
    values[1] = longest;
    values[2] = typical;
    values[3] = rows;
    return 1;
}

const char *os_file_hash(const char *path) {
    if (!path) return "";
    FILE *file = fopen(path, "rb");
    if (!file) return "";
    struct stat before = {0}, after = {0};
    if (fstat(fileno(file), &before) != 0) { fclose(file); return ""; }
    ns_sha256_core_ctx ctx;
    ns_sha256_core_init(&ctx);
    u8 data[65536], digest[32];
    size_t size;
    while ((size = fread(data, 1, sizeof(data), file)) > 0) ns_sha256_core_update(&ctx, data, size);
    int failed = ferror(file);
    fclose(file);
    if (failed || stat(path, &after) != 0 || before.st_size != after.st_size ||
        before.st_ino != after.st_ino || before.st_dev != after.st_dev ||
        before.st_mtime != after.st_mtime || before.st_ctime != after.st_ctime) return "";
#if defined(__APPLE__)
    if (before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec || before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec) return "";
#elif !defined(NS_WIN)
    if (before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) return "";
#endif
    ns_sha256_core_final(&ctx, digest);
    ns_sha256_core_hex(digest, os_hash_hex);
    return os_hash_hex;
}

#if !defined(NS_WIN) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
#include <spawn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
extern char **environ;
static _Thread_local char *os_git_output;
static _Thread_local size_t os_git_output_len;
static _Thread_local i32 os_git_last_status = -1;
i32 os_git_status(void) { return os_git_last_status; }
// A thread's last Git result lives until its next call. Lens calls Git from
// worker tasks, each on a thread of its own, so a result left in thread-local
// storage leaked when the thread ended; these keys free it at thread exit.
static pthread_key_t os_git_keys[3];
static pthread_once_t os_git_keys_once = PTHREAD_ONCE_INIT;
static void os_git_keys_init(void) { for (int i = 0; i < 3; ++i) pthread_key_create(&os_git_keys[i], free); }
static char *os_git_keep(int which, char *text) {
    pthread_once(&os_git_keys_once, os_git_keys_init);
    pthread_setspecific(os_git_keys[which], text);
    return text;
}

// Git is launched with an argv vector, never through a shell. Reject truncated
// output instead of letting an incomplete patch masquerade as the entire diff.
static const char *os_git_capture(char *const argv[], i32 limit, int *exit_status) {
    free(os_git_output);
    os_git_output = os_git_keep(0, NULL);
    os_git_output_len = 0;
    *exit_status = -1;
    os_git_last_status = -1;
    if (limit < 1 || limit > 1048576) return "";
    int pipes[2];
    if (pipe(pipes) != 0) return "";
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    pid_t pid;
    int error = posix_spawnp(&pid, "git", &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (error) { close(pipes[0]); return ""; }
    os_git_output = os_git_keep(0, malloc((size_t)limit + 1));
    size_t used = 0;
    ns_bool failed = os_git_output == NULL;
    char chunk[8192];
    while (!failed) {
        ssize_t n = read(pipes[0], chunk, sizeof(chunk));
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; failed = true; break; }
        if (used + (size_t)n > (size_t)limit) { failed = true; break; }
        memcpy(os_git_output + used, chunk, (size_t)n);
        used += (size_t)n;
    }
    if (failed) kill(pid, SIGTERM);
    close(pipes[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) { failed = true; break; } }
    if (failed) { free(os_git_output); os_git_output = os_git_keep(0, NULL); return ""; }
    os_git_output[used] = 0;
    os_git_output_len = used;
    if (WIFEXITED(status)) *exit_status = WEXITSTATUS(status);
    os_git_last_status = *exit_status;
    return os_git_output;
}

// Porcelain v1 -z: "XY PATH\0", or "XY TO\0FROM\0" when either status is R/C.
// NUL separators cannot cross the FFI string boundary, so the result is one
// path per line, relative to `path`. A rename or copy contributes its new path;
// with `kinds`, each line starts with "M ", "A " or "D ", and a rename also
// lists its old path as deleted.
static _Thread_local char *os_git_changes_text;

// Append raw[start, end) minus the workspace prefix as one line, preceded by
// "K " when `kind` is set. Paths outside the prefix or holding a line break
// are skipped; false means the buffer is full.
static ns_bool os_git_changes_line(char *out, size_t *used, size_t cap, const char *raw, size_t start, size_t end,
                                   const char *prefix, size_t prefix_len, char kind) {
    if (end <= start) return true;
    if (prefix_len) {
        if (end - start < prefix_len || memcmp(raw + start, prefix, prefix_len) != 0) return true;
        start += prefix_len;
    }
    if (end <= start) return true;
    for (size_t k = start; k < end; k++) if (raw[k] == '\n' || raw[k] == '\r') return true;
    size_t n = end - start;
    size_t head = kind ? 2 : 0;
    if (*used + head + n + 1 > cap) return false;
    if (kind) {
        out[(*used)++] = kind;
        out[(*used)++] = ' ';
    }
    memcpy(out + *used, raw + start, n);
    for (size_t k = *used; k < *used + n; k++) if (out[k] == '\\') out[k] = '/';
    *used += n;
    out[(*used)++] = '\n';
    return true;
}

static const char *os_git_changes_list(const char *path, ns_bool kinds) {
    free(os_git_changes_text);
    os_git_changes_text = os_git_keep(1, NULL);
    if (!path || !path[0]) { os_git_last_status = -1; return ""; }
    int status = 0;
    char *repo[] = {"git", "-C", (char *)path, "rev-parse", "--is-inside-work-tree", NULL};
    os_git_capture(repo, 64, &status);
    if (status != 0) return "";
    // Porcelain paths are relative to the repository root, not to `path`.
    char *prefix_args[] = {"git", "-C", (char *)path, "rev-parse", "--show-prefix", NULL};
    const char *prefix_raw = os_git_capture(prefix_args, OS_MAX_PATH - 1, &status);
    if (status != 0 || !prefix_raw) return "";
    char prefix[OS_MAX_PATH];
    size_t prefix_len = os_git_output_len;
    if (prefix_len >= sizeof(prefix)) { os_git_last_status = -1; return ""; }
    memcpy(prefix, prefix_raw, prefix_len);
    prefix[prefix_len] = 0;
    while (prefix_len && (prefix[prefix_len - 1] == '\n' || prefix[prefix_len - 1] == '\r')) prefix[--prefix_len] = 0;
    char *args[] = {"git", "-C", (char *)path, "--no-pager", "status", "--porcelain=v1", "-z",
                    "--untracked-files=all", NULL};
    const char *raw = os_git_capture(args, 1048576, &status);
    if (status != 0 || !raw) return "";
    size_t len = os_git_output_len;
    // A rename listed with kinds takes one byte more than its raw record.
    size_t cap = len * 2 + 1;
    char *out = (char *)malloc(cap);
    if (!out) { os_git_last_status = -1; return ""; }
    size_t used = 0;
    size_t i = 0;
    ns_bool room = true;
    while (room && i + 3 <= len) {
        char x = raw[i];
        char y = raw[i + 1];
        if (raw[i + 2] != ' ') break;
        i += 3;
        // -z renames are "to\\0from\\0": the first name is the current path.
        size_t start = i;
        while (i < len && raw[i] != '\0') i++;
        size_t end = i;
        if (i < len) i++;
        size_t from_start = i;
        size_t from_end = i;
        ns_bool moved = x == 'R' || x == 'C' || y == 'R' || y == 'C';
        if (moved) {
            while (i < len && raw[i] != '\0') i++;
            from_end = i;
            if (i < len) i++;
        }
        char kind = 0;
        if (kinds) {
            if (x == '?' || moved || (x == 'A' && y != 'D')) kind = 'A';
            else if (x == 'D' || y == 'D') kind = 'D';
            else kind = 'M';
        }
        room = os_git_changes_line(out, &used, cap - 1, raw, start, end, prefix, prefix_len, kind);
        // A copy keeps its source; a rename removes it.
        if (room && kinds && (x == 'R' || y == 'R'))
            room = os_git_changes_line(out, &used, cap - 1, raw, from_start, from_end, prefix, prefix_len, 'D');
    }
    out[used] = 0;
    os_git_changes_text = os_git_keep(1, out);
    os_git_last_status = 0;
    return out;
}

const char *os_git_changes(const char *path) { return os_git_changes_list(path, false); }

const char *os_git_change_kinds(const char *path) { return os_git_changes_list(path, true); }

static ns_bool os_git_path(const char *path, char parent[OS_MAX_PATH], const char **name) {
    if (!path || !path[0] || strlen(path) >= OS_MAX_PATH) return false;
    const char *slash = strrchr(path, '/');
    if (!slash) { strcpy(parent, "."); *name = path; return true; }
    size_t n = (size_t)(slash - path);
    if (n == 0) n = 1;
    memcpy(parent, path, n); parent[n] = 0;
    *name = slash + 1;
    return (*name)[0] != 0;
}

const char *os_git_revision(const char *path) {
    char parent[OS_MAX_PATH]; const char *name; int status;
    if (!os_git_path(path, parent, &name)) return "";
    char *repo[] = {"git", "-C", parent, "rev-parse", "--is-inside-work-tree", NULL};
    os_git_capture(repo, 128, &status);
    if (status != 0) return "";
    char *head[] = {"git", "-C", parent, "rev-parse", "--verify", "HEAD", NULL};
    const char *result = os_git_capture(head, 128, &status);
    if (status != 0) return "unborn";
    size_t len = strlen(result);
    while (len && (os_git_output[len - 1] == '\n' || os_git_output[len - 1] == '\r')) os_git_output[--len] = 0;
    return os_git_output;
}

const char *os_git_base(const char *path, i32 max_bytes) {
    char parent[OS_MAX_PATH], object[OS_MAX_PATH]; const char *name; int status;
    if (!os_git_path(path, parent, &name)) return "";
    char *prefix[] = {"git", "-C", parent, "rev-parse", "--show-prefix", NULL};
    const char *result = os_git_capture(prefix, OS_MAX_PATH - 1, &status);
    if (status != 0) return "";
    size_t len = strcspn(result, "\r\n");
    if (len + strlen(name) + 6 >= sizeof(object)) return "";
    memcpy(object, "HEAD:", 5); memcpy(object + 5, result, len);
    strcpy(object + 5 + len, name);
    char *show[] = {"git", "-C", parent, "--no-pager", "show", object, NULL};
    result = os_git_capture(show, max_bytes, &status);
    return status == 0 ? result : "";
}

static ns_bool os_git_temp(char path[OS_MAX_PATH], const char *text) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    if (snprintf(path, OS_MAX_PATH, "%s/ns-lens-diff-XXXXXX", tmp) >= OS_MAX_PATH) return false;
    int fd = mkstemp(path);
    if (fd < 0) return false;
    size_t len = strlen(text), used = 0;
    while (used < len) {
        ssize_t n = write(fd, text + used, len - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); unlink(path); return false; }
        used += (size_t)n;
    }
    if (close(fd) != 0) { unlink(path); return false; }
    return true;
}

const char *os_git_diff_text(const char *before, const char *after, i32 max_bytes) {
    if (!before || !after) return "";
    if (strcmp(before, after) == 0) { os_git_last_status = 0; return ""; }
    char a[OS_MAX_PATH], b[OS_MAX_PATH]; int status;
    if (!os_git_temp(a, before)) return "";
    if (!os_git_temp(b, after)) { unlink(a); return ""; }
    char *args[] = {"git", "--no-pager", "diff", "--no-index", "--no-ext-diff", "--no-textconv",
                    "--no-color", "--unified=0", "--", a, b, NULL};
    const char *result = os_git_capture(args, max_bytes, &status);
    unlink(a); unlink(b);
    return status == 0 || status == 1 ? result : "";
}

// Progress is process-wide so the thread that draws can read it while a
// worker thread waits on Git.
static i32 os_git_progress_value = -1;
i32 os_git_progress(void) { return __atomic_load_n(&os_git_progress_value, __ATOMIC_RELAXED); }

#define OS_GIT_RUN_KEEP 65536
// Enough for a commit that names each of its paths.
#define OS_GIT_RUN_ARGS 8192
static _Thread_local char *os_git_run_text;

// The environment plus GIT_TERMINAL_PROMPT=0, so a missing credential fails
// instead of waiting on a prompt nobody can see.
static char **os_git_run_env(void) {
    size_t count = 0;
    while (environ[count]) count++;
    char **env = malloc((count + 2) * sizeof(char *));
    if (!env) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(environ[i], "GIT_TERMINAL_PROMPT=", 20) != 0) env[used++] = environ[i];
    }
    env[used++] = "GIT_TERMINAL_PROMPT=0";
    env[used] = NULL;
    return env;
}

// Track the last "NN%" in Git's progress output across read boundaries.
static void os_git_run_scan(const char *data, size_t n, i32 *number, i32 *digits) {
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c >= '0' && c <= '9') {
            if (*digits < 3) *number = *number * 10 + (c - '0');
            (*digits)++;
        } else {
            if (c == '%' && *digits >= 1 && *digits <= 3 && *number <= 100)
                __atomic_store_n(&os_git_progress_value, *number, __ATOMIC_RELAXED);
            *number = 0;
            *digits = 0;
        }
    }
}

const char *os_git_run(const char *folder, const char *args) {
    free(os_git_run_text);
    os_git_run_text = os_git_keep(2, NULL);
    os_git_last_status = -1;
    __atomic_store_n(&os_git_progress_value, -1, __ATOMIC_RELAXED);
    if (!folder || !folder[0] || !args) return "";
    char *list = strdup(args);
    if (!list) return "";
    char **argv = malloc((OS_GIT_RUN_ARGS + 4) * sizeof(char *));
    if (!argv) { free(list); return ""; }
    int argc = 0;
    argv[argc++] = "git";
    argv[argc++] = "-C";
    argv[argc++] = (char *)folder;
    for (char *p = list; *p;) {
        char *end = strchr(p, '\n');
        if (end) *end = 0;
        if (*p) {
            if (argc >= OS_GIT_RUN_ARGS + 3) { free(argv); free(list); return ""; }
            argv[argc++] = p;
        }
        if (!end) break;
        p = end + 1;
    }
    argv[argc] = NULL;
    char **env = os_git_run_env();
    int pipes[2];
    if (!env || pipe(pipes) != 0) { free(env); free(argv); free(list); return ""; }
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_SETSID
    // A new session has no controlling terminal, so ssh cannot prompt on the
    // terminal that launched this app and stall the job.
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#endif
    pid_t pid;
    int error = posix_spawnp(&pid, "git", &actions, &attr, argv, env);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    free(env);
    free(argv);
    free(list);
    if (error) { close(pipes[0]); return ""; }
    // Keep only the tail: the last lines carry Git's result or error.
    char *text = malloc(OS_GIT_RUN_KEEP * 2 + 1);
    size_t used = 0;
    i32 number = 0, digits = 0;
    char chunk[8192];
    for (;;) {
        ssize_t n = read(pipes[0], chunk, sizeof(chunk));
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; break; }
        os_git_run_scan(chunk, (size_t)n, &number, &digits);
        if (!text) continue;
        if (used + (size_t)n > OS_GIT_RUN_KEEP * 2) {
            memmove(text, text + used - OS_GIT_RUN_KEEP, OS_GIT_RUN_KEEP);
            used = OS_GIT_RUN_KEEP;
        }
        memcpy(text + used, chunk, (size_t)n);
        used += (size_t)n;
    }
    close(pipes[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) { status = -1; break; } }
    if (!text) return "";
    text[used] = 0;
    os_git_run_text = os_git_keep(2, text);
    os_git_last_status = status >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return os_git_run_text;
}
#elif !defined(NS_GIT_EMBEDDED)
i32 os_git_status(void) { return -1; }
i32 os_git_progress(void) { return -1; }
const char *os_git_run(const char *folder, const char *args) { ns_unused(folder); ns_unused(args); return ""; }
const char *os_git_revision(const char *path) { ns_unused(path); return ""; }
const char *os_git_base(const char *path, i32 max_bytes) { ns_unused(path); ns_unused(max_bytes); return ""; }
const char *os_git_diff_text(const char *before, const char *after, i32 max_bytes) {
    ns_unused(before); ns_unused(after); ns_unused(max_bytes); return "";
}
const char *os_git_changes(const char *path) { ns_unused(path); return ""; }
const char *os_git_change_kinds(const char *path) { ns_unused(path); return ""; }
#endif

#ifndef NS_GIT_EMBEDDED
ns_bool os_git_embedded(void) { return false; }
ns_bool os_git_identity_import(const char *key, const char *passphrase) { ns_unused(key); ns_unused(passphrase); return false; }
ns_bool os_git_identity_delete(void) { return false; }
ns_bool os_git_identity_exists(void) { return false; }
ns_bool os_git_identity_generate(const char *comment) { ns_unused(comment); return false; }
const char *os_git_identity_public(void) { return ""; }
i32 os_ssh_open(const char *host, i32 port, const char *user, const char *folder, i32 cols, i32 rows) {
    ns_unused(host); ns_unused(port); ns_unused(user); ns_unused(folder); ns_unused(cols); ns_unused(rows); return 0;
}
i32 os_ssh_state(i32 handle) { ns_unused(handle); return -1; }
const char *os_ssh_error(i32 handle) { ns_unused(handle); return "SSH terminals are not available in this build"; }
const char *os_ssh_read(i32 handle, i32 capacity) { ns_unused(handle); ns_unused(capacity); return ""; }
i32 os_ssh_write(i32 handle, const char *data) { ns_unused(handle); ns_unused(data); return -1; }
i32 os_ssh_resize(i32 handle, i32 cols, i32 rows) { ns_unused(handle); ns_unused(cols); ns_unused(rows); return 0; }
void os_ssh_close(i32 handle) { ns_unused(handle); }
#endif
#if !(defined(__APPLE__) && TARGET_OS_IOS)
i32 os_dialog_file_begin(const char *title, i32 kind) { ns_unused(title); ns_unused(kind); return 0; }
i32 os_dialog_prompt_begin(const char *title, const char *fields, i32 secret_mask) { ns_unused(title); ns_unused(fields); ns_unused(secret_mask); return 0; }
void os_dialog_clear(void) {}
i32 os_dialog_poll(void) { return -1; }
const char *os_dialog_value(i32 index) { ns_unused(index); return ""; }
#endif
