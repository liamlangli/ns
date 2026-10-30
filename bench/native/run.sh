#!/bin/sh
# Time each native-backend benchmark three ways: the `ns build --exe` artifact,
# the ns_cpu interpreter (`ns run --cpu`) and the AST interpreter (`ns run`).
# Usage: sh bench/native/run.sh [path/to/ns]
set -e
here=$(cd "$(dirname "$0")" && pwd)
ns=${1:-$here/../../bin/ns}
out=${TMPDIR:-/tmp}/ns_bench_native
mkdir -p "$out"

seconds() {
    /usr/bin/time -p "$@" 2>&1 >/dev/null | awk '/^real/ { print $2 }'
}

printf '%-12s %8s %8s %8s\n' bench native cpu eval
for src in "$here"/*.ns; do
    name=$(basename "$src" .ns)
    "$ns" build --exe "$src" -o "$out/$name" >/dev/null 2>&1
    expect=$("$ns" run --cpu "$src")
    got=$("$out/$name")
    if [ "$got" != "$expect" ]; then
        echo "$name: native printed '$got', ns_cpu printed '$expect'" >&2
        exit 1
    fi
    printf '%-12s %8s %8s %8s\n' "$name" "$(seconds "$out/$name")" \
        "$(seconds "$ns" run --cpu "$src")" "$(seconds "$ns" run "$src")"
done
