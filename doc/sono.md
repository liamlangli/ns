# Sono module

`sono` is a small music synthesizer driven from script. A program describes a
song — tempo, tracks, instruments, notes — and sono renders it offline into
samples or a WAV file. It is meant for writing game music and sound effects
as source instead of shipping recorded audio.

```ns
use sono
use audio

fn main() {
    let song = sono_new(SONO_RATE, 120)
    sono_set_loop(song, 16)

    let lead = sono_track(song, SONO_LEAD)
    sono_track_echo(song, lead, 0.75, 0.3, 0.25)
    sono_play(song, lead, 0, "o5 e:0.5 g a:1 g:0.5 e d:1 c:2 r:2 | d:0.5 e g:1 e:0.5 d c:1 a4:3 r:1")

    let bass = sono_track(song, SONO_BASS)
    sono_play(song, bass, 0, "o2 (c:0.5*4 a1*4 f1*4 g1*4)*2")

    let kick = sono_track(song, SONO_KICK)
    let hat = sono_track(song, SONO_HIHAT)
    let bar = 0.0 as f64
    for i in 0 to 4 {
        sono_steps(song, kick, bar, 0.25, "x... ..x. x... ....", 60)
        bar = sono_steps(song, hat, bar, 0.25, "x.x. x.xX x.x. x.xx", 60)
    }

    if !sono_save_wav(song, "theme.wav") {
        print(`sono: {sono_last_error()}\n`)
    }
    sono_free(song)

    let music = audio_load_music("theme.wav")
    audio_play(music, true)
}
```

`sample/ns/sono.ns` is a longer example with chords, drums and two effects.

## Model

- **Song** — `sono_new(sample_rate, bpm)` returns a positive handle, 0 on
  failure. Free it with `sono_free`. `SONO_RATE` is 44100.
- **Time** is in beats (quarter notes). `sono_set_bpm` sets the tempo and
  `sono_set_swing(song, 0..1)` delays every off-beat eighth (0.33 is a triplet
  shuffle, 1 pushes it to the last sixteenth).
- **Pitch** is a MIDI note number: 60 is middle C, 69 is A 440 Hz, and
  fractional values detune. `sono_pitch("f#3")` converts a name.
- **Track** — `sono_track(song, instrument)` returns the track index. Each
  track has one instrument and a strip of effects, applied in this order:
  envelope (per note), drive, low-pass filter, volume, pan, ping-pong echo,
  and a send to the song's shared reverb.
- **Velocity** and gains are 0..1. The master bus runs through a soft limiter,
  linear below 0.9, so a busy mix bends instead of clipping.

Each instrument starts with its own envelope; `sono_track_envelope(song,
track, attack, decay, sustain, release)` replaces it (seconds, seconds,
level, seconds).

## Instruments

| Group | Constants | Notes |
| --- | --- | --- |
| Tones | `SONO_SINE`, `SONO_SQUARE`, `SONO_SAW`, `SONO_TRIANGLE`, `SONO_NOISE` | Band-limited oscillators. Noise follows the pitch with a low-pass. |
| Instruments | `SONO_PLUCK` (string), `SONO_KEYS` (electric piano), `SONO_BELL`, `SONO_ORGAN`, `SONO_PAD`, `SONO_BASS`, `SONO_LEAD` | Pluck is Karplus-Strong; keys and bell are FM; pad is three detuned saws; bass has an enveloped filter; lead has vibrato. |
| Drums | `SONO_KICK`, `SONO_SNARE`, `SONO_HIHAT`, `SONO_OPEN_HAT`, `SONO_CLAP`, `SONO_TOM`, `SONO_CRASH` | One-shot. |
| Effects | `SONO_COIN`, `SONO_JUMP`, `SONO_LASER`, `SONO_EXPLOSION`, `SONO_HIT`, `SONO_POWERUP`, `SONO_BLIP` | One-shot game sounds. |

One-shot instruments ignore the note length and always sound for their own
natural length. Pitch 60 is their stock tuning; each semitone away retunes
them, so `x` (60) is the default hit and `n67` a higher tom or a brighter coin.
`sono_slide` bends a one-shot over its whole length.

## Placing notes

```ns
sono_note(song, track, beat, beats, pitch, velocity)
sono_slide(song, track, beat, beats, from_pitch, to_pitch, velocity)
```

The two text forms schedule many notes at once. Both return the beat after
what they scheduled, so calls chain, or -1 with `sono_last_error()` naming the
problem and its byte offset; a failed call schedules nothing.

### Notation: `sono_play(song, track, beat, score)`

| Text | Meaning |
| --- | --- |
| `c d e f g a b` | A note in the current octave (case does not matter). |
| `c# eb f##` | `#` sharpens, `b` right after a letter flattens. |
| `c5 a3` | Digits pick that note's octave only. |
| `x` | A hit at pitch 60, for drums. |
| `n64` `n61.5` | A raw MIDI pitch. |
| `:0.5` `:2` `:1/3` | Length in beats. It sticks for every following note (default 1). |
| `r` `r:2` | Rest. |
| `~` `~:2` | Tie: hold the previous note or chord for one more length. |
| `[c e g]` | Chord; its notes start together. A length after `]` applies to all. |
| `o5` `>` `<` | Set, raise or lower the current octave (default 4). |
| `v0.6` | Velocity for the following notes (default 0.8). |
| `!` `?` | Suffix accent (×1.25) or ghost note (×0.5). |
| `*4` | Suffix repeat for a note, chord or rest. |
| `( ... )*2` | Repeat a group. Groups nest. |
| spaces, `,` and `|` | Ignored; use `|` for bar lines. |

Suffixes go in the order length, accent, repeat: `g:0.5!*3`. Write notes
apart (`c b`) — `cb` reads as C flat.

```ns
// Two bars of 4/4: a dotted rhythm, a chord held for two beats, a triplet run.
sono_play(song, keys, 0, "o4 c:1.5 e:0.5 [f a c5]:2 | (g:1/3 a b)*2 c5:2")
```

### Step grid: `sono_steps(song, track, beat, step, pattern, pitch)`

One character per `step` beats: `X` accent, `x` hit, `o` ghost, `.` `-` `_`
rest. Spaces and `|` are ignored. Every hit uses `pitch` and is `step` beats
long, so the grid works for melodic tracks too.

```ns
sono_steps(song, snare, 0, 0.25, ".... x... .... x..o", 60)
```

## Effects

| Call | Range |
| --- | --- |
| `sono_track_volume(song, track, volume)` | 0..4, default 0.7 |
| `sono_track_pan(song, track, pan)` | -1 left .. 1 right |
| `sono_track_transpose(song, track, semitones)` | added to every note |
| `sono_track_filter(song, track, cutoff_hz, resonance)` | low-pass; cutoff 0 turns it off |
| `sono_track_drive(song, track, amount)` | 0..1 saturation |
| `sono_track_vibrato(song, track, rate_hz, semitones)` | eases in over the first quarter second |
| `sono_track_echo(song, track, beats, feedback, mix)` | tempo-synced ping-pong delay |
| `sono_track_reverb(song, track, amount)` | send to the shared room reverb |
| `sono_set_volume(song, volume)` | master gain, default 0.8 |

## Rendering

- `sono_frames(song)` / `sono_seconds(song)` give the rendered length,
  including release, echo and reverb tails. `sono_beats(song)` is the end of
  the last note.
- `sono_set_loop(song, beats)` renders exactly that many beats and folds every
  tail past the end back onto the start, so `audio_play(handle, true)` loops
  without a gap or a cut-off reverb.
- `sono_render(song, out, frames)` writes interleaved stereo `f32` frames into
  `out`, which needs `frames * 2` elements; extra frames are zeroed.
- `sono_save_wav(song, path)` writes a 16-bit stereo PCM WAV.

Rendering is deterministic: the same song and `sono_set_seed` value always give
the same samples, interpreted, on ns_cpu or compiled natively. The result is
cached until the song changes. A song renders at most ten minutes.

`sono_clear(song)` removes the notes and keeps the tracks, so one song can be
reused to build several effects.

## Platforms

sono is portable C with no dependency beyond libm. It loads as `sono.dylib` /
`sono.so` on desktop, links into native builds, and is embedded in generated
Apple apps. Wasm builds do not support it yet. Handles are not synchronized;
build and render a song from one task at a time.
