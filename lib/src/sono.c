// Scriptable music synthesizer behind lib/sono.ns. See lib/include/sono.h for
// the contract and doc/sono.md for the notation.
//
// A song stores plain data: a tempo, up to SONO_MAX_TRACKS tracks and a flat
// event list. Rendering walks the tracks one at a time: every event of the
// track is synthesized into one mono buffer, then the track strip (drive,
// low-pass, volume, pan, ping-pong echo) runs once over that buffer and adds
// it to the stereo master and the shared reverb send. The master result is
// cached until the song changes again.

#include "sono.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SONO_PI 3.14159265358979323846
#define SONO_MAX_SONGS 256
#define SONO_MAX_TRACKS 64
#define SONO_MAX_EVENTS (1 << 20)
#define SONO_MAX_SECONDS 600.0
#define SONO_MAX_DEPTH 16
#define SONO_ECHO_TAIL_MAX 12.0
#define SONO_REVERB_TAIL 2.5

typedef struct sono_event {
    f64 beat;
    f64 beats;
    f64 pitch;
    f64 pitch_to;
    f64 velocity;
    i32 track;
} sono_event;

typedef struct sono_strip {
    i32 instrument;
    f64 volume;
    f64 pan;
    f64 attack, decay, sustain, release;
    f64 transpose;
    f64 cutoff, resonance;
    f64 drive;
    f64 vibrato_rate, vibrato_depth;
    f64 echo_beats, echo_feedback, echo_mix;
    f64 reverb;
} sono_strip;

typedef struct sono_song {
    i32 rate;
    f64 bpm;
    f64 swing;
    f64 volume;
    f64 loop_beats;
    u32 seed;
    sono_strip tracks[SONO_MAX_TRACKS];
    i32 track_count;
    sono_event *events;
    i32 event_count;
    i32 event_cap;
    // Cached interleaved stereo render; valid while `dirty` is false.
    f32 *mix;
    i32 mix_frames;
    ns_bool dirty;
} sono_song;

// Per-instrument defaults. A one-shot instrument (drums, effects) ignores the
// note length and always sounds for `length` seconds.
typedef struct sono_preset {
    f64 attack, decay, sustain, release;
    ns_bool one_shot;
    f64 length;
    f64 vibrato_rate, vibrato_depth;
} sono_preset;

static const sono_preset sono_presets[NS_SONO_INSTRUMENT_COUNT] = {
    [NS_SONO_SINE] = {0.005, 0.1, 0.85, 0.08, false, 0, 0, 0},
    [NS_SONO_SQUARE] = {0.003, 0.1, 0.8, 0.05, false, 0, 0, 0},
    [NS_SONO_SAW] = {0.003, 0.1, 0.8, 0.05, false, 0, 0, 0},
    [NS_SONO_TRIANGLE] = {0.004, 0.1, 0.85, 0.06, false, 0, 0, 0},
    [NS_SONO_NOISE] = {0.002, 0.1, 0.7, 0.05, false, 0, 0, 0},
    [NS_SONO_PLUCK] = {0.001, 0.0, 1.0, 0.12, false, 0, 0, 0},
    [NS_SONO_KEYS] = {0.002, 0.0, 1.0, 0.35, false, 0, 0, 0},
    [NS_SONO_BELL] = {0.001, 0.0, 1.0, 1.4, false, 0, 0, 0},
    [NS_SONO_ORGAN] = {0.01, 0.0, 1.0, 0.08, false, 0, 0, 0},
    [NS_SONO_PAD] = {0.45, 0.4, 0.8, 0.9, false, 0, 0, 0},
    [NS_SONO_BASS] = {0.003, 0.25, 0.7, 0.06, false, 0, 0, 0},
    [NS_SONO_LEAD] = {0.01, 0.12, 0.8, 0.12, false, 0, 5.5, 0.15},
    [NS_SONO_KICK] = {0.0005, 0, 1, 0.01, true, 0.45, 0, 0},
    [NS_SONO_SNARE] = {0.0005, 0, 1, 0.01, true, 0.28, 0, 0},
    [NS_SONO_HIHAT] = {0.0005, 0, 1, 0.01, true, 0.08, 0, 0},
    [NS_SONO_OPEN_HAT] = {0.0005, 0, 1, 0.01, true, 0.5, 0, 0},
    [NS_SONO_CLAP] = {0.0005, 0, 1, 0.01, true, 0.3, 0, 0},
    [NS_SONO_TOM] = {0.0005, 0, 1, 0.01, true, 0.4, 0, 0},
    [NS_SONO_CRASH] = {0.0005, 0, 1, 0.01, true, 1.6, 0, 0},
    [NS_SONO_COIN] = {0.0005, 0, 1, 0.01, true, 0.35, 0, 0},
    [NS_SONO_JUMP] = {0.0005, 0, 1, 0.01, true, 0.22, 0, 0},
    [NS_SONO_LASER] = {0.0005, 0, 1, 0.01, true, 0.25, 0, 0},
    [NS_SONO_EXPLOSION] = {0.0005, 0, 1, 0.01, true, 1.0, 0, 0},
    [NS_SONO_HIT] = {0.0005, 0, 1, 0.01, true, 0.18, 0, 0},
    [NS_SONO_POWERUP] = {0.0005, 0, 1, 0.01, true, 0.6, 0, 0},
    [NS_SONO_BLIP] = {0.0005, 0, 1, 0.01, true, 0.07, 0, 0},
};

static sono_song *sono_songs[SONO_MAX_SONGS];
static char sono_error[256];

static void sono_fail(const char *message) {
    snprintf(sono_error, sizeof(sono_error), "%s", message);
}

static f64 sono_clamp(f64 v, f64 lo, f64 hi) {
    if (!(v >= lo)) return lo; // also catches NaN
    return v > hi ? hi : v;
}

static sono_song *sono_get(i32 song) {
    if (song <= 0 || song > SONO_MAX_SONGS || !sono_songs[song - 1]) {
        sono_fail("invalid song handle");
        return NULL;
    }
    return sono_songs[song - 1];
}

static sono_strip *sono_get_track(i32 song, i32 track) {
    sono_song *s = sono_get(song);
    if (!s) return NULL;
    if (track < 0 || track >= s->track_count) {
        sono_fail("invalid track index");
        return NULL;
    }
    s->dirty = true;
    return &s->tracks[track];
}

// ---- random numbers ---------------------------------------------------------

static u32 sono_hash(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x ? x : 0x9e3779b9u;
}

// White noise in [-1, 1).
static f64 sono_noise(u32 *state) {
    u32 x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return (f64)x / 2147483648.0 - 1.0;
}

// ---- songs ------------------------------------------------------------------

i32 sono_new(i32 sample_rate, f64 bpm) {
    if (sample_rate < 8000 || sample_rate > 192000) {
        sono_fail("sample rate must be 8000..192000");
        return 0;
    }
    if (!(bpm > 0)) {
        sono_fail("bpm must be positive");
        return 0;
    }
    for (i32 i = 0; i < SONO_MAX_SONGS; i++) {
        if (sono_songs[i]) continue;
        sono_song *s = calloc(1, sizeof(sono_song));
        if (!s) {
            sono_fail("out of memory");
            return 0;
        }
        s->rate = sample_rate;
        s->bpm = bpm;
        s->volume = 0.8;
        s->seed = 1;
        s->dirty = true;
        sono_songs[i] = s;
        return i + 1;
    }
    sono_fail("too many songs");
    return 0;
}

void sono_free(i32 song) {
    sono_song *s = sono_get(song);
    if (!s) return;
    free(s->events);
    free(s->mix);
    free(s);
    sono_songs[song - 1] = NULL;
}

void sono_clear(i32 song) {
    sono_song *s = sono_get(song);
    if (!s) return;
    s->event_count = 0;
    s->dirty = true;
}

void sono_set_bpm(i32 song, f64 bpm) {
    sono_song *s = sono_get(song);
    if (!s || !(bpm > 0)) return;
    s->bpm = bpm;
    s->dirty = true;
}

void sono_set_swing(i32 song, f64 amount) {
    sono_song *s = sono_get(song);
    if (!s) return;
    s->swing = sono_clamp(amount, 0, 1);
    s->dirty = true;
}

void sono_set_volume(i32 song, f64 volume) {
    sono_song *s = sono_get(song);
    if (!s) return;
    s->volume = sono_clamp(volume, 0, 4);
    s->dirty = true;
}

void sono_set_loop(i32 song, f64 beats) {
    sono_song *s = sono_get(song);
    if (!s) return;
    s->loop_beats = beats > 0 ? beats : 0;
    s->dirty = true;
}

void sono_set_seed(i32 song, i32 seed) {
    sono_song *s = sono_get(song);
    if (!s) return;
    s->seed = (u32)seed;
    s->dirty = true;
}

// ---- tracks -----------------------------------------------------------------

i32 sono_track(i32 song, i32 instrument) {
    sono_song *s = sono_get(song);
    if (!s) return -1;
    if (instrument < 0 || instrument >= NS_SONO_INSTRUMENT_COUNT) {
        sono_fail("unknown instrument");
        return -1;
    }
    if (s->track_count >= SONO_MAX_TRACKS) {
        sono_fail("too many tracks");
        return -1;
    }
    const sono_preset *p = &sono_presets[instrument];
    sono_strip *t = &s->tracks[s->track_count];
    memset(t, 0, sizeof(*t));
    t->instrument = instrument;
    t->volume = 0.7;
    t->attack = p->attack;
    t->decay = p->decay;
    t->sustain = p->sustain;
    t->release = p->release;
    t->vibrato_rate = p->vibrato_rate;
    t->vibrato_depth = p->vibrato_depth;
    s->dirty = true;
    return s->track_count++;
}

void sono_track_volume(i32 song, i32 track, f64 volume) {
    sono_strip *t = sono_get_track(song, track);
    if (t) t->volume = sono_clamp(volume, 0, 4);
}

void sono_track_pan(i32 song, i32 track, f64 pan) {
    sono_strip *t = sono_get_track(song, track);
    if (t) t->pan = sono_clamp(pan, -1, 1);
}

void sono_track_envelope(i32 song, i32 track, f64 attack, f64 decay, f64 sustain, f64 release) {
    sono_strip *t = sono_get_track(song, track);
    if (!t) return;
    t->attack = sono_clamp(attack, 0, 30);
    t->decay = sono_clamp(decay, 0, 30);
    t->sustain = sono_clamp(sustain, 0, 1);
    t->release = sono_clamp(release, 0, 30);
}

void sono_track_transpose(i32 song, i32 track, f64 semitones) {
    sono_strip *t = sono_get_track(song, track);
    if (t) t->transpose = sono_clamp(semitones, -96, 96);
}

void sono_track_filter(i32 song, i32 track, f64 cutoff_hz, f64 resonance) {
    sono_strip *t = sono_get_track(song, track);
    if (!t) return;
    t->cutoff = cutoff_hz > 0 ? cutoff_hz : 0;
    t->resonance = sono_clamp(resonance, 0, 1);
}

void sono_track_drive(i32 song, i32 track, f64 amount) {
    sono_strip *t = sono_get_track(song, track);
    if (t) t->drive = sono_clamp(amount, 0, 1);
}

void sono_track_vibrato(i32 song, i32 track, f64 rate_hz, f64 semitones) {
    sono_strip *t = sono_get_track(song, track);
    if (!t) return;
    t->vibrato_rate = sono_clamp(rate_hz, 0, 40);
    t->vibrato_depth = sono_clamp(semitones, 0, 12);
}

void sono_track_echo(i32 song, i32 track, f64 beats, f64 feedback, f64 mix) {
    sono_strip *t = sono_get_track(song, track);
    if (!t) return;
    t->echo_beats = beats > 0 ? beats : 0;
    t->echo_feedback = sono_clamp(feedback, 0, 0.95);
    t->echo_mix = sono_clamp(mix, 0, 1);
}

void sono_track_reverb(i32 song, i32 track, f64 amount) {
    sono_strip *t = sono_get_track(song, track);
    if (t) t->reverb = sono_clamp(amount, 0, 1);
}

// ---- events -----------------------------------------------------------------

static sono_event *sono_push(sono_song *s, i32 track, f64 beat, f64 beats, f64 pitch, f64 pitch_to, f64 velocity) {
    if (track < 0 || track >= s->track_count) {
        sono_fail("invalid track index");
        return NULL;
    }
    if (!(beat >= 0) || !(beats > 0) || !isfinite(beat) || !isfinite(beats) || !isfinite(pitch) ||
        !isfinite(pitch_to)) {
        sono_fail("note needs a beat >= 0, a positive length and a finite pitch");
        return NULL;
    }
    if (s->event_count >= SONO_MAX_EVENTS) {
        sono_fail("too many events");
        return NULL;
    }
    if (s->event_count == s->event_cap) {
        i32 cap = s->event_cap ? s->event_cap * 2 : 256;
        sono_event *events = realloc(s->events, (size_t)cap * sizeof(sono_event));
        if (!events) {
            sono_fail("out of memory");
            return NULL;
        }
        s->events = events;
        s->event_cap = cap;
    }
    sono_event *e = &s->events[s->event_count++];
    e->beat = beat;
    e->beats = beats;
    e->pitch = sono_clamp(pitch, -24, 151);
    e->pitch_to = sono_clamp(pitch_to, -24, 151);
    e->velocity = sono_clamp(velocity, 0, 1);
    e->track = track;
    s->dirty = true;
    return e;
}

ns_bool sono_note(i32 song, i32 track, f64 beat, f64 beats, f64 pitch, f64 velocity) {
    sono_song *s = sono_get(song);
    return s && sono_push(s, track, beat, beats, pitch, pitch, velocity) != NULL;
}

ns_bool sono_slide(i32 song, i32 track, f64 beat, f64 beats, f64 from_pitch, f64 to_pitch, f64 velocity) {
    sono_song *s = sono_get(song);
    return s && sono_push(s, track, beat, beats, from_pitch, to_pitch, velocity) != NULL;
}

// ---- notation ---------------------------------------------------------------

typedef struct sono_parser {
    sono_song *song;
    i32 track;
    const char *src;
    f64 cursor;
    f64 length;
    f64 velocity;
    i32 octave;
    i32 last_first; // first event of the previous note or chord, for `~`
    i32 last_count;
    i32 depth;
    ns_bool failed;
} sono_parser;

static void sono_parse_fail(sono_parser *p, i32 at, const char *what) {
    if (p->failed) return;
    p->failed = true;
    snprintf(sono_error, sizeof(sono_error), "sono_play: %s at offset %d", what, at);
}

static ns_bool sono_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == '|';
}

static ns_bool sono_is_digit(char c) {
    return c >= '0' && c <= '9';
}

// A decimal number such as 2, 0.5 or .25. Returns false when none is there.
static ns_bool sono_parse_number(const char *s, i32 *i, i32 end, f64 *out) {
    i32 at = *i;
    f64 v = 0;
    ns_bool any = false;
    while (at < end && sono_is_digit(s[at])) {
        v = v * 10 + (s[at] - '0');
        at++;
        any = true;
    }
    if (at < end && s[at] == '.') {
        f64 scale = 0.1;
        at++;
        while (at < end && sono_is_digit(s[at])) {
            v += (s[at] - '0') * scale;
            scale *= 0.1;
            at++;
            any = true;
        }
    }
    if (!any) return false;
    *i = at;
    *out = v;
    return true;
}

// `:len` or `:num/den`; sticky for the notes that follow.
static void sono_parse_length(sono_parser *p, i32 *i, i32 end) {
    if (*i >= end || p->src[*i] != ':') return;
    i32 at = *i + 1;
    f64 v = 0, den = 1;
    if (!sono_parse_number(p->src, &at, end, &v)) {
        sono_parse_fail(p, *i, "expected a length after ':'");
        return;
    }
    if (at < end && p->src[at] == '/') {
        at++;
        if (!sono_parse_number(p->src, &at, end, &den) || den <= 0) {
            sono_parse_fail(p, *i, "expected a denominator after '/'");
            return;
        }
    }
    if (!(v / den > 0)) {
        sono_parse_fail(p, *i, "length must be positive");
        return;
    }
    p->length = v / den;
    *i = at;
}

// Optional accent (`!` louder, `?` softer) and `*n` repeat count.
static f64 sono_parse_accent(const char *s, i32 *i, i32 end) {
    f64 scale = 1;
    while (*i < end && (s[*i] == '!' || s[*i] == '?')) {
        scale *= s[*i] == '!' ? 1.25 : 0.5;
        (*i)++;
    }
    return scale;
}

static i32 sono_parse_repeat(sono_parser *p, i32 *i, i32 end) {
    if (*i >= end || p->src[*i] != '*') return 1;
    i32 at = *i + 1;
    f64 n = 0;
    if (!sono_parse_number(p->src, &at, end, &n) || n < 1 || n > 4096 || n != floor(n)) {
        sono_parse_fail(p, *i, "expected a repeat count 1..4096 after '*'");
        return 1;
    }
    *i = at;
    return (i32)n;
}

// A pitch: a note letter with accidentals and an optional octave that applies
// to this note only, `x` for a drum hit (middle C), or `n<midi>`. Returns
// false when no pitch starts at *i.
static ns_bool sono_parse_pitch(const char *s, i32 *i, i32 end, i32 octave, f64 *out) {
    static const i32 steps[7] = {9, 11, 0, 2, 4, 5, 7}; // a b c d e f g
    i32 at = *i;
    if (at >= end) return false;
    char c = s[at];
    if (c == 'x' || c == 'X') {
        *out = 60;
        *i = at + 1;
        return true;
    }
    if (c == 'n') {
        at++;
        f64 v = 0;
        if (!sono_parse_number(s, &at, end, &v)) return false;
        *out = v;
        *i = at;
        return true;
    }
    char lower = (char)(c | 0x20);
    if (lower < 'a' || lower > 'g') return false;
    i32 semitone = steps[lower - 'a'];
    at++;
    while (at < end && (s[at] == '#' || s[at] == 'b')) {
        semitone += s[at] == '#' ? 1 : -1;
        at++;
    }
    if (at < end && sono_is_digit(s[at])) {
        i32 o = 0;
        while (at < end && sono_is_digit(s[at])) {
            o = o * 10 + (s[at] - '0');
            at++;
            if (o > 10) return false;
        }
        octave = o;
    }
    *out = (f64)((octave + 1) * 12 + semitone);
    *i = at;
    return true;
}

static i32 sono_find_close(const char *s, i32 open_at, i32 end, char open, char close) {
    i32 depth = 0;
    for (i32 i = open_at; i < end; i++) {
        if (s[i] == open) depth++;
        else if (s[i] == close && --depth == 0) return i;
    }
    return -1;
}

static void sono_emit(sono_parser *p, const f64 *pitches, i32 count, f64 accent, i32 repeat) {
    for (i32 r = 0; r < repeat && !p->failed; r++) {
        i32 first = p->song->event_count;
        for (i32 k = 0; k < count; k++) {
            if (!sono_push(p->song, p->track, p->cursor, p->length, pitches[k], pitches[k], p->velocity * accent)) {
                p->failed = true;
                return;
            }
        }
        p->last_first = first;
        p->last_count = count;
        p->cursor += p->length;
    }
}

static void sono_parse_range(sono_parser *p, i32 begin, i32 end) {
    const char *s = p->src;
    if (++p->depth > SONO_MAX_DEPTH) {
        sono_parse_fail(p, begin, "groups nest too deeply");
        return;
    }
    i32 i = begin;
    while (i < end && !p->failed) {
        char c = s[i];
        if (sono_is_space(c)) {
            i++;
        } else if (c == '(') {
            i32 close = sono_find_close(s, i, end, '(', ')');
            if (close < 0) {
                sono_parse_fail(p, i, "unclosed '('");
                break;
            }
            i32 after = close + 1;
            i32 repeat = sono_parse_repeat(p, &after, end);
            for (i32 r = 0; r < repeat && !p->failed; r++) sono_parse_range(p, i + 1, close);
            i = after;
        } else if (c == '[') {
            i32 close = sono_find_close(s, i, end, '[', ']');
            if (close < 0) {
                sono_parse_fail(p, i, "unclosed '['");
                break;
            }
            f64 pitches[16];
            i32 count = 0;
            i32 at = i + 1;
            while (at < close && !p->failed) {
                if (sono_is_space(s[at])) {
                    at++;
                    continue;
                }
                f64 pitch = 0;
                if (!sono_parse_pitch(s, &at, close, p->octave, &pitch)) {
                    sono_parse_fail(p, at, "expected a pitch inside '[...]'");
                    break;
                }
                if (count == 16) {
                    sono_parse_fail(p, at, "a chord holds at most 16 notes");
                    break;
                }
                pitches[count++] = pitch;
            }
            i = close + 1;
            sono_parse_length(p, &i, end);
            f64 accent = sono_parse_accent(s, &i, end);
            i32 repeat = sono_parse_repeat(p, &i, end);
            if (!p->failed && count > 0) sono_emit(p, pitches, count, accent, repeat);
        } else if (c == 'r') {
            i++;
            sono_parse_length(p, &i, end);
            i32 repeat = sono_parse_repeat(p, &i, end);
            p->cursor += p->length * repeat;
            p->last_count = 0;
        } else if (c == '~') {
            i++;
            sono_parse_length(p, &i, end);
            for (i32 k = 0; k < p->last_count; k++) p->song->events[p->last_first + k].beats += p->length;
            p->cursor += p->length;
        } else if (c == ':') {
            sono_parse_length(p, &i, end);
        } else if (c == 'o') {
            i32 at = i + 1;
            f64 o = 0;
            if (!sono_parse_number(s, &at, end, &o) || o > 10 || o != floor(o)) {
                sono_parse_fail(p, i, "expected an octave 0..10 after 'o'");
                break;
            }
            p->octave = (i32)o;
            i = at;
        } else if (c == '>' || c == '<') {
            p->octave += c == '>' ? 1 : -1;
            if (p->octave < 0 || p->octave > 10) {
                sono_parse_fail(p, i, "octave out of range");
                break;
            }
            i++;
        } else if (c == 'v') {
            i32 at = i + 1;
            f64 v = 0;
            if (!sono_parse_number(s, &at, end, &v)) {
                sono_parse_fail(p, i, "expected a velocity after 'v'");
                break;
            }
            p->velocity = sono_clamp(v, 0, 1);
            i = at;
        } else {
            f64 pitch = 0;
            i32 at = i;
            if (!sono_parse_pitch(s, &at, end, p->octave, &pitch)) {
                char what[48];
                snprintf(what, sizeof(what), "unexpected '%c'", c);
                sono_parse_fail(p, i, what);
                break;
            }
            i = at;
            sono_parse_length(p, &i, end);
            f64 accent = sono_parse_accent(s, &i, end);
            i32 repeat = sono_parse_repeat(p, &i, end);
            if (!p->failed) sono_emit(p, &pitch, 1, accent, repeat);
        }
    }
    p->depth--;
}

f64 sono_play(i32 song, i32 track, f64 beat, const char *score) {
    sono_song *s = sono_get(song);
    if (!s) return -1;
    if (track < 0 || track >= s->track_count) {
        sono_fail("invalid track index");
        return -1;
    }
    if (!score || !(beat >= 0)) {
        sono_fail("sono_play needs a score and a beat >= 0");
        return -1;
    }
    sono_parser p = {0};
    p.song = s;
    p.track = track;
    p.src = score;
    p.cursor = beat;
    p.length = 1;
    p.velocity = 0.8;
    p.octave = 4;
    i32 rollback = s->event_count;
    sono_parse_range(&p, 0, (i32)strlen(score));
    if (p.failed) {
        s->event_count = rollback; // a bad score adds nothing
        return -1;
    }
    s->dirty = true;
    return p.cursor;
}

f64 sono_steps(i32 song, i32 track, f64 beat, f64 step, const char *pattern, f64 pitch) {
    sono_song *s = sono_get(song);
    if (!s) return -1;
    if (track < 0 || track >= s->track_count) {
        sono_fail("invalid track index");
        return -1;
    }
    if (!pattern || !(beat >= 0) || !(step > 0)) {
        sono_fail("sono_steps needs a pattern, a beat >= 0 and a positive step");
        return -1;
    }
    i32 rollback = s->event_count;
    f64 cursor = beat;
    for (i32 i = 0; pattern[i]; i++) {
        char c = pattern[i];
        if (sono_is_space(c)) continue;
        f64 velocity = c == 'X' ? 1.0 : c == 'x' ? 0.8 : c == 'o' ? 0.45 : -1;
        if (velocity < 0 && c != '.' && c != '-' && c != '_') {
            snprintf(sono_error, sizeof(sono_error), "sono_steps: unexpected '%c' at offset %d", c, i);
            s->event_count = rollback;
            return -1;
        }
        if (velocity > 0 && !sono_push(s, track, cursor, step, pitch, pitch, velocity)) {
            s->event_count = rollback;
            return -1;
        }
        cursor += step;
    }
    return cursor;
}

f64 sono_pitch(const char *name) {
    if (!name) return -1;
    i32 end = (i32)strlen(name);
    i32 begin = 0;
    while (begin < end && sono_is_space(name[begin])) begin++;
    while (end > begin && sono_is_space(name[end - 1])) end--;
    i32 at = begin;
    f64 pitch = 0;
    if (!sono_parse_pitch(name, &at, end, 4, &pitch) || at != end) return -1;
    return pitch;
}

// ---- time -------------------------------------------------------------------

// Swing moves every off-beat eighth later; the two halves of a beat stretch
// and shrink linearly around it so note order never changes.
static f64 sono_swing_beat(f64 beat, f64 swing) {
    if (swing <= 0) return beat;
    f64 whole = floor(beat);
    f64 x = beat - whole;
    f64 split = 0.5 + 0.25 * swing;
    x = x < 0.5 ? x * (split / 0.5) : split + (x - 0.5) * ((1 - split) / 0.5);
    return whole + x;
}

static f64 sono_beat_seconds(const sono_song *s, f64 beat) {
    return sono_swing_beat(beat, s->swing) * 60.0 / s->bpm;
}

static f64 sono_event_gate(const sono_song *s, const sono_event *e) {
    const sono_preset *p = &sono_presets[s->tracks[e->track].instrument];
    if (p->one_shot) return p->length;
    f64 gate = sono_beat_seconds(s, e->beat + e->beats) - sono_beat_seconds(s, e->beat);
    return gate > 0.002 ? gate : 0.002;
}

static f64 sono_echo_tail(const sono_song *s, const sono_strip *t) {
    if (t->echo_beats <= 0 || t->echo_mix <= 0) return 0;
    f64 delay = t->echo_beats * 60.0 / s->bpm;
    f64 repeats = t->echo_feedback > 0.001 ? ceil(log(0.001) / log(t->echo_feedback)) : 1;
    f64 tail = delay * (repeats + 1);
    return tail < SONO_ECHO_TAIL_MAX ? tail : SONO_ECHO_TAIL_MAX;
}

f64 sono_beats(i32 song) {
    sono_song *s = sono_get(song);
    if (!s) return 0;
    f64 end = 0;
    for (i32 i = 0; i < s->event_count; i++) {
        f64 e = s->events[i].beat + s->events[i].beats;
        if (e > end) end = e;
    }
    return end;
}

// Seconds until every voice, echo and reverb tail has died away.
static f64 sono_natural_seconds(const sono_song *s) {
    f64 end = 0;
    for (i32 i = 0; i < s->event_count; i++) {
        const sono_event *e = &s->events[i];
        f64 stop = sono_beat_seconds(s, e->beat) + sono_event_gate(s, e) + s->tracks[e->track].release;
        if (stop > end) end = stop;
    }
    f64 echo = 0;
    ns_bool reverb = false;
    for (i32 t = 0; t < s->track_count; t++) {
        f64 tail = sono_echo_tail(s, &s->tracks[t]);
        if (tail > echo) echo = tail;
        if (s->tracks[t].reverb > 0) reverb = true;
    }
    end += echo + (reverb ? SONO_REVERB_TAIL : 0);
    return end < SONO_MAX_SECONDS ? end : SONO_MAX_SECONDS;
}

static i32 sono_loop_frames(const sono_song *s) {
    f64 seconds = s->loop_beats * 60.0 / s->bpm;
    if (seconds > SONO_MAX_SECONDS) seconds = SONO_MAX_SECONDS;
    return (i32)ceil(seconds * s->rate);
}

f64 sono_seconds(i32 song) {
    sono_song *s = sono_get(song);
    if (!s) return 0;
    if (s->loop_beats > 0) return (f64)sono_loop_frames(s) / s->rate;
    return sono_natural_seconds(s);
}

i32 sono_frames(i32 song) {
    sono_song *s = sono_get(song);
    if (!s) return 0;
    if (s->loop_beats > 0) return sono_loop_frames(s);
    return (i32)ceil(sono_natural_seconds(s) * s->rate);
}

i32 sono_sample_rate(i32 song) {
    sono_song *s = sono_get(song);
    return s ? s->rate : 0;
}

// ---- dsp --------------------------------------------------------------------

// Topology-preserving state-variable filter (Simper). resonance 0..1.
typedef struct sono_svf {
    f64 ic1, ic2;
} sono_svf;

typedef struct sono_svf_out {
    f64 low, band, high;
} sono_svf_out;

static sono_svf_out sono_svf_run(sono_svf *f, f64 x, f64 cutoff, f64 resonance, f64 rate) {
    f64 hz = sono_clamp(cutoff, 10, rate * 0.45);
    f64 g = tan(SONO_PI * hz / rate);
    f64 k = 2.0 - 1.94 * sono_clamp(resonance, 0, 1);
    f64 a1 = 1.0 / (1.0 + g * (g + k));
    f64 a2 = g * a1;
    f64 a3 = g * a2;
    f64 v3 = x - f->ic2;
    f64 v1 = a1 * f->ic1 + a2 * v3;
    f64 v2 = f->ic2 + a2 * f->ic1 + a3 * v3;
    f->ic1 = 2 * v1 - f->ic1;
    f->ic2 = 2 * v2 - f->ic2;
    sono_svf_out out = {v2, v1, x - k * v1 - v2};
    return out;
}

// PolyBLEP residual that rounds the step of a naive saw or pulse.
static f64 sono_blep(f64 t, f64 dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0;
}

static f64 sono_saw(f64 phase, f64 dt) {
    return 2.0 * phase - 1.0 - sono_blep(phase, dt);
}

// Band-limited pulse with its DC offset removed, so narrow widths stay centred.
static f64 sono_pulse(f64 phase, f64 dt, f64 width) {
    f64 v = (phase < width ? 1.0 : -1.0) - (2.0 * width - 1.0);
    f64 shifted = phase + 1.0 - width;
    shifted -= floor(shifted);
    return v + sono_blep(phase, dt) - sono_blep(shifted, dt);
}

static f64 sono_wrap(f64 phase) {
    return phase - floor(phase);
}

static f64 sono_midi_hz(f64 pitch) {
    return 440.0 * pow(2.0, (pitch - 69.0) / 12.0);
}

typedef struct sono_voice {
    f64 phase[6];
    sono_svf filter[1];
    u32 rng;
    f32 *ks; // Karplus-Strong delay line for the pluck
    i32 ks_len;
    i32 ks_at;
    f64 ks_prev;
    f64 ks_tune; // all-pass coefficient for the fractional part of the period
    f64 ks_x1, ks_y1;
} sono_voice;

// Metallic cluster of square partials used by the cymbals.
static f64 sono_metal(sono_voice *v, f64 ratio, f64 rate) {
    static const f64 partials[6] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
    f64 sum = 0;
    for (i32 k = 0; k < 6; k++) {
        v->phase[k] = sono_wrap(v->phase[k] + partials[k] * 1.7 * ratio / rate);
        sum += v->phase[k] < 0.5 ? 1.0 : -1.0;
    }
    return sum / 6.0;
}

// One sample of `instrument` at voice time t seconds and pitch `pitch`.
static f64 sono_instrument(i32 instrument, sono_voice *v, f64 t, f64 pitch, f64 rate) {
    f64 hz = sono_midi_hz(pitch);
    f64 dt = hz / rate;
    if (dt > 0.49) dt = 0.49;
    f64 tune = pow(2.0, (pitch - 60.0) / 12.0); // drums and effects
    switch (instrument) {
    case NS_SONO_SINE:
        v->phase[0] = sono_wrap(v->phase[0] + dt);
        return 0.8 * sin(2 * SONO_PI * v->phase[0]);
    case NS_SONO_SQUARE:
        v->phase[0] = sono_wrap(v->phase[0] + dt);
        return 0.45 * sono_pulse(v->phase[0], dt, 0.5);
    case NS_SONO_SAW:
        v->phase[0] = sono_wrap(v->phase[0] + dt);
        return 0.5 * sono_saw(v->phase[0], dt);
    case NS_SONO_TRIANGLE:
        v->phase[0] = sono_wrap(v->phase[0] + dt);
        return 0.8 * (4.0 * fabs(v->phase[0] - 0.5) - 1.0);
    case NS_SONO_NOISE: {
        sono_svf_out f = sono_svf_run(&v->filter[0], sono_noise(&v->rng), hz * 8, 0.1, rate);
        return 0.6 * f.low;
    }
    case NS_SONO_PLUCK: {
        if (!v->ks) return 0;
        // The loop is the delay line, a two-tap average (half a sample) and
        // a first-order all-pass that supplies the rest of the period.
        f64 out = v->ks[v->ks_at];
        f64 low = 0.4985 * (out + v->ks_prev);
        v->ks_prev = out;
        f64 tuned = v->ks_tune * low + v->ks_x1 - v->ks_tune * v->ks_y1;
        v->ks_x1 = low;
        v->ks_y1 = tuned;
        v->ks[v->ks_at] = (f32)tuned;
        if (++v->ks_at == v->ks_len) v->ks_at = 0;
        return 0.9 * out;
    }
    case NS_SONO_KEYS: {
        v->phase[0] = sono_wrap(v->phase[0] + dt);
        v->phase[1] = sono_wrap(v->phase[1] + dt);
        v->phase[2] = sono_wrap(v->phase[2] + dt * 14.0);
        f64 index = 1.4 * exp(-t * 3.0) + 0.15;
        f64 body = sin(2 * SONO_PI * v->phase[0] + index * sin(2 * SONO_PI * v->phase[1]));
        f64 tine = 0.12 * exp(-t * 18.0) * sin(2 * SONO_PI * v->phase[2]);
        return 0.6 * exp(-t * 0.9) * (body + tine);
    }
    case NS_SONO_BELL: {
        v->phase[0] = sono_wrap(v->phase[0] + dt);
        v->phase[1] = sono_wrap(v->phase[1] + dt * 3.5);
        f64 index = 3.5 * exp(-t * 1.6) + 0.3;
        return 0.5 * exp(-t * 0.8) * sin(2 * SONO_PI * v->phase[0] + index * sin(2 * SONO_PI * v->phase[1]));
    }
    case NS_SONO_ORGAN: {
        static const f64 harmonics[6] = {1, 2, 3, 4, 6, 8};
        static const f64 weights[6] = {1.0, 0.6, 0.4, 0.25, 0.15, 0.1};
        v->phase[0] = sono_wrap(v->phase[0] + dt);
        f64 sum = 0;
        for (i32 k = 0; k < 6; k++) {
            if (dt * harmonics[k] < 0.45) sum += weights[k] * sin(2 * SONO_PI * harmonics[k] * v->phase[0]);
        }
        return 0.3 * sum;
    }
    case NS_SONO_PAD: {
        static const f64 detune[3] = {0.9942, 1.0, 1.0058};
        f64 sum = 0;
        for (i32 k = 0; k < 3; k++) {
            f64 d = dt * detune[k];
            v->phase[k] = sono_wrap(v->phase[k] + d);
            sum += sono_saw(v->phase[k], d);
        }
        sono_svf_out f = sono_svf_run(&v->filter[0], sum, 900 + 700 * sin(2 * SONO_PI * 0.3 * t) + hz * 2, 0.2, rate);
        return 0.3 * f.low;
    }
    case NS_SONO_BASS: {
        v->phase[0] = sono_wrap(v->phase[0] + dt);
        v->phase[1] = sono_wrap(v->phase[1] + dt * 0.5);
        f64 x = 0.6 * sono_saw(v->phase[0], dt) + 0.5 * sin(2 * SONO_PI * v->phase[1]);
        sono_svf_out f = sono_svf_run(&v->filter[0], x, 180 + 2400 * exp(-t * 14.0), 0.35, rate);
        return 0.75 * f.low;
    }
    case NS_SONO_LEAD: {
        v->phase[0] = sono_wrap(v->phase[0] + dt);
        v->phase[1] = sono_wrap(v->phase[1] + dt * 1.003);
        f64 x = 0.5 * sono_pulse(v->phase[0], dt, 0.25) + 0.3 * sono_saw(v->phase[1], dt);
        sono_svf_out f = sono_svf_run(&v->filter[0], x, 5200, 0.15, rate);
        return 0.6 * f.low;
    }
    case NS_SONO_KICK: {
        f64 f = (45.0 + 110.0 * exp(-t * 30.0)) * tune;
        v->phase[0] = sono_wrap(v->phase[0] + f / rate);
        f64 click = 0.3 * exp(-t * 400.0) * sono_noise(&v->rng);
        return exp(-t * 7.0) * sin(2 * SONO_PI * v->phase[0]) + click;
    }
    case NS_SONO_SNARE: {
        v->phase[0] = sono_wrap(v->phase[0] + 185.0 * tune / rate);
        f64 tone = 0.45 * exp(-t * 22.0) * sin(2 * SONO_PI * v->phase[0]);
        sono_svf_out f = sono_svf_run(&v->filter[0], sono_noise(&v->rng), 1500 * tune, 0.1, rate);
        return tone + 0.75 * exp(-t * 15.0) * f.high;
    }
    case NS_SONO_HIHAT:
    case NS_SONO_OPEN_HAT:
    case NS_SONO_CRASH: {
        f64 decay = instrument == NS_SONO_HIHAT ? 55.0 : instrument == NS_SONO_OPEN_HAT ? 7.0 : 2.4;
        f64 x = 0.5 * sono_metal(v, tune, rate) + 0.6 * sono_noise(&v->rng);
        sono_svf_out f = sono_svf_run(&v->filter[0], x, (instrument == NS_SONO_CRASH ? 4500 : 7000) * tune, 0.2, rate);
        return 0.5 * exp(-t * decay) * f.high;
    }
    case NS_SONO_CLAP: {
        f64 env = 0;
        for (i32 k = 0; k < 3; k++) {
            f64 at = t - k * 0.011;
            if (at >= 0 && at < 0.011) env += exp(-at * 160.0);
        }
        if (t >= 0.033) env += 0.8 * exp(-(t - 0.033) * 18.0);
        sono_svf_out f = sono_svf_run(&v->filter[0], sono_noise(&v->rng), 1200 * tune, 0.45, rate);
        return 0.9 * env * f.band;
    }
    case NS_SONO_TOM: {
        f64 f = (110.0 + 70.0 * exp(-t * 14.0)) * tune;
        v->phase[0] = sono_wrap(v->phase[0] + f / rate);
        return 0.9 * exp(-t * 8.0) * sin(2 * SONO_PI * v->phase[0]) + 0.1 * exp(-t * 60.0) * sono_noise(&v->rng);
    }
    case NS_SONO_COIN: {
        f64 f = (t < 0.07 ? 987.77 : 1318.51) * tune;
        f64 d = f / rate;
        v->phase[0] = sono_wrap(v->phase[0] + d);
        f64 env = t < 0.07 ? 1.0 : exp(-(t - 0.07) * 9.0);
        return 0.7 * env * sono_pulse(v->phase[0], d, 0.5);
    }
    case NS_SONO_JUMP: {
        f64 f = 280.0 * tune * (1.0 + 2.2 * t / 0.22);
        f64 d = f / rate;
        v->phase[0] = sono_wrap(v->phase[0] + d);
        return 0.7 * (1.0 - t / 0.22) * sono_pulse(v->phase[0], d, 0.25);
    }
    case NS_SONO_LASER: {
        f64 f = (1700.0 * exp(-t * 13.0) + 110.0) * tune;
        f64 d = sono_clamp(f / rate, 0, 0.45);
        v->phase[0] = sono_wrap(v->phase[0] + d);
        return 0.7 * (1.0 - t / 0.25) * sono_pulse(v->phase[0], d, 0.5);
    }
    case NS_SONO_EXPLOSION: {
        sono_svf_out f = sono_svf_run(&v->filter[0], sono_noise(&v->rng), (3200.0 * exp(-t * 4.5) + 90.0) * tune, 0.25, rate);
        v->phase[0] = sono_wrap(v->phase[0] + 48.0 * tune / rate);
        f64 rumble = 0.5 * exp(-t * 4.0) * sin(2 * SONO_PI * v->phase[0]);
        return exp(-t * 3.2) * (1.2 * f.low + rumble);
    }
    case NS_SONO_HIT: {
        f64 f = 240.0 * tune * exp(-t * 8.0);
        f64 d = f / rate;
        v->phase[0] = sono_wrap(v->phase[0] + d);
        return 0.9 * exp(-t * 30.0) * sono_noise(&v->rng) + 0.6 * exp(-t * 14.0) * sono_pulse(v->phase[0], d, 0.5);
    }
    case NS_SONO_POWERUP: {
        f64 step = floor(t / 0.06);
        f64 f = 392.0 * tune * pow(2.0, step * 4.0 / 12.0) * (1.0 + 0.03 * sin(2 * SONO_PI * 28.0 * t));
        f64 d = sono_clamp(f / rate, 0, 0.45);
        v->phase[0] = sono_wrap(v->phase[0] + d);
        return 0.6 * (1.0 - t / 0.6) * sono_pulse(v->phase[0], d, 0.5);
    }
    case NS_SONO_BLIP: {
        f64 d = sono_clamp(880.0 * tune / rate, 0, 0.45);
        v->phase[0] = sono_wrap(v->phase[0] + d);
        return 0.6 * (1.0 - t / 0.07) * sono_pulse(v->phase[0], d, 0.5);
    }
    default:
        return 0;
    }
}

static f64 sono_envelope(const sono_strip *t, f64 time, f64 gate) {
    f64 level;
    f64 at = time < gate ? time : gate;
    if (at < t->attack) level = at / t->attack;
    else if (at < t->attack + t->decay) level = 1.0 - (1.0 - t->sustain) * (at - t->attack) / t->decay;
    else level = t->sustain;
    if (time <= gate) return level;
    if (t->release <= 0) return 0;
    f64 x = 1.0 - (time - gate) / t->release;
    return x > 0 ? level * x * x : 0;
}

// Adds one event's voice to the track's mono buffer of `frames` frames.
static void sono_render_voice(const sono_song *s, const sono_strip *t, const sono_event *e, u32 index, f32 *mono,
                              i32 frames) {
    f64 rate = s->rate;
    f64 start = sono_beat_seconds(s, e->beat);
    f64 gate = sono_event_gate(s, e);
    f64 length = gate + t->release;
    i32 first = (i32)llround(start * rate);
    i32 count = (i32)ceil(length * rate);
    if (first >= frames) return;
    if (first + count > frames) count = frames - first;

    sono_voice v;
    memset(&v, 0, sizeof(v));
    v.rng = sono_hash(s->seed * 0x9e3779b9u + index * 0x85ebca6bu + 1u);
    if (t->instrument == NS_SONO_PLUCK) {
        f64 hz = sono_midi_hz(e->pitch + t->transpose);
        f64 period = rate / (hz > 20 ? hz : 20) - 0.5;
        v.ks_len = (i32)floor(period);
        f64 frac = period - v.ks_len;
        if (frac < 0.1 && v.ks_len > 2) {
            v.ks_len--;
            frac += 1.0;
        }
        if (v.ks_len < 2) v.ks_len = 2;
        v.ks_tune = (1.0 - frac) / (1.0 + frac);
        v.ks = malloc((size_t)v.ks_len * sizeof(f32));
        if (v.ks) {
            f64 prev = 0;
            for (i32 k = 0; k < v.ks_len; k++) {
                prev = 0.5 * (prev + sono_noise(&v.rng)); // a softer, rounder pick
                v.ks[k] = (f32)prev;
            }
        }
    }
    f64 amp = e->velocity;
    for (i32 k = 0; k < count; k++) {
        f64 time = k / rate;
        f64 slide = gate > 0 ? sono_clamp(time / gate, 0, 1) : 1;
        f64 pitch = e->pitch + (e->pitch_to - e->pitch) * slide + t->transpose;
        if (t->vibrato_depth > 0 && t->vibrato_rate > 0) {
            f64 fade = sono_clamp(time / 0.25, 0, 1); // vibrato settles in
            pitch += fade * t->vibrato_depth * sin(2 * SONO_PI * t->vibrato_rate * time);
        }
        f64 x = sono_instrument(t->instrument, &v, time, pitch, rate);
        i32 at = first + k;
        if (at >= 0) mono[at] += (f32)(amp * sono_envelope(t, time, gate) * x);
    }
    free(v.ks);
}

// Freeverb-style stereo reverb on the send bus.
#define SONO_COMBS 8
#define SONO_ALLPASSES 4

typedef struct sono_delay {
    f32 *data;
    i32 len;
    i32 at;
    f64 store;
} sono_delay;

static ns_bool sono_delay_init(sono_delay *d, i32 len) {
    d->len = len > 1 ? len : 1;
    d->at = 0;
    d->store = 0;
    d->data = calloc((size_t)d->len, sizeof(f32));
    return d->data != NULL;
}

static f64 sono_comb(sono_delay *d, f64 x, f64 feedback, f64 damp) {
    f64 out = d->data[d->at];
    d->store = out * (1 - damp) + d->store * damp;
    d->data[d->at] = (f32)(x + d->store * feedback);
    if (++d->at == d->len) d->at = 0;
    return out;
}

static f64 sono_allpass(sono_delay *d, f64 x) {
    f64 buffered = d->data[d->at];
    d->data[d->at] = (f32)(x + buffered * 0.5);
    if (++d->at == d->len) d->at = 0;
    return buffered - x;
}

static ns_bool sono_reverb(const sono_song *s, const f32 *send, f32 *mix, i32 frames) {
    static const i32 combs[SONO_COMBS] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const i32 allpasses[SONO_ALLPASSES] = {556, 441, 341, 225};
    f64 scale = s->rate / 44100.0;
    sono_delay comb[2][SONO_COMBS];
    sono_delay pass[2][SONO_ALLPASSES];
    memset(comb, 0, sizeof(comb));
    memset(pass, 0, sizeof(pass));
    ns_bool ok = true;
    for (i32 c = 0; c < 2; c++) {
        i32 spread = c ? 23 : 0;
        for (i32 k = 0; k < SONO_COMBS; k++) ok = sono_delay_init(&comb[c][k], (i32)((combs[k] + spread) * scale)) && ok;
        for (i32 k = 0; k < SONO_ALLPASSES; k++) ok = sono_delay_init(&pass[c][k], (i32)((allpasses[k] + spread) * scale)) && ok;
    }
    if (ok) {
        for (i32 i = 0; i < frames; i++) {
            f64 in = send[i] * 0.03;
            for (i32 c = 0; c < 2; c++) {
                f64 out = 0;
                for (i32 k = 0; k < SONO_COMBS; k++) out += sono_comb(&comb[c][k], in, 0.84, 0.25);
                for (i32 k = 0; k < SONO_ALLPASSES; k++) out = sono_allpass(&pass[c][k], out);
                mix[i * 2 + c] += (f32)(out * 1.5);
            }
        }
    } else {
        sono_fail("out of memory");
    }
    for (i32 c = 0; c < 2; c++) {
        for (i32 k = 0; k < SONO_COMBS; k++) free(comb[c][k].data);
        for (i32 k = 0; k < SONO_ALLPASSES; k++) free(pass[c][k].data);
    }
    return ok;
}

// Runs one track's voices and strip into the master `mix` and reverb `send`.
static ns_bool sono_render_track(const sono_song *s, i32 track, f32 *mono, f32 *mix, f32 *send, i32 frames) {
    const sono_strip *t = &s->tracks[track];
    ns_bool any = false;
    memset(mono, 0, (size_t)frames * sizeof(f32));
    for (i32 i = 0; i < s->event_count; i++) {
        if (s->events[i].track != track) continue;
        sono_render_voice(s, t, &s->events[i], (u32)i, mono, frames);
        any = true;
    }
    if (!any) return true;

    f64 drive_gain = 1.0 + t->drive * 8.0;
    f64 drive_norm = tanh(drive_gain);
    f64 angle = (t->pan + 1.0) * SONO_PI * 0.25;
    f64 gain_l = t->volume * cos(angle) * 1.41421356237;
    f64 gain_r = t->volume * sin(angle) * 1.41421356237;
    sono_svf filter = {0, 0};

    sono_delay echo[2];
    memset(echo, 0, sizeof(echo));
    ns_bool use_echo = t->echo_beats > 0 && t->echo_mix > 0;
    if (use_echo) {
        i32 len = (i32)llround(t->echo_beats * 60.0 / s->bpm * s->rate);
        if (!sono_delay_init(&echo[0], len) || !sono_delay_init(&echo[1], len)) {
            free(echo[0].data);
            free(echo[1].data);
            sono_fail("out of memory");
            return false;
        }
    }
    for (i32 i = 0; i < frames; i++) {
        f64 x = mono[i];
        if (t->drive > 0) x = tanh(x * drive_gain) / drive_norm;
        if (t->cutoff > 0) x = sono_svf_run(&filter, x, t->cutoff, t->resonance, s->rate).low;
        f64 l = x * gain_l;
        f64 r = x * gain_r;
        if (use_echo) {
            // Ping-pong: the dry signal enters on the left and every repeat
            // crosses to the other side.
            f64 dl = echo[0].data[echo[0].at];
            f64 dr = echo[1].data[echo[1].at];
            echo[0].data[echo[0].at] = (f32)((l + r) * 0.5 + dr * t->echo_feedback);
            echo[1].data[echo[1].at] = (f32)(dl * t->echo_feedback);
            if (++echo[0].at == echo[0].len) echo[0].at = 0;
            if (++echo[1].at == echo[1].len) echo[1].at = 0;
            l += dl * t->echo_mix;
            r += dr * t->echo_mix;
        }
        mix[i * 2] += (f32)l;
        mix[i * 2 + 1] += (f32)r;
        if (t->reverb > 0) send[i] += (f32)((l + r) * 0.5 * t->reverb);
    }
    free(echo[0].data);
    free(echo[1].data);
    return true;
}

// Linear below 0.9, then a tanh knee that never exceeds 1.
static f32 sono_limit(f64 x) {
    f64 a = fabs(x);
    if (a > 0.9) a = 0.9 + 0.1 * tanh((a - 0.9) / 0.1);
    return (f32)(x < 0 ? -a : a);
}

static ns_bool sono_mixdown(sono_song *s) {
    if (!s->dirty && s->mix) return true;
    i32 natural = (i32)ceil(sono_natural_seconds(s) * s->rate);
    i32 out_frames = s->loop_beats > 0 ? sono_loop_frames(s) : natural;
    i32 frames = natural > out_frames ? natural : out_frames;
    if (frames < 1) frames = 1;
    f32 *mono = calloc((size_t)frames, sizeof(f32));
    f32 *send = calloc((size_t)frames, sizeof(f32));
    f32 *mix = calloc((size_t)frames * 2, sizeof(f32));
    ns_bool ok = mono && send && mix;
    ns_bool reverb = false;
    if (!ok) sono_fail("out of memory");
    for (i32 t = 0; ok && t < s->track_count; t++) {
        ok = sono_render_track(s, t, mono, mix, send, frames);
        if (s->tracks[t].reverb > 0) reverb = true;
    }
    if (ok && reverb) ok = sono_reverb(s, send, mix, frames);
    free(mono);
    free(send);
    if (!ok) {
        free(mix);
        return false;
    }
    if (out_frames < 1) out_frames = 1;
    if (s->loop_beats > 0 && frames > out_frames) {
        // Wrap every tail past the loop point back onto its start.
        for (i32 i = out_frames; i < frames; i++) {
            i32 to = i % out_frames;
            mix[to * 2] += mix[i * 2];
            mix[to * 2 + 1] += mix[i * 2 + 1];
        }
    }
    for (i32 i = 0; i < out_frames * 2; i++) mix[i] = sono_limit(mix[i] * s->volume);
    free(s->mix);
    s->mix = mix;
    s->mix_frames = out_frames;
    s->dirty = false;
    return true;
}

i32 sono_render(i32 song, f32 *out, i32 frames) {
    sono_song *s = sono_get(song);
    if (!s) return -1;
    if (!out || frames < 0) {
        sono_fail("sono_render needs a buffer of frames * 2 floats");
        return -1;
    }
    if (!sono_mixdown(s)) return -1;
    i32 n = frames < s->mix_frames ? frames : s->mix_frames;
    memcpy(out, s->mix, (size_t)n * 2 * sizeof(f32));
    if (frames > n) memset(out + n * 2, 0, (size_t)(frames - n) * 2 * sizeof(f32));
    return n;
}

static void sono_put_u16(unsigned char *p, u32 v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void sono_put_u32(unsigned char *p, u32 v) {
    sono_put_u16(p, v & 0xffff);
    sono_put_u16(p + 2, v >> 16);
}

ns_bool sono_save_wav(i32 song, const char *path) {
    sono_song *s = sono_get(song);
    if (!s) return false;
    if (!path || !path[0]) {
        sono_fail("sono_save_wav needs a path");
        return false;
    }
    if (!sono_mixdown(s)) return false;
    u32 samples = (u32)s->mix_frames * 2;
    u32 data_bytes = samples * 2;
    unsigned char header[44];
    memcpy(header, "RIFF", 4);
    sono_put_u32(header + 4, 36 + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    sono_put_u32(header + 16, 16);
    sono_put_u16(header + 20, 1); // PCM
    sono_put_u16(header + 22, 2); // stereo
    sono_put_u32(header + 24, (u32)s->rate);
    sono_put_u32(header + 28, (u32)s->rate * 4);
    sono_put_u16(header + 32, 4);
    sono_put_u16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    sono_put_u32(header + 40, data_bytes);

    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(sono_error, sizeof(sono_error), "cannot open %s for writing", path);
        return false;
    }
    ns_bool ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
    unsigned char chunk[4096];
    u32 used = 0;
    for (u32 i = 0; ok && i < samples; i++) {
        f64 x = sono_clamp(s->mix[i], -1, 1);
        i32 v = (i32)lrint(x * 32767.0);
        sono_put_u16(chunk + used, (u32)(v & 0xffff));
        used += 2;
        if (used == sizeof(chunk)) {
            ok = fwrite(chunk, 1, used, f) == used;
            used = 0;
        }
    }
    if (ok && used) ok = fwrite(chunk, 1, used, f) == used;
    if (fclose(f) != 0) ok = false;
    if (!ok) snprintf(sono_error, sizeof(sono_error), "failed to write %s", path);
    return ok;
}

const char *sono_last_error(void) {
    return sono_error;
}
