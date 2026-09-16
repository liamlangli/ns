// Portable audio fallback.
//
// Platforms without a native audio backend (Linux, Windows) still resolve the
// `audio` module so a program that plays music on Apple hosts runs here with
// silent playback: every load fails, so callers guard on the handle they
// already check for a missing file.
#include "audio.h"

#include <string.h>

static const char *audio_stub_error = "audio playback is not available on this platform";

ns_bool audio_init(void) {
    return false;
}

void audio_shutdown(void) {}

i32 audio_load(const char *path, i32 kind) {
    ns_unused(path);
    ns_unused(kind);
    return 0;
}

i32 audio_load_music(const char *path) {
    ns_unused(path);
    return 0;
}

i32 audio_load_sfx(const char *path) {
    ns_unused(path);
    return 0;
}

void audio_unload(i32 handle) {
    ns_unused(handle);
}

ns_bool audio_play(i32 handle, ns_bool loop) {
    ns_unused(handle);
    ns_unused(loop);
    return false;
}

void audio_pause(i32 handle) {
    ns_unused(handle);
}

ns_bool audio_resume(i32 handle) {
    ns_unused(handle);
    return false;
}

void audio_stop(i32 handle) {
    ns_unused(handle);
}

void audio_stop_all(void) {}

void audio_set_volume(i32 handle, f64 volume) {
    ns_unused(handle);
    ns_unused(volume);
}

void audio_set_master_volume(f64 volume) {
    ns_unused(volume);
}

ns_bool audio_is_playing(i32 handle) {
    ns_unused(handle);
    return false;
}

f64 audio_duration(i32 handle) {
    ns_unused(handle);
    return 0.0;
}

f64 audio_position(i32 handle) {
    ns_unused(handle);
    return 0.0;
}

ns_bool audio_seek(i32 handle, f64 seconds) {
    ns_unused(handle);
    ns_unused(seconds);
    return false;
}

const char *audio_last_error(void) {
    return audio_stub_error;
}
