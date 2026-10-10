#pragma once

#include "ns_type.h"

// Scriptable music synthesizer used by lib/sono.ns.
//
// A song is an opaque positive handle that owns a tempo, a list of tracks and
// the note events scheduled on them. Every track plays one built-in
// instrument (oscillators, keys, drums or game sound effects) through its own
// envelope, filter, drive, echo and reverb send. Rendering is offline and
// deterministic: the same song always produces the same samples, either into
// a caller-owned interleaved stereo f32 buffer or straight into a 16-bit WAV
// file that the audio module can load.
//
// Positions and lengths are in beats, pitches are MIDI note numbers (60 is
// middle C, fractions are allowed) and gains are normalized 0..1. Handles are
// not synchronized: build and render a song from one thread at a time.

enum {
    NS_SONO_SINE = 0,
    NS_SONO_SQUARE,
    NS_SONO_SAW,
    NS_SONO_TRIANGLE,
    NS_SONO_NOISE,
    NS_SONO_PLUCK,
    NS_SONO_KEYS,
    NS_SONO_BELL,
    NS_SONO_ORGAN,
    NS_SONO_PAD,
    NS_SONO_BASS,
    NS_SONO_LEAD,
    NS_SONO_KICK,
    NS_SONO_SNARE,
    NS_SONO_HIHAT,
    NS_SONO_OPEN_HAT,
    NS_SONO_CLAP,
    NS_SONO_TOM,
    NS_SONO_CRASH,
    NS_SONO_COIN,
    NS_SONO_JUMP,
    NS_SONO_LASER,
    NS_SONO_EXPLOSION,
    NS_SONO_HIT,
    NS_SONO_POWERUP,
    NS_SONO_BLIP,
    NS_SONO_INSTRUMENT_COUNT
};

// Songs. sample_rate is 8000..192000; bpm is beats (quarter notes) per minute.
i32 sono_new(i32 sample_rate, f64 bpm);
void sono_free(i32 song);
void sono_clear(i32 song);
void sono_set_bpm(i32 song, f64 bpm);
// 0 plays straight eighths; 1 delays every off-beat eighth to the last
// sixteenth. About 0.33 is a triplet shuffle.
void sono_set_swing(i32 song, f64 amount);
void sono_set_volume(i32 song, f64 volume);
// A positive length renders exactly that many beats and wraps every tail back
// onto the start, so the result loops seamlessly. 0 renders until the tails end.
void sono_set_loop(i32 song, f64 beats);
void sono_set_seed(i32 song, i32 seed);

// Tracks. Returns the track index (0, 1, ...) or -1.
i32 sono_track(i32 song, i32 instrument);
void sono_track_volume(i32 song, i32 track, f64 volume);
void sono_track_pan(i32 song, i32 track, f64 pan);
void sono_track_envelope(i32 song, i32 track, f64 attack, f64 decay, f64 sustain, f64 release);
void sono_track_transpose(i32 song, i32 track, f64 semitones);
void sono_track_filter(i32 song, i32 track, f64 cutoff_hz, f64 resonance);
void sono_track_drive(i32 song, i32 track, f64 amount);
void sono_track_vibrato(i32 song, i32 track, f64 rate_hz, f64 semitones);
void sono_track_echo(i32 song, i32 track, f64 beats, f64 feedback, f64 mix);
void sono_track_reverb(i32 song, i32 track, f64 amount);

// Events.
ns_bool sono_note(i32 song, i32 track, f64 beat, f64 beats, f64 pitch, f64 velocity);
ns_bool sono_slide(i32 song, i32 track, f64 beat, f64 beats, f64 from_pitch, f64 to_pitch, f64 velocity);
// Text notation and step grids; both return the beat after the last step, or
// -1 with sono_last_error() describing the problem.
f64 sono_play(i32 song, i32 track, f64 beat, const char *score);
f64 sono_steps(i32 song, i32 track, f64 beat, f64 step, const char *pattern, f64 pitch);
// "c4", "F#3", "bb2" -> MIDI number; -1 when the name is not a pitch.
f64 sono_pitch(const char *name);

// Rendering.
f64 sono_beats(i32 song);
f64 sono_seconds(i32 song);
i32 sono_frames(i32 song);
i32 sono_sample_rate(i32 song);
i32 sono_render(i32 song, f32 *out, i32 frames);
ns_bool sono_save_wav(i32 song, const char *path);

const char *sono_last_error(void);
