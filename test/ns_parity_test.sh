#!/bin/sh

# Run the same .ns sources through the interpreter and the AArch64 compile
# path. Exit status (and stdout, when the compiled program prints) must match.

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

ns=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ "$(uname -s)" != "Darwin" ]; then
    printf '%s\n' 'SKIP: compile/interpret parity requires Darwin arm64.'
    exit 0
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-parity.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fail=0
run_one() {
    src=$1
    name=$(basename "$src" .ns)
    set +e
    "$ns" run "$src" >"$tmp/$name.int.out" 2>"$tmp/$name.int.err"
    int_status=$?
    set -e
    "$ns" build --exe "$src" -o "$tmp/$name.bin" >/dev/null
    set +e
    "$tmp/$name.bin" >"$tmp/$name.cmp.out" 2>"$tmp/$name.cmp.err"
    cmp_status=$?
    set -e
    if [ "$int_status" -ne "$cmp_status" ]; then
        printf '%s\n' "FAIL: $name interpret exit $int_status, compile exit $cmp_status." >&2
        fail=1
        return
    fi
    if ! cmp -s "$tmp/$name.int.out" "$tmp/$name.cmp.out"; then
        printf '%s\n' "FAIL: $name stdout differs between interpret and compile." >&2
        fail=1
        return
    fi
    printf '%s\n' "PASS: $name interpret/compile exit $int_status."
}

run_one "$root/test/parity_core.ns"
run_one "$root/test/parity_p1.ns"
run_one "$root/test/container_test.ns"
run_one "$root/test/fn_ret_test.ns"
run_one "$root/test/gen_expr_test.ns"
run_one "$root/test/parity_fnval.ns"
run_one "$root/test/parity_union.ns"
run_one "$root/test/parity_ref.ns"
run_one "$root/test/parity_task.ns"
run_one "$root/test/parity_ffi.ns"

if [ "$fail" -ne 0 ]; then
    exit 1
fi
printf '%s\n' 'PASS: compile/interpret parity for the current whitelist.'
