#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

ns=$1

if ! "$ns" --help | grep -q 'lint_fix \[path\]'; then
    printf '%s\n' 'FAIL: ns help does not document lint_fix.' >&2
    exit 1
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-lint-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp/project/src/vendor" "$tmp/project/test"
printf '%s\n' \
    'schema = "ns.mod/v1"' \
    'name = "lint-project"' \
    'version = "0.1.0"' \
    'type = "app"' \
    'source = "src"' \
    'entry = "main.ns"' \
    'exclude = ["vendor/"]' > "$tmp/project/ns.mod"

printf '%s\n' \
    'use std' \
    '' \
    'struct point {' \
    '    x: f32,' \
    '    y: f32' \
    '}' \
    '' \
    'fn main() {' \
    '    let p = point { x: 1, y: 2 }' \
    '    let n = 1+2' \
    '    print(`{p.x} {n}\n`)' \
    '}' > "$tmp/project/src/main.ns"
printf '%s\n' 'let vendored = 1+2' > "$tmp/project/src/vendor/skipped.ns"

# A findings-free run exits 0; an error-severity finding exits 1 so CI can gate.
if (cd "$tmp/project" && "$ns" lint > "$tmp/lint.out" 2>&1); then
    printf '%s\n' 'FAIL: ns lint exited 0 with an error-severity finding.' >&2
    cat "$tmp/lint.out" >&2
    exit 1
fi

if ! grep -q 'binary_op_space' "$tmp/lint.out"; then
    printf '%s\n' 'FAIL: ns lint did not report the unspaced binary operator.' >&2
    cat "$tmp/lint.out" >&2
    exit 1
fi

if ! grep -q 'struct_label' "$tmp/lint.out"; then
    printf '%s\n' 'FAIL: ns lint did not report the fully ordered struct literal.' >&2
    cat "$tmp/lint.out" >&2
    exit 1
fi

if grep -q 'skipped.ns' "$tmp/lint.out"; then
    printf '%s\n' 'FAIL: ns lint scanned a path excluded by ns.mod.' >&2
    exit 1
fi

(cd "$tmp/project" && "$ns" lint_fix > "$tmp/fix.out" 2>&1)
grep -q 'let n = 1 + 2' "$tmp/project/src/main.ns"
grep -q 'let p = point { 1, 2 }' "$tmp/project/src/main.ns"
grep -q 'let vendored = 1+2' "$tmp/project/src/vendor/skipped.ns"

# The fixed project is clean, still runs, and `ns lint --fix` is the same command.
(cd "$tmp/project" && "$ns" lint)
(cd "$tmp/project" && "$ns" lint --fix)
(cd "$tmp/project" && "$ns" run | grep -q '1.00 3')

printf '%s\n' 'PASS: ns lint reports project findings and ns lint_fix rewrites them.'

# The [lint] table customizes the rules; a disabled rule stops reporting.
printf '%s\n' \
    'schema = "ns.mod/v1"' \
    'name = "lint-config"' \
    'version = "0.1.0"' \
    'type = "app"' \
    'source = "."' \
    'entry = "main.ns"' \
    '' \
    '[lint]' \
    'nested_name_max = 4' \
    'binary_op_space = "off"' \
    'snake_case = "off"' > "$tmp/config-ns.mod"

mkdir -p "$tmp/config"
cp "$tmp/config-ns.mod" "$tmp/config/ns.mod"
printf '%s\n' \
    'use std' \
    '' \
    'fn main() {' \
    '    let f = { value in' \
    '        return value+1' \
    '    }' \
    '    print(`{f(1)}\n`)' \
    '}' > "$tmp/config/main.ns"

(cd "$tmp/config" && "$ns" lint > "$tmp/config.out" 2>&1)
if grep -q 'binary_op_space' "$tmp/config.out"; then
    printf '%s\n' 'FAIL: [lint] binary_op_space = "off" still reported.' >&2
    exit 1
fi
if ! grep -q 'nested_name' "$tmp/config.out"; then
    printf '%s\n' 'FAIL: [lint] nested_name_max = 4 did not report a 5 char nested name.' >&2
    cat "$tmp/config.out" >&2
    exit 1
fi
# A disabled rule must not be rewritten either.
(cd "$tmp/config" && "$ns" lint_fix > /dev/null 2>&1)
grep -q 'return value+1' "$tmp/config/main.ns"

# snake_case is on by default and lint_fix rewrites camelCase identifiers.
printf '%s\n' \
    'fn main() {' \
    '    let fooBar = 1' \
    '}' > "$tmp/snake.ns"
if "$ns" lint "$tmp/snake.ns" > "$tmp/snake.out" 2>&1; then
    printf '%s\n' 'FAIL: ns lint exited 0 for a camelCase identifier.' >&2
    cat "$tmp/snake.out" >&2
    exit 1
fi
if ! grep -q 'snake_case' "$tmp/snake.out"; then
    printf '%s\n' 'FAIL: ns lint did not report a camelCase identifier.' >&2
    cat "$tmp/snake.out" >&2
    exit 1
fi
"$ns" lint_fix "$tmp/snake.ns" > /dev/null 2>&1
grep -q 'let foo_bar = 1' "$tmp/snake.ns"
"$ns" lint "$tmp/snake.ns"

# A project can disable the mandatory snake_case rule.
mkdir -p "$tmp/camel"
printf '%s\n' \
    'schema = "ns.mod/v1"' \
    'name = "camel"' \
    'version = "0.1.0"' \
    'type = "app"' \
    'source = "."' \
    'entry = "main.ns"' \
    '' \
    '[lint]' \
    'snake_case = "off"' > "$tmp/camel/ns.mod"
printf '%s\n' \
    'fn main() {' \
    '    let fooBar = 1' \
    '}' > "$tmp/camel/main.ns"
(cd "$tmp/camel" && "$ns" lint > "$tmp/camel.out" 2>&1) || true
if grep -q 'snake_case' "$tmp/camel.out"; then
    printf '%s\n' 'FAIL: [lint] snake_case = "off" still reported.' >&2
    cat "$tmp/camel.out" >&2
    exit 1
fi

# A single file argument needs no project at all.
printf '%s\n' 'fn main() {' '    let a = 1+2' '}' > "$tmp/loose.ns"
if "$ns" lint "$tmp/loose.ns" > "$tmp/loose.out" 2>&1; then
    printf '%s\n' 'FAIL: ns lint exited 0 for a file outside a project.' >&2
    exit 1
fi
grep -q 'binary_op_space' "$tmp/loose.out"
"$ns" lint_fix "$tmp/loose.ns" > /dev/null 2>&1
grep -q 'let a = 1 + 2' "$tmp/loose.ns"
"$ns" lint "$tmp/loose.ns"

printf '%s\n' 'PASS: the [lint] table customizes rules and a loose file lints on its own.'
