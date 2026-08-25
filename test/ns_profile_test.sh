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
# Keep `ns profile` headless in CI; the auto-open path is covered manually.
export NS_PROFILE_NO_VIEW=1

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
if ! "$ns" --help | grep -q '\-\-live-port'; then
    printf '%s\n' 'FAIL: ns help does not mention live profiling.' >&2
    exit 1
fi
if [ ! -f "$root/nscode/profile/live.ns" ]; then
    printf '%s\n' 'FAIL: nscode/profile/live.ns is missing.' >&2
    exit 1
fi
if ! grep -q 'nscode/profile/live.ns' "$root/Makefile"; then
    printf '%s\n' 'FAIL: make install does not install nscode/profile/live.ns.' >&2
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

grep -q '^format: ns-profile-v6$' "$tmp/bin/ns.profile"
grep -q '^threads: ' "$tmp/bin/ns.profile"
grep -q '^thread: 0 main$' "$tmp/bin/ns.profile"
grep -q '^timeline_blob: ' "$tmp/bin/ns.profile"
test -f "$tmp/bin/ns.profile.tl" -o -f "$tmp/bin/ns.profile.tl.zst"
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
grep -q '^format: ns-profile-v6$' "$tmp/bin/ns.profile"
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
grep -q '^format: ns-profile-v6$' "$tmp/run-profile/bin/ns.profile"
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

# ---------------------------------------------------------------------------
# Live capture: `ns profile --live-port` streams the run to a listening viewer.
# The stand-in viewer here checks the wire format end to end.
live_port=9711
cat > "$tmp/live_listen.ns" <<EOF
use std
use net

fn u32_at(buf: [u8], off: i32) i32 {
    let b0 = (buf[off] as i32) & 255
    let b1 = (buf[off + 1] as i32) & 255
    let b2 = (buf[off + 2] as i32) & 255
    let b3 = (buf[off + 3] as i32) & 255
    return b0 + b1 * 256 + b2 * 65536 + b3 * 16777216
}

fn main() i32 {
    let server = net_tcp_listen_local($live_port)
    if server < 0 {
        print("FAIL: listen" + unescape("\\n"))
        return 1
    }
    let fd = net_tcp_accept(server)
    if fd < 0 {
        print("FAIL: accept" + unescape("\\n"))
        return 1
    }
    let buf = [u8](1048576)
    let len = 0
    loop true {
        let n = net_recv(fd)
        if n <= 0 {
            break
        }
        if len + n > buf.len {
            break
        }
        len = len + net_buf_read(buf, len, n)
    }
    net_close(fd)
    net_close(server)
    if len < 4 {
        print("FAIL: short stream" + unescape("\\n"))
        return 1
    }
    if u32_at(buf, 0) != 1347179342 {
        print("FAIL: magic" + unescape("\\n"))
        return 1
    }
    let pos = 4
    let hello = 0
    let syms = 0
    let threads = 0
    let frames = 0
    let events = 0
    let bye = 0
    loop len - pos >= 8 {
        let kind = u32_at(buf, pos)
        let size = u32_at(buf, pos + 4)
        if size < 0 {
            break
        }
        if len - pos - 8 < size {
            break
        }
        if kind == 1 {
            hello = hello + 1
        }
        if kind == 2 {
            syms = syms + 1
        }
        if kind == 3 {
            threads = threads + 1
        }
        if kind == 4 {
            frames = frames + 1
            events = events + u32_at(buf, pos + 20)
        }
        if kind == 5 {
            bye = bye + 1
        }
        pos = pos + 8 + size
    }
    print(\`live: hello={hello} threads={threads} symbols={syms} frames={frames} events={events} bye={bye}\` + unescape("\\n"))
    if pos != len {
        print("FAIL: framing" + unescape("\\n"))
        return 1
    }
    if hello != 1 {
        print("FAIL: hello" + unescape("\\n"))
        return 1
    }
    if threads < 1 {
        print("FAIL: threads" + unescape("\\n"))
        return 1
    }
    if syms < 1 {
        print("FAIL: symbols" + unescape("\\n"))
        return 1
    }
    if frames < 1 {
        print("FAIL: frames" + unescape("\\n"))
        return 1
    }
    if events < 1 {
        print("FAIL: events" + unescape("\\n"))
        return 1
    }
    if bye != 1 {
        print("FAIL: bye" + unescape("\\n"))
        return 1
    }
    print("PASS: live stream" + unescape("\\n"))
    return 0
}
EOF

cd "$tmp"
rm -rf "$tmp/bin"
"$ns" run "$tmp/live_listen.ns" > "$tmp/live.txt" 2>&1 &
listener=$!
sleep 1
"$ns" profile --live-port "$live_port" hot.ns > "$tmp/live-run.txt" 2>&1
wait "$listener"
if ! grep -q 'PASS: live stream' "$tmp/live.txt"; then
    printf '%s\n' 'FAIL: live capture did not reach the listening viewer.' >&2
    cat "$tmp/live.txt" >&2
    exit 1
fi
grep -q 'live capture streaming to 127.0.0.1' "$tmp/live-run.txt"
# A live run still leaves the report behind, and never opens a second viewer.
test -f "$tmp/bin/ns.profile"

# Without a viewer the run degrades to an ordinary local profile.
rm -rf "$tmp/bin"
"$ns" profile --live-port 9712 hot.ns > "$tmp/live-none.txt" 2>&1
grep -q 'live viewer not listening' "$tmp/live-none.txt"
test -f "$tmp/bin/ns.profile"
grep -q '^format: ns-profile-v6$' "$tmp/bin/ns.profile"
