#!/bin/sh

# A compiled program hands back what a call did not publish, so one that
# allocates many times the size of the heap still runs. Before that, the heap
# only ever grew and a program like this aborted with "ns_rt: heap overflow"
# once it had used up the 4 GiB of addresses the heap has. The reclamation is
# the runtime's, so every host with a native compile path runs this.

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

ns=$1

case "$(uname -s)-$(uname -m)" in
Darwin-arm64|Linux-x86_64) ;;
*)
    printf '%s\n' 'SKIP: heap reclamation requires a native compile path.'
    exit 0
    ;;
esac

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

# A function returning a string used to leave its scratch to the caller, where
# storing the result into a global kept it for good. The result now moves down
# to the release point. 200000 calls of 16 KiB scratch is about 3 GiB.
cat > "$tmp/strret.ns" <<'NS'
use std

let last = ""
let total = 0

fn label(round: i32) str {
    let scratch = [f32](4096)
    scratch[0] = round as f32
    let head = substr(`prefix-{round}`, 0, 3)
    return head + `-{(scratch[0] as i32) % 10}`
}

fn main() {
    for round in 0 to 200000 {
        last = label(round)
        total = total + last.len
    }
    print(`{last} {total}\n`)
}
NS

"$ns" build --exe "$tmp/strret.ns" -o "$tmp/strret" >/dev/null
if ! out=$("$tmp/strret" 2>"$tmp/strret.err"); then
    printf '%s\n' "FAIL: string results kept their callee's scratch." >&2
    sed -n '1,5p' "$tmp/strret.err" >&2
    exit 1
fi
if [ "$out" != "pre-9 1000000" ]; then
    printf '%s\n' "FAIL: moved string results read back wrong: $out" >&2
    exit 1
fi

# Storing into containers - a fresh local one, or the caller's from a worker
# thread - and dispatching a worker every frame used to pin the whole frame for
# good. 200000 frames of 16 KiB scratch is about 3 GiB.
cat > "$tmp/publish.ns" <<'NS'
use std
use task

let checksum = 0

fn frame(round: i32) {
    let scratch = [f32](4096)
    scratch[0] = round as f32
    let local = [str](1)
    local[0] = `l{round}`
    let out = [str](1)
    let pending = dispatch(queue_worker) { in
        let tmp = [f32](4096)
        tmp[1] = 3.0
        out[0] = `w{round}`
    }
    wait(pending)
    checksum = (checksum + (scratch[0] as i32) + out[0].len + local[0].len) % 1000003
}

fn main() {
    for round in 0 to 200000 {
        frame(round)
    }
    print(`checksum {checksum}\n`)
}
NS

"$ns" build --exe "$tmp/publish.ns" -o "$tmp/publish" >/dev/null
if ! out=$("$tmp/publish" 2>"$tmp/publish.err"); then
    printf '%s\n' "FAIL: storing into containers and dispatching workers leaked the heap." >&2
    sed -n '1,5p' "$tmp/publish.err" >&2
    exit 1
fi
case "$out" in
    "checksum "*) ;;
    *)
        printf '%s\n' "FAIL: unexpected output: $out" >&2
        exit 1
        ;;
esac

printf '%s\n' "PASS: released memory is reused, so a long run stays inside the heap."
