#include <TargetConditionals.h>
#if TARGET_OS_IOS || TARGET_OS_TV || (defined(TARGET_OS_VISION) && TARGET_OS_VISION)

#include "os.h"

// Sandboxed Apple mobile apps have no process launcher or synchronous native
// file panels. File access itself is provided by os.c inside the app sandbox;
// these host-oriented services report unavailable without aborting the VM.
void os_watch_stop(void) {}
i32 os_watch_start(const char *path) {
    ns_unused(path);
    return 0;
}
i32 os_watch_poll(void) { return 0; }

const char *os_open_file_dialog(const char *title) {
    ns_unused(title);
    return "";
}

const char *os_save_file_dialog(const char *title, const char *suggested_name) {
    ns_unused(title);
    ns_unused(suggested_name);
    return "";
}

const char *os_open_folder_dialog(const char *title) {
    ns_unused(title);
    return "";
}

i32 os_launch_ns_project(const char *folder, const char *entry) {
    ns_unused(folder);
    ns_unused(entry);
    return 0;
}


#if TARGET_OS_IOS && !TARGET_OS_TV && !TARGET_OS_VISION
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <Security/Security.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#ifdef NS_GIT_EMBEDDED
#include <libssh2.h>
#endif
// Dialog results are published once, under a lock, and polled by the ns frame.
// No synchronous dispatch onto a thread waiting for an ns callback is used.
static pthread_mutex_t os_dialog_lock = PTHREAD_MUTEX_INITIALIZER;
static int os_dialog_state = -1;
static char *os_dialog_values[4];
static _Thread_local char *os_dialog_result;
static id os_document_delegate;
static UIViewController *os_dialog_host(void) {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (scene.activationState != UISceneActivationStateForegroundActive || ![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow *window in ((UIWindowScene *)scene).windows) if (window.isKeyWindow) {
            UIViewController *host = window.rootViewController;
            while (host.presentedViewController) host = host.presentedViewController;
            return host;
        }
    }
    return nil;
}
static void os_secret_free(char *text) {
    if (text) { volatile char *p = text; size_t n = strlen(text); while (n--) *p++ = 0; free(text); }
}
static int os_dialog_start(void) {
    pthread_mutex_lock(&os_dialog_lock);
    if (os_dialog_state == 0) { pthread_mutex_unlock(&os_dialog_lock); return 0; }
    for (int i = 0; i < 4; ++i) { os_secret_free(os_dialog_values[i]); os_dialog_values[i] = NULL; }
    os_dialog_state = 0;
    pthread_mutex_unlock(&os_dialog_lock); return 1;
}
static void os_dialog_complete(NSArray<NSString *> *values) {
    pthread_mutex_lock(&os_dialog_lock);
    for (NSUInteger i = 0; i < values.count && i < 4; ++i) os_dialog_values[i] = strdup(values[i].UTF8String ?: "");
    os_dialog_state = values ? 1 : -1;
    pthread_mutex_unlock(&os_dialog_lock);
}
i32 os_dialog_poll(void) {
    pthread_mutex_lock(&os_dialog_lock); int state = os_dialog_state; pthread_mutex_unlock(&os_dialog_lock); return state;
}
const char *os_dialog_value(i32 index) {
    os_secret_free(os_dialog_result); os_dialog_result = NULL;
    pthread_mutex_lock(&os_dialog_lock);
    if (index >= 0 && index < 4 && os_dialog_values[index]) os_dialog_result = strdup(os_dialog_values[index]);
    pthread_mutex_unlock(&os_dialog_lock);
    return os_dialog_result ?: "";
}
void os_dialog_clear(void) {
    pthread_mutex_lock(&os_dialog_lock);
    if (os_dialog_state != 0) {
        for (int i = 0; i < 4; ++i) { os_secret_free(os_dialog_values[i]); os_dialog_values[i] = NULL; }
        os_dialog_state = -1;
    }
    pthread_mutex_unlock(&os_dialog_lock);
    os_secret_free(os_dialog_result); os_dialog_result = NULL;
}
@interface NSFileDialogDelegate : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) NSInteger kind;
@end
@implementation NSFileDialogDelegate
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    (void)controller; os_dialog_complete(nil);
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller;
    NSURL *url = urls.firstObject;
    if (!url) { os_dialog_complete(nil); return; }
    BOOL scoped = [url startAccessingSecurityScopedResource];
    NSInteger kind = self.kind;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] init];
            __block NSString *result = nil;
            [coordinator coordinateReadingItemAtURL:url options:0 error:nil byAccessor:^(NSURL *source) {
                if (kind == 2) {
                    NSNumber *size = nil;
                    [source getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
                    if (size && size.unsignedLongLongValue <= 65536) {
                        NSData *data = [NSData dataWithContentsOfURL:source];
                        if (data.length && data.length <= 65536) result = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
                    }
                } else {
                    // Imports land beside the app's other documents, so the
                    // copy is visible in Files; a taken name gets " 2", " 3"...
                    NSString *dir = [NSString stringWithUTF8String:os_documents_dir("ns_lens")];
                    NSString *name = source.lastPathComponent.stringByDeletingPathExtension;
                    NSString *extension = source.pathExtension;
                    NSFileManager *fm = NSFileManager.defaultManager;
                    for (int n = 1; n <= 99 && !result; ++n) {
                        NSString *leaf = n == 1 ? source.lastPathComponent : [NSString stringWithFormat:@"%@ %d", name, n];
                        if (n > 1 && extension.length) leaf = [leaf stringByAppendingPathExtension:extension];
                        NSString *destination = [dir stringByAppendingPathComponent:leaf];
                        if ([fm fileExistsAtPath:destination]) continue;
                        if ([fm copyItemAtURL:source toURL:[NSURL fileURLWithPath:destination] error:nil]) result = destination;
                        else break;
                    }
                }
            }];
            if (scoped) [url stopAccessingSecurityScopedResource];
            os_dialog_complete(result ? @[result] : nil);
#if !__has_feature(objc_arc)
            if (kind == 2) [result release];
            [coordinator release];
#endif
        }
    });
}
@end
i32 os_dialog_file_begin(const char *title, i32 kind) {
    if (kind < 0 || kind > 2 || !os_dialog_start()) return 0;
    NSString *heading = [NSString stringWithUTF8String:title ?: ""];
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *host = os_dialog_host();
        if (!host) { os_dialog_complete(nil); return; }
        UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
            initForOpeningContentTypes:@[kind == 1 ? UTTypeFolder : UTTypeItem] asCopy:NO];
        NSFileDialogDelegate *delegate = [[NSFileDialogDelegate alloc] init];
        delegate.kind = kind;
#if !__has_feature(objc_arc)
        [os_document_delegate release];
#endif
        os_document_delegate = delegate;
        picker.delegate = delegate; picker.title = heading; picker.allowsMultipleSelection = NO;
        [host presentViewController:picker animated:YES completion:nil];
#if !__has_feature(objc_arc)
        [picker release];
#endif
    });
    return 1;
}
i32 os_dialog_prompt_begin(const char *title, const char *fields, i32 secret_mask) {
    if (!os_dialog_start()) return 0;
    NSString *heading = [NSString stringWithUTF8String:title ?: ""];
    NSArray *labels = [[NSString stringWithUTF8String:fields ?: ""] componentsSeparatedByString:@"\n"];
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *host = os_dialog_host();
        if (!host) { os_dialog_complete(nil); return; }
        UIAlertController *prompt = [UIAlertController alertControllerWithTitle:heading message:nil preferredStyle:UIAlertControllerStyleAlert];
        for (NSUInteger i = 0; i < labels.count && i < 4; ++i) {
            [prompt addTextFieldWithConfigurationHandler:^(UITextField *field) {
                field.placeholder = labels[i]; field.secureTextEntry = (secret_mask & (1 << i)) != 0;
                field.autocorrectionType = UITextAutocorrectionTypeNo;
                field.autocapitalizationType = UITextAutocapitalizationTypeNone;
            }];
        }
        [prompt addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *action) { (void)action; os_dialog_complete(nil); }]];
        [prompt addAction:[UIAlertAction actionWithTitle:@"Continue" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
            (void)action; NSMutableArray *values = [NSMutableArray array];
            for (UITextField *field in prompt.textFields) { [values addObject:field.text ?: @""]; field.text = @""; }
            os_dialog_complete(values);
        }]];
        [host presentViewController:prompt animated:YES completion:nil];
    });
    return 1;
}
#ifdef NS_GIT_EMBEDDED
// One Keychain record makes replacement of key + passphrase atomic. Device-only
// accessibility prevents migration/backups and blocks access while locked.
static NSMutableDictionary *os_git_keychain_query(NSString *account) {
    NSString *service = [(NSBundle.mainBundle.bundleIdentifier ?: @"ns") stringByAppendingString:@".git"];
    return [@{(__bridge id)kSecClass:(__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService:service, (__bridge id)kSecAttrAccount:account} mutableCopy];
}
static NSData *os_git_keychain_read(NSString *account) {
    NSMutableDictionary *query = os_git_keychain_query(account);
    query[(__bridge id)kSecReturnData] = @YES;
    CFTypeRef result = NULL;
    OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
#if !__has_feature(objc_arc)
    [query release];
#endif
    if (status != errSecSuccess) return nil;
    NSData *data = [NSData dataWithData:(__bridge NSData *)result]; CFRelease(result); return data;
}
static BOOL os_git_keychain_write(NSString *account, NSData *data) {
    NSMutableDictionary *query = os_git_keychain_query(account);
    NSDictionary *attrs = @{(__bridge id)kSecValueData:data,
        (__bridge id)kSecAttrAccessible:(__bridge id)kSecAttrAccessibleWhenUnlockedThisDeviceOnly};
    OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)query, (__bridge CFDictionaryRef)attrs);
    if (status == errSecItemNotFound) { [query addEntriesFromDictionary:attrs]; status = SecItemAdd((__bridge CFDictionaryRef)query, NULL); }
#if !__has_feature(objc_arc)
    [query release];
#endif
    return status == errSecSuccess;
}
char *os_git_secret_copy(const char *name) {
    @autoreleasepool {
        NSData *record = os_git_keychain_read(@"identity");
        NSDictionary *values = record ? [NSJSONSerialization JSONObjectWithData:record options:0 error:nil] : nil;
        NSString *text = [values isKindOfClass:NSDictionary.class] ? values[[NSString stringWithUTF8String:name]] : nil;
        return [text isKindOfClass:NSString.class] ? strdup(text.UTF8String) : NULL;
    }
}
ns_bool os_git_identity_import(const char *key, const char *passphrase) {
    if (!key || !passphrase || strlen(key) > 65536 || strlen(passphrase) > 4096) return false;
    // Structural OpenSSH validation; PEM keys are also decrypted here. An
    // encrypted OpenSSH key's passphrase is checked during SSH authentication.
    extern int os_git_key_valid(const char *, const char *);
    if (!os_git_key_valid(key, passphrase)) return false;
    NSDictionary *values = @{@"private-key":[NSString stringWithUTF8String:key] ?: @"", @"passphrase":[NSString stringWithUTF8String:passphrase] ?: @""};
    NSData *data = [NSJSONSerialization dataWithJSONObject:values options:0 error:nil];
    return data && os_git_keychain_write(@"identity", data);
}
ns_bool os_git_identity_delete(void) {
    NSMutableDictionary *query = os_git_keychain_query(@"identity");
    OSStatus status = SecItemDelete((__bridge CFDictionaryRef)query);
#if !__has_feature(objc_arc)
    [query release];
#endif
    return status == errSecSuccess || status == errSecItemNotFound;
}
int os_git_trust_host(const char *endpoint, const char *sha256) {
    if (NSThread.isMainThread) return 0; // A network operation must run on a worker.
    @autoreleasepool {
        NSString *account = [@"host:" stringByAppendingString:[NSString stringWithUTF8String:endpoint]];
        NSData *expected = [[NSString stringWithUTF8String:sha256] dataUsingEncoding:NSUTF8StringEncoding];
        NSData *stored = os_git_keychain_read(account);
        if (stored) return [stored isEqualToData:expected]; // Never silently replace a pin.
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block BOOL trusted = NO;
        dispatch_async(dispatch_get_main_queue(), ^{
            UIViewController *host = os_dialog_host();
            if (!host) { dispatch_semaphore_signal(done); return; }
            NSString *message = [NSString stringWithFormat:@"%@\nFingerprint:\n%@\nVerify this fingerprint with the server before trusting it.", [NSString stringWithUTF8String:endpoint], [NSString stringWithUTF8String:sha256]];
            UIAlertController *prompt = [UIAlertController alertControllerWithTitle:@"Trust SSH host?" message:message preferredStyle:UIAlertControllerStyleAlert];
            [prompt addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *action) { (void)action; dispatch_semaphore_signal(done); }]];
            [prompt addAction:[UIAlertAction actionWithTitle:@"Trust" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) { (void)action; trusted = os_git_keychain_write(account, expected); dispatch_semaphore_signal(done); }]];
            [host presentViewController:prompt animated:YES completion:nil];
        });
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
#if !OS_OBJECT_USE_OBJC
        dispatch_release(done);
#endif
        return trusted;
    }
}
#endif
i32 os_share_file(const char *path) {
    if (!path || !path[0]) return 0;
    NSString *name = [NSString stringWithUTF8String:path];
    if (!name || ![[NSFileManager defaultManager] fileExistsAtPath:name]) return 0;
    NSURL *url = [NSURL fileURLWithPath:name];
    dispatch_async(dispatch_get_main_queue(), ^{
        UIWindow *window = nil;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if (scene.activationState != UISceneActivationStateForegroundActive || ![scene isKindOfClass:UIWindowScene.class]) continue;
            for (UIWindow *candidate in ((UIWindowScene *)scene).windows) {
                if (candidate.isKeyWindow) { window = candidate; break; }
            }
        }
        UIViewController *host = window.rootViewController;
        while (host.presentedViewController) host = host.presentedViewController;
        if (!host) return;
        UIActivityViewController *sheet = [[UIActivityViewController alloc] initWithActivityItems:@[url] applicationActivities:nil];
        // iPad requires an explicit popover anchor.
        sheet.popoverPresentationController.sourceView = host.view;
        sheet.popoverPresentationController.sourceRect = CGRectMake(CGRectGetMidX(host.view.bounds), CGRectGetMidY(host.view.bounds), 1, 1);
        [host presentViewController:sheet animated:YES completion:nil];
#if !__has_feature(objc_arc)
        [sheet release];
#endif
    });
    return 1;
}
#else
i32 os_share_file(const char *path) { ns_unused(path); return 0; }
#if TARGET_OS_IOS
i32 os_dialog_file_begin(const char *title, i32 kind) { ns_unused(title); ns_unused(kind); return 0; }
i32 os_dialog_prompt_begin(const char *title, const char *fields, i32 mask) { ns_unused(title); ns_unused(fields); ns_unused(mask); return 0; }
void os_dialog_clear(void) {}
i32 os_dialog_poll(void) { return -1; }
const char *os_dialog_value(i32 index) { ns_unused(index); return ""; }
#endif
#endif

#endif
