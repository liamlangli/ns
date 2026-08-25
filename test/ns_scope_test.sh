#!/bin/sh

# A compiled program hands back what a call did not publish, so one that
# allocates many times the size of the heap still runs. Before that, the heap
# only ever grew and a program like this aborted with "ns_rt: heap overflow"
# once it had used up the 4 GiB of addresses the heap has.

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

ns=$1

if [ "$(uname -s)" != "Darwin" ]; then
    printf '%s\n' 'SKIP: heap reclamation requires the AArch64 compile path.'
    exit 0
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-scope.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cat > "$tmp/scope.ns" <<'NS'
use std

let checksum = 0
let keep = [i32](8)

// One frame's worth of work: 16 KiB of scratch that nobody keeps, and one
// number that outlives the call.
fn frame(round: i32) {
    let scratch = [f32](4096)
    scratch[0] = round as f32
    scratch[4095] = (round * 2) as f32
    checksum = (checksum + (scratch[0] as i32) + (scratch[4095] as i32)) % 1000003
    keep[round % 8] = checksum
}

fn main() {
    // 600000 rounds is about 9 GiB, more than twice the address space the heap
    // has: the run only reaches the end if each round's memory comes back.
    for round in 0 to 600000 {
        frame(round)
    }
    print(`checksum {checksum} kept {keep[0]}\n`)
}
NS

"$ns" build --exe "$tmp/scope.ns" -o "$tmp/scope" >/dev/null

set +e
out=$("$tmp/scope" 2>"$tmp/scope.err")
status=$?
set -e

if [ "$status" -ne 0 ]; then
    printf '%s\n' "FAIL: a program allocating past the heap size exited $status." >&2
    sed -n '1,5p' "$tmp/scope.err" >&2
    exit 1
fi

case "$out" in
    "checksum "*" kept "*) ;;
    *)
        printf '%s\n' "FAIL: unexpected output: $out" >&2
        exit 1
        ;;
esac

printf '%s\n' "PASS: released memory is reused, so a long run stays inside the heap."
