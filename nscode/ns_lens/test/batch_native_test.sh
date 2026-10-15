#!/bin/sh
# Exercise the exec path: ns test itself uses the interpreter.
set -eu
ns=$1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64|Linux-x86_64) ;;
*) printf '%s\n' 'SKIP: lens batch test requires a native compiler backend.'; exit 0 ;;
esac
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-lens-batch.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp/src"
for file in "$root"/src/*.ns; do
    case "$file" in */main.ns) continue ;; esac
    cp "$file" "$tmp/src/"
done
cp "$root/test/batch_test.ns" "$tmp/src/main.ns"
cat > "$tmp/ns.mod" <<'MOD'
schema = "ns.mod/v2"
name = "batch-test"
type = "cli"
target = "exec"
source = "src"
entry = "main.ns"
MOD
"$ns" build "$tmp" >/dev/null
"$tmp/bin/batch-test"
cp "$root/test/zoom_code_test.ns" "$tmp/src/main.ns"
"$ns" build "$tmp" >/dev/null
"$tmp/bin/batch-test"
cp "$root/test/ignore_test.ns" "$tmp/src/main.ns"
"$ns" build "$tmp" >/dev/null
"$tmp/bin/batch-test"
