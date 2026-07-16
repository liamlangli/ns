# Audio module

`audio` loads local music and sound effects and plays them through Apple's
AVFAudio framework. The first backend supports macOS, iOS, and visionOS.

```ns
use audio

fn main() {
    if !audio_init() {
        print(`audio init failed: {audio_last_error()}`)
        return
    }

    let music = audio_load_music("assets/theme.m4a")
    let click = audio_load_sfx("assets/click.wav")
    if music == 0 || click == 0 {
        print(`audio load failed: {audio_last_error()}`)
        audio_shutdown()
        return
    }

    audio_set_volume(music, 0.7)
    audio_play(music, true)
    audio_play(click, false)

    // SFX calls may overlap. Music supports pause/resume, seek, and looping.
    audio_pause(music)
    audio_seek(music, 10.0)
    audio_resume(music)

    audio_unload(click)
    audio_unload(music)
    audio_shutdown()
}
```

Paths can be absolute or relative. Relative paths first use the process working
directory, then the application bundle's resource directory. Supported file
formats are those decoded by `AVAudioPlayer`, including common WAV, AIFF, CAF,
MP3, AAC/M4A, and Apple Lossless files.

Music handles own one player, so `audio_play` restarts that track. SFX handles
retain source data and allocate an independent player per call, allowing the
same effect to overlap. The backend currently supports up to 255 loaded assets
and 64 simultaneous SFX voices.

On iOS and visionOS, `audio_init` activates an ambient, mix-with-others audio
session. On macOS it is a lightweight no-op. All other calls initialize their
native storage lazily, but explicit initialization is recommended so session
errors can be handled at startup.
