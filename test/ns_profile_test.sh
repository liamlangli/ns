#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

ns=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-profile.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if [ ! -f "$root/nscode/profile/ns.mod" ] || [ ! -f "$root/nscode/profile/main.ns" ]; then
    printf '%s\n' 'FAIL: nscode/profile viewer sources are missing.' >&2
    exit 1
fi
if ! grep -q 'share/nscode/profile' "$root/Makefile"; then
    printf '%s\n' 'FAIL: make install does not install nscode/profile.' >&2
    exit 1
fi
if ! grep -q 'nscode/profile' "$root/Makefile" || ! grep -q 'profiler:' "$root/Makefile"; then
    printf '%s\n' 'FAIL: Makefile does not compile nscode/profile via ns build.' >&2
    exit 1
fi
if [ "$(uname -s)" = Darwin ]; then
    viewer="$root/nscode/profile/bin/nscode-profile.app/Contents/MacOS/nscode-profile"
    if [ ! -x "$viewer" ]; then
        printf '%s\n' 'FAIL: compiled nscode-profile.app is missing; make should ns build nscode/profile.' >&2
        exit 1
    fi
    if ! file "$viewer" | grep -q 'Mach-O'; then
        printf '%s\n' 'FAIL: nscode-profile is not a Mach-O executable.' >&2
        exit 1
    fi
fi

if ! "$ns" --help | grep -q 'print a hot-path summary'; then
    printf '%s\n' 'FAIL: ns help does not mention the profile hot-path summary.' >&2
    exit 1
fi

if ! "$ns" --help | grep -q 'profiler \[file\]'; then
    printf '%s\n' 'FAIL: ns help does not mention `ns profiler`.' >&2
    exit 1
fi
if "$ns" --help | grep -q 'profile view'; then
    printf '%s\n' 'FAIL: ns help still mentions removed `ns profile view`.' >&2
    exit 1
fi

cat > "$tmp/hot.ns" <<'EOF'
use std

fn leaf() {
    let x = 0
    for i in 0 to 2000 {
        x = x + i
    }
}

fn mid() {
    leaf()
    leaf()
}

fn main() {
    mid()
}
EOF

cd "$tmp"
"$ns" profile hot.ns > "$tmp/out.txt"

test -f "$tmp/ns.profile"
test -f "$tmp/ns.profile.json"

grep -q '^format: ns-profile-v4$' "$tmp/ns.profile"
grep -q '^fn: scope ' "$tmp/ns.profile"
grep -q '^flame: ' "$tmp/ns.profile"
grep -q 'main;mid;leaf' "$tmp/ns.profile"
grep -q 'traceEvents' "$tmp/ns.profile.json"
grep -q '"ph": "X"' "$tmp/ns.profile.json"
grep -q 'hot functions by self time' "$tmp/out.txt"
grep -q 'hot stacks by self time' "$tmp/out.txt"
if grep -q '^flamechart:' "$tmp/out.txt"; then
    printf '%s\n' 'FAIL: console profile still prints an ASCII flamechart.' >&2
    exit 1
fi

# --profile on a bare file is the same collection path.
rm -f "$tmp/ns.profile" "$tmp/ns.profile.json"
"$ns" --profile hot.ns > "$tmp/flag.txt"
test -f "$tmp/ns.profile"
grep -q '^format: ns-profile-v4$' "$tmp/ns.profile"
grep -q 'main;mid;leaf' "$tmp/ns.profile"
