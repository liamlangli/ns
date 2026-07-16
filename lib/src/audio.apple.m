#include "audio.h"

#include <TargetConditionals.h>
#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>

#define AUDIO_MAX_ASSETS 255
#define AUDIO_MAX_VOICES 64

#if __has_feature(objc_arc)
#define AUDIO_RELEASE(object) ((void)0)
#else
#define AUDIO_RELEASE(object) [(object) release]
#endif

typedef struct audio_asset {
    NSData *data;
    AVAudioPlayer *player;
    i32 kind;
    i32 generation;
    f64 volume;
} audio_asset;

typedef struct audio_voice {
    AVAudioPlayer *player;
    i32 handle;
} audio_voice;

static audio_asset audio_assets[AUDIO_MAX_ASSETS];
static audio_voice audio_voices[AUDIO_MAX_VOICES];
static NSObject *audio_mutex;
static f64 audio_master_volume = 1.0;
static char audio_error[512];

static NSObject *audio_lock(void) {
    @synchronized([NSObject class]) {
        if (!audio_mutex) audio_mutex = [[NSObject alloc] init];
    }
    return audio_mutex;
}

static f64 audio_clamp_volume(f64 volume) {
    if (volume < 0.0) return 0.0;
    if (volume > 1.0) return 1.0;
    return volume;
}

static void audio_clear_error(void) {
    audio_error[0] = '\0';
}

static void audio_set_error(NSString *message) {
    const char *utf8 = message ? [message UTF8String] : "unknown audio error";
    snprintf(audio_error, sizeof(audio_error), "%s", utf8 ? utf8 : "unknown audio error");
}

static i32 audio_make_handle(i32 index, i32 generation) {
    return (generation << 8) | (index + 1);
}

static audio_asset *audio_asset_for_handle(i32 handle) {
    if (handle <= 0) return NULL;
    i32 index = (handle & 0xff) - 1;
    i32 generation = handle >> 8;
    if (index < 0 || index >= AUDIO_MAX_ASSETS) return NULL;
    audio_asset *asset = &audio_assets[index];
    if (!asset->data || asset->generation != generation) return NULL;
    return asset;
}

static NSURL *audio_url_for_path(const char *path) {
    if (!path || !path[0]) return nil;
    NSString *string = [NSString stringWithUTF8String:path];
    if (!string) return nil;
    NSString *expanded = [string stringByExpandingTildeInPath];
    if ([[NSFileManager defaultManager] fileExistsAtPath:expanded]) {
        return [NSURL fileURLWithPath:expanded];
    }
    if (![expanded isAbsolutePath]) {
        NSURL *resource = [[NSBundle mainBundle] resourceURL];
        NSURL *bundled = [resource URLByAppendingPathComponent:expanded];
        if (bundled && [[NSFileManager defaultManager] fileExistsAtPath:[bundled path]]) return bundled;
    }
    return [NSURL fileURLWithPath:expanded];
}

static AVAudioPlayer *audio_new_player(NSData *data, NSError **error) {
    AVAudioPlayer *player = [[AVAudioPlayer alloc] initWithData:data error:error];
    if (player) [player prepareToPlay];
    return player;
}

static void audio_release_voice(i32 index) {
    [audio_voices[index].player stop];
    AUDIO_RELEASE(audio_voices[index].player);
    audio_voices[index] = (audio_voice){0};
}

static void audio_collect_voices(void) {
    for (i32 i = 0; i < AUDIO_MAX_VOICES; ++i) {
        AVAudioPlayer *player = audio_voices[i].player;
        if (player && ![player isPlaying] && [player currentTime] >= [player duration]) {
            audio_release_voice(i);
        }
    }
}

ns_bool audio_init(void) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            audio_clear_error();
#if TARGET_OS_IOS || (defined(TARGET_OS_VISION) && TARGET_OS_VISION)
            AVAudioSession *session = [AVAudioSession sharedInstance];
            NSError *error = nil;
            if (![session setCategory:AVAudioSessionCategoryAmbient
                                  mode:AVAudioSessionModeDefault
                               options:AVAudioSessionCategoryOptionMixWithOthers
                                 error:&error] ||
                ![session setActive:YES error:&error]) {
                audio_set_error([error localizedDescription]);
                return false;
            }
#endif
            return true;
        }
    }
}

void audio_shutdown(void) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            for (i32 i = 0; i < AUDIO_MAX_VOICES; ++i) {
                if (audio_voices[i].player) audio_release_voice(i);
            }
            for (i32 i = 0; i < AUDIO_MAX_ASSETS; ++i) {
                [audio_assets[i].player stop];
                AUDIO_RELEASE(audio_assets[i].player);
                AUDIO_RELEASE(audio_assets[i].data);
                audio_assets[i].player = nil;
                audio_assets[i].data = nil;
                audio_assets[i].kind = 0;
                audio_assets[i].volume = 0.0;
            }
            audio_clear_error();
#if TARGET_OS_IOS || (defined(TARGET_OS_VISION) && TARGET_OS_VISION)
            NSError *error = nil;
            if (![[AVAudioSession sharedInstance]
                    setActive:NO
                  withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                        error:&error]) {
                audio_set_error([error localizedDescription]);
            }
#endif
        }
    }
}

i32 audio_load(const char *path, i32 kind) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            audio_clear_error();
            if (kind != AUDIO_MUSIC && kind != AUDIO_SFX) {
                audio_set_error(@"audio kind must be AUDIO_MUSIC or AUDIO_SFX");
                return 0;
            }
            NSURL *url = audio_url_for_path(path);
            if (!url || ![[NSFileManager defaultManager] fileExistsAtPath:[url path]]) {
                audio_set_error([NSString stringWithFormat:@"audio file not found: %s", path ? path : ""]);
                return 0;
            }
            NSError *error = nil;
            NSData *data = [[NSData alloc] initWithContentsOfURL:url
                                                        options:NSDataReadingMappedIfSafe
                                                          error:&error];
            if (!data) {
                audio_set_error([error localizedDescription]);
                return 0;
            }
            AVAudioPlayer *probe = audio_new_player(data, &error);
            if (!probe) {
                AUDIO_RELEASE(data);
                audio_set_error([error localizedDescription]);
                return 0;
            }
            i32 index = -1;
            for (i32 i = 0; i < AUDIO_MAX_ASSETS; ++i) {
                if (!audio_assets[i].data) {
                    index = i;
                    break;
                }
            }
            if (index < 0) {
                AUDIO_RELEASE(probe);
                AUDIO_RELEASE(data);
                audio_set_error(@"audio asset limit reached");
                return 0;
            }
            audio_asset *asset = &audio_assets[index];
            asset->generation = (asset->generation + 1) & 0x7fffff;
            if (asset->generation == 0) asset->generation = 1;
            asset->data = data;
            asset->kind = kind;
            asset->volume = 1.0;
            if (kind == AUDIO_MUSIC) {
                asset->player = probe;
                [asset->player setVolume:(float)audio_master_volume];
            } else {
                asset->player = nil;
                AUDIO_RELEASE(probe);
            }
            return audio_make_handle(index, asset->generation);
        }
    }
}

i32 audio_load_music(const char *path) {
    return audio_load(path, AUDIO_MUSIC);
}

i32 audio_load_sfx(const char *path) {
    return audio_load(path, AUDIO_SFX);
}

void audio_unload(i32 handle) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            audio_asset *asset = audio_asset_for_handle(handle);
            if (!asset) return;
            for (i32 i = 0; i < AUDIO_MAX_VOICES; ++i) {
                if (audio_voices[i].player && audio_voices[i].handle == handle) audio_release_voice(i);
            }
            [asset->player stop];
            AUDIO_RELEASE(asset->player);
            AUDIO_RELEASE(asset->data);
            asset->player = nil;
            asset->data = nil;
            asset->kind = 0;
            asset->volume = 0.0;
            audio_clear_error();
        }
    }
}

ns_bool audio_play(i32 handle, ns_bool loop) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            audio_clear_error();
            audio_asset *asset = audio_asset_for_handle(handle);
            if (!asset) {
                audio_set_error(@"invalid audio handle");
                return false;
            }
            if (asset->kind == AUDIO_MUSIC) {
                [asset->player stop];
                [asset->player setCurrentTime:0.0];
                [asset->player setNumberOfLoops:loop ? -1 : 0];
                if (![asset->player play]) {
                    audio_set_error(@"audio playback could not start");
                    return false;
                }
                return true;
            }
            audio_collect_voices();
            i32 voice_index = -1;
            for (i32 i = 0; i < AUDIO_MAX_VOICES; ++i) {
                if (!audio_voices[i].player) {
                    voice_index = i;
                    break;
                }
            }
            if (voice_index < 0) {
                audio_set_error(@"simultaneous audio voice limit reached");
                return false;
            }
            NSError *error = nil;
            AVAudioPlayer *player = audio_new_player(asset->data, &error);
            if (!player) {
                audio_set_error([error localizedDescription]);
                return false;
            }
            [player setNumberOfLoops:loop ? -1 : 0];
            [player setVolume:(float)(asset->volume * audio_master_volume)];
            audio_voices[voice_index] = (audio_voice){.player = player, .handle = handle};
            if (![player play]) {
                audio_release_voice(voice_index);
                audio_set_error(@"audio playback could not start");
                return false;
            }
            return true;
        }
    }
}

void audio_pause(i32 handle) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            audio_asset *asset = audio_asset_for_handle(handle);
            if (!asset) return;
            if (asset->kind == AUDIO_MUSIC) [asset->player pause];
            for (i32 i = 0; i < AUDIO_MAX_VOICES; ++i) {
                if (audio_voices[i].handle == handle) [audio_voices[i].player pause];
            }
            audio_clear_error();
        }
    }
}

ns_bool audio_resume(i32 handle) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            audio_clear_error();
            audio_asset *asset = audio_asset_for_handle(handle);
            if (!asset) {
                audio_set_error(@"invalid audio handle");
                return false;
            }
            ns_bool resumed = false;
            if (asset->kind == AUDIO_MUSIC) {
                if (![asset->player play]) {
                    audio_set_error(@"audio playback could not resume");
                    return false;
                }
                return true;
            }
            for (i32 i = 0; i < AUDIO_MAX_VOICES; ++i) {
                if (audio_voices[i].player && audio_voices[i].handle == handle) {
                    resumed = [audio_voices[i].player play] || resumed;
                }
            }
            return resumed;
        }
    }
}

void audio_stop(i32 handle) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            audio_asset *asset = audio_asset_for_handle(handle);
            if (!asset) return;
            if (asset->kind == AUDIO_MUSIC) {
                [asset->player stop];
                [asset->player setCurrentTime:0.0];
            }
            for (i32 i = 0; i < AUDIO_MAX_VOICES; ++i) {
                if (audio_voices[i].player && audio_voices[i].handle == handle) audio_release_voice(i);
            }
            audio_clear_error();
        }
    }
}

void audio_stop_all(void) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            for (i32 i = 0; i < AUDIO_MAX_ASSETS; ++i) {
                if (audio_assets[i].player) {
                    [audio_assets[i].player stop];
                    [audio_assets[i].player setCurrentTime:0.0];
                }
            }
            for (i32 i = 0; i < AUDIO_MAX_VOICES; ++i) {
                if (audio_voices[i].player) audio_release_voice(i);
            }
            audio_clear_error();
        }
    }
}

void audio_set_volume(i32 handle, f64 volume) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            audio_asset *asset = audio_asset_for_handle(handle);
            if (!asset) return;
            asset->volume = audio_clamp_volume(volume);
            [asset->player setVolume:(float)(asset->volume * audio_master_volume)];
            for (i32 i = 0; i < AUDIO_MAX_VOICES; ++i) {
                if (audio_voices[i].player && audio_voices[i].handle == handle) {
                    [audio_voices[i].player setVolume:(float)(asset->volume * audio_master_volume)];
                }
            }
            audio_clear_error();
        }
    }
}

void audio_set_master_volume(f64 volume) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            audio_master_volume = audio_clamp_volume(volume);
            for (i32 i = 0; i < AUDIO_MAX_ASSETS; ++i) {
                audio_asset *asset = &audio_assets[i];
                if (asset->player) [asset->player setVolume:(float)(asset->volume * audio_master_volume)];
            }
            for (i32 i = 0; i < AUDIO_MAX_VOICES; ++i) {
                audio_asset *asset = audio_asset_for_handle(audio_voices[i].handle);
                if (audio_voices[i].player && asset) {
                    [audio_voices[i].player setVolume:(float)(asset->volume * audio_master_volume)];
                }
            }
            audio_clear_error();
        }
    }
}

ns_bool audio_is_playing(i32 handle) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            audio_asset *asset = audio_asset_for_handle(handle);
            if (!asset) return false;
            if (asset->player && [asset->player isPlaying]) return true;
            audio_collect_voices();
            for (i32 i = 0; i < AUDIO_MAX_VOICES; ++i) {
                if (audio_voices[i].handle == handle && [audio_voices[i].player isPlaying]) return true;
            }
            return false;
        }
    }
}

f64 audio_duration(i32 handle) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            audio_asset *asset = audio_asset_for_handle(handle);
            if (!asset) return 0.0;
            if (asset->player) return [asset->player duration];
            NSError *error = nil;
            AVAudioPlayer *player = audio_new_player(asset->data, &error);
            if (!player) return 0.0;
            f64 duration = [player duration];
            AUDIO_RELEASE(player);
            return duration;
        }
    }
}

f64 audio_position(i32 handle) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            audio_asset *asset = audio_asset_for_handle(handle);
            if (!asset) return 0.0;
            if (asset->player) return [asset->player currentTime];
            for (i32 i = 0; i < AUDIO_MAX_VOICES; ++i) {
                if (audio_voices[i].player && audio_voices[i].handle == handle) {
                    return [audio_voices[i].player currentTime];
                }
            }
            return 0.0;
        }
    }
}

ns_bool audio_seek(i32 handle, f64 seconds) {
    @autoreleasepool {
        @synchronized(audio_lock()) {
            audio_clear_error();
            audio_asset *asset = audio_asset_for_handle(handle);
            if (!asset) {
                audio_set_error(@"invalid audio handle");
                return false;
            }
            ns_bool found = false;
            if (asset->player) {
                f64 duration = [asset->player duration];
                [asset->player setCurrentTime:seconds < 0.0 ? 0.0 : (seconds > duration ? duration : seconds)];
                found = true;
            }
            for (i32 i = 0; i < AUDIO_MAX_VOICES; ++i) {
                AVAudioPlayer *player = audio_voices[i].player;
                if (player && audio_voices[i].handle == handle) {
                    f64 duration = [player duration];
                    [player setCurrentTime:seconds < 0.0 ? 0.0 : (seconds > duration ? duration : seconds)];
                    found = true;
                }
            }
            return found;
        }
    }
}

const char *audio_last_error(void) {
    return audio_error;
}
