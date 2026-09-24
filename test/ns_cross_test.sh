#!/bin/sh
# Cross linking uses the Linux machine's gcc (NS_LINUX_HOST) or a GNU cross
# gcc named x86_64-linux-gnu-gcc. The feature modules are not required: this
# program imports only `std`.

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

if ! command -v x86_64-linux-gnu-gcc >/dev/null 2>&1 && [ -z "${NS_LINUX_HOST:-}" ]; then
    echo "skip: set NS_LINUX_HOST or install x86_64-linux-gnu-gcc"
    exit 0
fi

ns=$1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-cross-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp/app"
cat > "$tmp/app/ns.mod" <<'EOF'
schema = "ns.mod/v2"
name = "hi"
version = "0.1.0"
type = "cli"
source = "."
entry = "main.ns"
EOF
cat > "$tmp/app/main.ns" <<'EOF'
use std

fn main() {
    print("hi\n")
}
EOF

"$ns" build --target x86_64-linux-gnu "$tmp/app" -o "$tmp/hi" > "$tmp/build.log" 2>&1 || {
    cat "$tmp/build.log" >&2
    echo "FAIL: cross build did not produce an executable." >&2
    exit 1
}

magic=$(od -An -t x1 -N 4 "$tmp/hi" | tr -d ' \n')
if [ "$magic" != "7f454c46" ]; then
    echo "FAIL: $tmp/hi is not an ELF file (magic $magic)." >&2
    exit 1
fi

echo "cross build emitted an x86_64 Linux ELF"

# A foreign platform target is skipped by a nameless `ns build` and built when named.
mkdir -p "$tmp/multi"
cat > "$tmp/multi/ns.mod" <<'EOF'
schema = "ns.mod/v2"
name = "multi"
version = "0.1.0"
type = "cli"
source = "."

[[targets]]
name = "host"
entry = "main.ns"
default = true

[[targets]]
name = "linux"
entry = "main.ns"
target_os = "linux"
target_arch = "x86_64"
EOF
cp "$tmp/app/main.ns" "$tmp/multi/main.ns"

"$ns" build "$tmp/multi" > "$tmp/all.log" 2>&1 || {
    cat "$tmp/all.log" >&2
    echo "FAIL: nameless build should skip the cross target and build the host one." >&2
    exit 1
}
if ! grep -q "skip linux" "$tmp/all.log"; then
    cat "$tmp/all.log" >&2
    echo "FAIL: nameless build did not skip the linux target." >&2
    exit 1
fi
test -f "$tmp/multi/bin/host/host"

(
    cd "$tmp/multi"
    "$ns" build linux > "$tmp/named.log" 2>&1
) || {
    cat "$tmp/named.log" >&2
    echo "FAIL: naming the linux target did not build it." >&2
    exit 1
}
named_magic=$(od -An -t x1 -N 4 "$tmp/multi/bin/linux/linux" | tr -d ' \n')
if [ "$named_magic" != "7f454c46" ]; then
    echo "FAIL: named linux target is not an ELF file (magic $named_magic)." >&2
    exit 1
fi

echo "nameless build skips the cross target; naming it emits ELF"
