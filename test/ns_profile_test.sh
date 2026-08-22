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

# The report belongs in bin/, never beside the sources.
test -f "$tmp/bin/ns.profile"
if [ -e "$tmp/ns.profile" ]; then
    printf '%s\n' 'FAIL: ns profile wrote ns.profile outside bin/.' >&2
    exit 1
fi
test ! -e "$tmp/ns.profile.json"

grep -q '^format: ns-profile-v5$' "$tmp/bin/ns.profile"
grep -q '^threads: ' "$tmp/bin/ns.profile"
grep -q '^thread: 0 main$' "$tmp/bin/ns.profile"
grep -q '^scope_event: main ' "$tmp/bin/ns.profile"
grep -q '^fn: scope ' "$tmp/bin/ns.profile"
grep -q '^flame: ' "$tmp/bin/ns.profile"
grep -q 'main;mid;leaf' "$tmp/bin/ns.profile"
grep -q 'hot functions by self time' "$tmp/out.txt"
grep -q 'hot stacks by self time' "$tmp/out.txt"
if grep -q '^flamechart:' "$tmp/out.txt"; then
    printf '%s\n' 'FAIL: console profile still prints an ASCII flamechart.' >&2
    exit 1
fi

# --profile on a bare file is the same collection path.
rm -rf "$tmp/bin"
"$ns" --profile hot.ns > "$tmp/flag.txt"
test -f "$tmp/bin/ns.profile"
if [ -e "$tmp/ns.profile" ]; then
    printf '%s\n' 'FAIL: ns --profile wrote ns.profile outside bin/.' >&2
    exit 1
fi
test ! -e "$tmp/ns.profile.json"
grep -q '^format: ns-profile-v5$' "$tmp/bin/ns.profile"
grep -q 'main;mid;leaf' "$tmp/bin/ns.profile"

# A profiled run of a project entry reports into that project's bin/, even when
# the command runs from outside the project directory.
mkdir -p "$tmp/run-profile/src"
cat > "$tmp/run-profile/ns.mod" <<'EOF'
schema = "ns.mod/v1"
name = "profile-run"
version = "0.1.0"
type = "app"
source = "src"
entry = "main.ns"
EOF
cat > "$tmp/run-profile/src/main.ns" <<'EOF'
fn main() {
    let answer = 40 + 2
    assert answer == 42
}
EOF

cd "$tmp"
"$ns" profile "$tmp/run-profile/src/main.ns" > "$tmp/run-profile.txt"
test -f "$tmp/run-profile/bin/ns.profile"
if [ -e "$tmp/run-profile/ns.profile" ]; then
    printf '%s\n' 'FAIL: ns profile wrote ns.profile at the project root.' >&2
    exit 1
fi
grep -q '^format: ns-profile-v5$' "$tmp/run-profile/bin/ns.profile"
cd "$tmp"

# A profiled build records compiler phases in the same report and viewer. Use
# Wasm here so the coverage does not depend on a host linker or bundle format.
mkdir -p "$tmp/build-profile/src"
cat > "$tmp/build-profile/ns.mod" <<'EOF'
schema = "ns.mod/v1"
name = "profile-build"
version = "0.1.0"
type = "app"
target = "wasm"
source = "src"
entry = "main.ns"
EOF
cat > "$tmp/build-profile/src/main.ns" <<'EOF'
fn main() {
    let answer = 40 + 2
    assert answer == 42
}
EOF

cd "$tmp/build-profile"
"$ns" build --profile --force > "$tmp/build-profile.txt"
test -f "$tmp/build-profile/bin/ns.profile"
test ! -e "$tmp/build-profile/ns.profile"
test -f "$tmp/build-profile/bin/profile-build.wasm"
grep -q 'compiler::build$' "$tmp/build-profile/bin/ns.profile"
grep -q 'compiler::check_cache$' "$tmp/build-profile/bin/ns.profile"
grep -q 'compiler::link_sources$' "$tmp/build-profile/bin/ns.profile"
grep -q 'compiler::parse$' "$tmp/build-profile/bin/ns.profile"
grep -q 'compiler::lower_ssa$' "$tmp/build-profile/bin/ns.profile"
grep -q 'compiler::emit_wasm$' "$tmp/build-profile/bin/ns.profile"
grep -q 'compiler::package_wasm$' "$tmp/build-profile/bin/ns.profile"
grep -q 'compiler::build;compiler::build_target;compiler::compile;compiler::parse' "$tmp/build-profile/bin/ns.profile"

# An up-to-date build still profiles cache validation and skips compilation.
"$ns" build --profile > "$tmp/build-profile-cache.txt"
grep -q 'compiler::check_cache$' "$tmp/build-profile/bin/ns.profile"
if grep -q 'compiler::parse$' "$tmp/build-profile/bin/ns.profile"; then
    printf '%s\n' 'FAIL: cached ns build --profile unexpectedly compiled sources.' >&2
    exit 1
fi
