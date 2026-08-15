#include "storage.internal.h"

#import <Foundation/Foundation.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define STORAGE_ERROR_CAPACITY 512

static char storage_error[STORAGE_ERROR_CAPACITY];
static char storage_root[STORAGE_PATH_CAPACITY];
static char storage_string[STORAGE_PATH_CAPACITY];
static NSString *storage_prefix;
static i32 storage_ready;

void storage_clear_error(void) {
    storage_error[0] = '\0';
}

void storage_set_error(const char *message) {
    snprintf(storage_error, sizeof(storage_error), "%s", message ? message : "storage error");
}

void storage_set_errorf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(storage_error, sizeof(storage_error), format, args);
    va_end(args);
}

const char *storage_last_error(void) {
    return storage_error;
}

i32 storage_make_dirs(const char *path) {
    @autoreleasepool {
        if (!path || !path[0]) return 0;
        NSString *value = [NSString stringWithUTF8String:path];
        if (!value) return 0;
        NSError *error = nil;
        if ([[NSFileManager defaultManager] createDirectoryAtPath:value
                                      withIntermediateDirectories:YES
                                                       attributes:nil
                                                            error:&error]) return 1;
        storage_set_error([[error localizedDescription] UTF8String]);
        return 0;
    }
}

const char *storage_app_data_dir(void) {
    if (!storage_ready) storage_set_error("storage_init must be called first");
    return storage_ready ? storage_root : "";
}

static NSString *storage_key(const char *key) {
    if (!storage_ready || !key || !key[0]) return nil;
    NSString *value = [NSString stringWithUTF8String:key];
    return value ? [storage_prefix stringByAppendingString:value] : nil;
}

static i32 storage_number_is_bool(NSNumber *value) {
    return value == (NSNumber *)kCFBooleanTrue || value == (NSNumber *)kCFBooleanFalse;
}

static i32 storage_number_is_float(NSNumber *value) {
    const char *type = [value objCType];
    return type && (type[0] == 'f' || type[0] == 'd');
}

i32 storage_init(const char *app_name) {
    @autoreleasepool {
        @synchronized([NSUserDefaults class]) {
            storage_clear_error();
            storage_ready = 0;
            const unsigned char *input = (const unsigned char *)(app_name && app_name[0] ? app_name : "ns");
            char safe[128];
            size_t length = 0;
            for (; *input && length + 1 < sizeof(safe); ++input) {
                unsigned char c = *input;
                safe[length++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ? (char)c : '_';
            }
            safe[length] = '\0';
            if (strcmp(safe, ".") == 0 || strcmp(safe, "..") == 0) snprintf(safe, sizeof(safe), "ns");
            NSString *name = [NSString stringWithUTF8String:safe];
            if (!name) {
                storage_set_error("application name is not valid UTF-8");
                return 0;
            }
#if !__has_feature(objc_arc)
            [storage_prefix release];
#endif
            storage_prefix = [[NSString alloc] initWithFormat:@"ns.storage.%@.", name];

            NSFileManager *manager = [NSFileManager defaultManager];
            NSURL *base = [[manager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask] firstObject];
            NSURL *directory = [base URLByAppendingPathComponent:name isDirectory:YES];
            NSError *error = nil;
            if (!directory || ![manager createDirectoryAtURL:directory
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:&error]) {
                storage_set_error(error ? [[error localizedDescription] UTF8String] : "could not create app data directory");
                return 0;
            }
            const char *path = [[directory path] fileSystemRepresentation];
            i32 count = snprintf(storage_root, sizeof(storage_root), "%s", path ? path : "");
            if (count < 0 || (size_t)count >= sizeof(storage_root)) {
                storage_set_error("application storage path is too long");
                return 0;
            }
            storage_ready = 1;
            return 1;
        }
    }
}

i32 storage_kv_has(const char *key) {
    @autoreleasepool {
        storage_clear_error();
        NSString *native_key = storage_key(key);
        return native_key && [[NSUserDefaults standardUserDefaults] objectForKey:native_key] != nil;
    }
}

i32 storage_kv_set_str(const char *key, const char *value) {
    @autoreleasepool {
        @synchronized([NSUserDefaults class]) {
            storage_clear_error();
            NSString *native_key = storage_key(key);
            NSString *native_value = value ? [NSString stringWithUTF8String:value] : nil;
            if (!native_key || !native_value) {
                storage_set_error("storage key or value is invalid");
                return 0;
            }
            [[NSUserDefaults standardUserDefaults] setObject:native_value forKey:native_key];
            return 1;
        }
    }
}

i32 storage_kv_set_i64(const char *key, i64 value) {
    @autoreleasepool {
        storage_clear_error();
        NSString *native_key = storage_key(key);
        if (!native_key) { storage_set_error("storage key is invalid"); return 0; }
        [[NSUserDefaults standardUserDefaults] setObject:[NSNumber numberWithLongLong:value] forKey:native_key];
        return 1;
    }
}

i32 storage_kv_set_f64(const char *key, f64 value) {
    @autoreleasepool {
        storage_clear_error();
        NSString *native_key = storage_key(key);
        if (!native_key) { storage_set_error("storage key is invalid"); return 0; }
        [[NSUserDefaults standardUserDefaults] setDouble:value forKey:native_key];
        return 1;
    }
}

i32 storage_kv_set_bool(const char *key, i32 value) {
    @autoreleasepool {
        storage_clear_error();
        NSString *native_key = storage_key(key);
        if (!native_key) { storage_set_error("storage key is invalid"); return 0; }
        [[NSUserDefaults standardUserDefaults] setBool:value != 0 forKey:native_key];
        return 1;
    }
}

const char *storage_kv_get_str(const char *key, const char *fallback) {
    @autoreleasepool {
        storage_clear_error();
        NSString *native_key = storage_key(key);
        id value = native_key ? [[NSUserDefaults standardUserDefaults] objectForKey:native_key] : nil;
        if (![value isKindOfClass:[NSString class]]) return fallback;
        const char *utf8 = [(NSString *)value UTF8String];
        snprintf(storage_string, sizeof(storage_string), "%s", utf8 ? utf8 : "");
        return storage_string;
    }
}

i64 storage_kv_get_i64(const char *key, i64 fallback) {
    @autoreleasepool {
        storage_clear_error();
        NSString *native_key = storage_key(key);
        id value = native_key ? [[NSUserDefaults standardUserDefaults] objectForKey:native_key] : nil;
        return [value isKindOfClass:[NSNumber class]] && !storage_number_is_bool(value) && !storage_number_is_float(value)
            ? [(NSNumber *)value longLongValue] : fallback;
    }
}

f64 storage_kv_get_f64(const char *key, f64 fallback) {
    @autoreleasepool {
        storage_clear_error();
        NSString *native_key = storage_key(key);
        id value = native_key ? [[NSUserDefaults standardUserDefaults] objectForKey:native_key] : nil;
        return [value isKindOfClass:[NSNumber class]] && storage_number_is_float(value)
            ? [(NSNumber *)value doubleValue] : fallback;
    }
}

i32 storage_kv_get_bool(const char *key, i32 fallback) {
    @autoreleasepool {
        storage_clear_error();
        NSString *native_key = storage_key(key);
        id value = native_key ? [[NSUserDefaults standardUserDefaults] objectForKey:native_key] : nil;
        return [value isKindOfClass:[NSNumber class]] && storage_number_is_bool(value)
            ? [(NSNumber *)value boolValue] : fallback;
    }
}

i32 storage_kv_remove(const char *key) {
    @autoreleasepool {
        storage_clear_error();
        NSString *native_key = storage_key(key);
        if (!native_key) { storage_set_error("storage key is invalid"); return 0; }
        [[NSUserDefaults standardUserDefaults] removeObjectForKey:native_key];
        return 1;
    }
}

i32 storage_kv_clear(void) {
    @autoreleasepool {
        storage_clear_error();
        if (!storage_ready) { storage_set_error("storage_init must be called first"); return 0; }
        NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
        NSDictionary *values = [defaults dictionaryRepresentation];
        for (NSString *key in values) {
            if ([key hasPrefix:storage_prefix]) [defaults removeObjectForKey:key];
        }
        return 1;
    }
}

i32 storage_kv_sync(void) {
    @autoreleasepool {
        storage_clear_error();
        if (!storage_ready) { storage_set_error("storage_init must be called first"); return 0; }
        return [[NSUserDefaults standardUserDefaults] synchronize];
    }
}
