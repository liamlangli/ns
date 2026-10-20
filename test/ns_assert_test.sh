#!/bin/sh

# A failing assert in a compiled program flushes what it printed and names the
# assert on stderr before it aborts. It used to be a bare trap: with stdout on a
# pipe every earlier print was lost and the process died of SIGTRAP with no
# message. The ns_cpu interpreter and the AST interpreter report the same way.

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

ns=$1

tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-assert.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cat > "$tmp/assert.ns" <<'NS'
use std

fn main() {
    print("before\n")
    assert false
    print("after\n")
}
NS

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    printf -- '--- stdout\n%s\n--- stderr\n' "$out" >&2
    cat "$tmp/err" >&2
    exit 1
}

# Runs "$@" with stdout on a pipe, leaving stdout in $out, stderr in
# $tmp/err and the exit status in $status.
run_piped() {
    set +e
    out=$( ("$@" 2>"$tmp/err"; echo "status=$?" >"$tmp/status") 2>/dev/null | cat)
    set -e
    status=$(sed 's/^status=//' "$tmp/status")
}

# check <label> <diagnostic text>
check() {
    [ "$status" -ne 0 ] || fail "$1: a failing assert exited 0"
    [ "$out" = "before" ] || fail "$1: the print before the assert was not flushed to the pipe"
    grep -q "$2" "$tmp/err" || fail "$1: stderr does not report '$2'"
}

case "$(uname -s)-$(uname -m)" in
Darwin-arm64|Linux-x86_64)
    "$ns" build --exe "$tmp/assert.ns" -o "$tmp/assert" >/dev/null
    run_piped "$tmp/assert"
    check native "assertion failed at .*assert.ns:5"
    ;;
*)
    printf '%s\n' 'SKIP: native assert requires a native compile path.'
    ;;
esac

run_piped "$ns" run --cpu "$tmp/assert.ns"
check ns_cpu "assertion failed at .*assert.ns:5"

run_piped "$ns" run "$tmp/assert.ns"
check interpreter "assertion failed"

printf '%s\n' 'PASS: failing asserts flush stdout and report their location'
