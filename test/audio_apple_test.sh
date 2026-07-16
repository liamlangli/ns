#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

audio_fixture=/tmp/ns-audio-test.wav
trap 'rm -f "$audio_fixture"' EXIT HUP INT TERM

sh test/audio_apple_compile.sh

# 10 ms of mono 8 kHz 16-bit PCM silence in a valid RIFF/WAVE container.
printf '%s' \
    'UklGRsQAAABXQVZFZm10IBAAAAABAAEAQB8AAIA+AAACABAAZGF0YaAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' \
    | base64 -D > "$audio_fixture"

"$1" run test/audio_test.ns
