#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

ns=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

cd "$root"
"$ns" run test/os_file_test.ns
if [ "$(uname -s)" = "Darwin" ]; then
    sh test/audio_apple_test.sh "$ns"
fi
