#!/bin/sh
# `ns <word>` opens an installed app named ns_<word> or <word> when the word is
# no command and names no file; only when nothing matches does the script
# error appear. A private SDK layout (bin/ns beside the apps) keeps the real
# installation out of it.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

ns=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-app-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
tmp=$(CDPATH= cd -- "$tmp" && pwd -P)

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    exit 1
}

mkdir -p "$tmp/sdk/bin" "$tmp/work/sub"
cp "$ns" "$tmp/sdk/bin/ns"
ln -s "$root/lib" "$tmp/sdk/ref"
ln -s "$(dirname -- "$ns")" "$tmp/sdk/lib"
sdk_ns=$tmp/sdk/bin/ns

# A command-line app takes over: argv, NS_ARG*, absolute paths, exit code.
cat > "$tmp/sdk/bin/ns_tool" <<EOF
#!/bin/sh
printf 'argv=%s|argc=%s|0=%s|1=%s|2=%s\n' "\$*" "\$NS_ARGC" "\$NS_ARG0" "\$NS_ARG1" "\${NS_ARG2:-unset}" > "$tmp/tool.out"
exit 7
EOF
chmod +x "$tmp/sdk/bin/ns_tool"
cd "$tmp/work"
status=0
NS_ARG2=stale "$sdk_ns" tool sub --flag || status=$?
[ "$status" -eq 7 ] || fail "ns tool exited $status, not the app's 7"
expected="argv=$tmp/work/sub --flag|argc=2|0=$tmp/work/sub|1=--flag|2=unset"
[ "$(cat "$tmp/tool.out")" = "$expected" ] || fail "ns tool passed $(cat "$tmp/tool.out")"

# The bare name is found when ns_<word> is not.
cat > "$tmp/sdk/bin/plain" <<EOF
#!/bin/sh
echo plain > "$tmp/plain.out"
EOF
chmod +x "$tmp/sdk/bin/plain"
"$sdk_ns" plain
[ "$(cat "$tmp/plain.out")" = plain ] || fail "ns plain did not run bin/plain"

# ns_<word> wins over <word>.
cat > "$tmp/sdk/bin/tool" <<EOF
#!/bin/sh
echo bare > "$tmp/bare.out"
EOF
chmod +x "$tmp/sdk/bin/tool"
rm -f "$tmp/tool.out"
"$sdk_ns" tool || true
[ -f "$tmp/tool.out" ] || fail "ns tool did not run bin/ns_tool"
[ ! -f "$tmp/bare.out" ] || fail "ns tool ran bin/tool over bin/ns_tool"

# A file of that name is a script, not an app.
printf 'fn main() {\n}\n' > "$tmp/work/plain"
rm -f "$tmp/plain.out"
"$sdk_ns" plain >/dev/null 2>&1 || true
[ ! -f "$tmp/plain.out" ] || fail "ns plain ran the app although ./plain exists"
rm -f "$tmp/work/plain"

# A command word is never an app.
cp "$tmp/sdk/bin/plain" "$tmp/sdk/bin/ns_lint"
rm -f "$tmp/plain.out"
"$sdk_ns" lint "$tmp/work" >/dev/null 2>&1 || true
[ ! -f "$tmp/plain.out" ] || fail "ns lint ran bin/ns_lint"

# Without a compiled app, the source project under share/nscode runs.
mkdir -p "$tmp/sdk/share/nscode/srcapp/src"
cat > "$tmp/sdk/share/nscode/srcapp/ns.mod" <<'EOF'
schema = "ns.mod/v2"
name = "srcapp"
version = "0.1.0"
type = "cli"
source = "src"
entry = "main.ns"
EOF
cat > "$tmp/sdk/share/nscode/srcapp/src/main.ns" <<'EOF'
use std
use os

fn main() {
    print(`srcapp {os_env("NS_ARGC")} {os_env("NS_ARG0")}\n`)
}
EOF
out=$("$sdk_ns" srcapp sub 2>/dev/null | tail -n 1)
[ "$out" = "srcapp 1 $tmp/work/sub" ] || fail "ns srcapp printed '$out'"

# Nothing matches: the script error, as before.
status=0
err=$("$sdk_ns" nosuchapp 2>&1) || status=$?
[ "$status" -ne 0 ] || fail "ns nosuchapp succeeded"
printf '%s' "$err" | grep -q 'empty file or folder nosuchapp' || fail "ns nosuchapp reported: $err"

# `ns ns` never starts ns again.
status=0
"$sdk_ns" ns >/dev/null 2>&1 || status=$?
[ "$status" -ne 0 ] || fail "ns ns succeeded"

# A Darwin bundle opens detached with the same arguments.
if [ "$(uname -s)" = Darwin ]; then
    mkdir -p "$tmp/sdk/bin/ns_win.app/Contents/MacOS"
    cat > "$tmp/sdk/bin/ns_win.app/Contents/MacOS/ns_win" <<EOF
#!/bin/sh
printf '%s|%s\n' "\$*" "\$NS_ARG0" > "$tmp/win.tmp"
mv "$tmp/win.tmp" "$tmp/win.out"
EOF
    chmod +x "$tmp/sdk/bin/ns_win.app/Contents/MacOS/ns_win"
    "$sdk_ns" win sub >/dev/null 2>&1
    i=0
    while [ ! -f "$tmp/win.out" ] && [ "$i" -lt 50 ]; do
        sleep 0.1
        i=$((i + 1))
    done
    [ "$(cat "$tmp/win.out" 2>/dev/null)" = "$tmp/work/sub|$tmp/work/sub" ] || fail "ns win bundle got '$(cat "$tmp/win.out" 2>/dev/null)'"
fi

echo "ns app fallback: passed"
