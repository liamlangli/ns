#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

ns=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

scaffold_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-scaffold.XXXXXX")
trap 'rm -rf "$scaffold_tmp"' EXIT HUP INT TERM

cd "$scaffold_tmp"
"$ns" create created
for file in ns.mod main.ns README.md AGENTS.md .gitignore; do
    test -f "created/$file"
done
cmp "$root/AGENTS.md" created/AGENTS.md

mkdir initialized
"$ns" init initialized
cmp "$root/AGENTS.md" initialized/AGENTS.md

mkdir preserved
printf '%s\n' '# project-specific agent rules' > preserved/AGENTS.md
"$ns" init preserved
test "$(cat preserved/AGENTS.md)" = '# project-specific agent rules'

cd "$root"
"$ns" run test/os_file_test.ns
if [ "$(uname -s)" = "Darwin" ]; then
    sh test/audio_apple_test.sh "$ns"
fi
