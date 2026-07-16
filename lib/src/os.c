#include "os.h"
#include "ns_os.h"

#include <locale.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

#ifdef NS_WIN
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
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

i32 os_write_file_atomic(const char *path, const char *text) {
    if (!path || !path[0] || !text) return 0;
    char temporary[OS_MAX_PATH];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary)) return 0;
    FILE *file = fopen(temporary, "wb");
    if (!file) return 0;
    size_t len = strlen(text);
    ns_bool ok = fwrite(text, 1, len, file) == len;
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

static i32 os_path_ignored(const char *relative, i32 is_dir) {
    if (strcmp(relative, ".git") == 0 || strncmp(relative, ".git/", 5) == 0) return 1;
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
    os_dir_scan_recursive(path, 0, -1);
    return os_entry_count;
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
