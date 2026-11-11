#!/bin/sh
# Parser errors, locations and exit status must match with memoization on/off.
set -eu
ns=$1
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "$root/bin/ns-ast-cache.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
check() {
    name=$1
    cpu=0
    cached=0
    NS_PARSE_TOKEN_CACHE=0 "$ns" --ast "$work/$name.ns" > "$work/plain" 2>&1 || cpu=$?
    NS_PARSE_TOKEN_CACHE=1 "$ns" --ast "$work/$name.ns" > "$work/cached" 2>&1 || cached=$?
    test "$cpu" -ne 0
    test "$cpu" -eq "$cached"
    diff -u "$work/plain" "$work/cached"
}
printf 'fn main() {\n let x = [i32](4)\n x[0] =\n }\n' > "$work/expression.ns"
printf 'fn main(\n' > "$work/signature.ns"
printf 'fn main() { let text = "unterminated\n' > "$work/string.ns"
check expression
check signature
check string
echo 'ns_ast_cache_test: parser diagnostics and exit status match'
