#pragma once

#include "ns_type.h"

typedef enum audio_kind {
    AUDIO_MUSIC = 1,
    AUDIO_SFX = 2,
} audio_kind;

ns_bool audio_init(void);
void audio_shutdown(void);

i32 audio_load(const char *path, i32 kind);
i32 audio_load_music(const char *path);
i32 audio_load_sfx(const char *path);
void audio_unload(i32 handle);

ns_bool audio_play(i32 handle, ns_bool loop);
void audio_pause(i32 handle);
ns_bool audio_resume(i32 handle);
void audio_stop(i32 handle);
void audio_stop_all(void);

void audio_set_volume(i32 handle, f64 volume);
void audio_set_master_volume(f64 volume);
ns_bool audio_is_playing(i32 handle);

f64 audio_duration(i32 handle);
f64 audio_position(i32 handle);
ns_bool audio_seek(i32 handle, f64 seconds);

const char *audio_last_error(void);
