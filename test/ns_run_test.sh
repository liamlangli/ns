#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

ns=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if ! "$ns" --help | grep -q 'wasm server port (default 9001'; then
    printf '%s\n' 'FAIL: ns help does not report the wasm server default port 9001.' >&2
    exit 1
fi

scaffold_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-scaffold.XXXXXX")
run_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-run-test.XXXXXX")
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-test-test.XXXXXX")
trap 'rm -rf "$scaffold_tmp" "$run_tmp" "$test_tmp"' EXIT HUP INT TERM

cd "$scaffold_tmp"
"$ns" create created
for file in ns.mod main.ns README.md AGENTS.md .gitignore; do
    test -f "created/$file"
done
if grep -q 'test/' created/ns.mod; then
    printf '%s\n' 'FAIL: scaffold redundantly excludes default test sources.' >&2
    exit 1
fi
cmp "$root/AGENTS.md" created/AGENTS.md

mkdir initialized
"$ns" init initialized
cmp "$root/AGENTS.md" initialized/AGENTS.md

mkdir preserved
printf '%s\n' '# project-specific agent rules' > preserved/AGENTS.md
"$ns" init preserved
test "$(cat preserved/AGENTS.md)" = '# project-specific agent rules'

cd "$root"
"$ns" run test/lang_test.ns
"$ns" run test/container_test.ns
"$ns" run test/gen_expr_test.ns
"$ns" run test/to_str_test.ns
"$ns" run test/str_fmt_test.ns
"$ns" run test/fn_ret_test.ns
"$ns" run test/fn_overload_test.ns
"$ns" run test/os_file_test.ns
"$ns" run test/gpu_pipeline_test.ns
"$ns" run test/gpu_v2_test.ns
"$ns" run test/shader_host_test.ns
"$ns" run test/dynamic_test.ns
"$ns" run test/task_net_test.ns
if [ "$(uname -s)" = "Darwin" ]; then
    sh test/audio_apple_test.sh "$ns"
    sh test/os_haptic_apple_compile.sh
fi

mkdir -p "$run_tmp/main-only" "$run_tmp/manifest-priority" "$run_tmp/missing"

# A parent manifest must not be inherited by bare `ns run` from a child dir.
printf '%s\n' \
    'schema = "ns.mod/v1"' \
    'entry = "parent-entry.ns"' > "$run_tmp/ns.mod"
printf '%s\n' 'this parent entry must not be selected' > "$run_tmp/parent-entry.ns"

printf '%s\n' \
    'use std' \
    'use os' \
    'fn main() {' \
    '    let now = os_time_ms()' \
    '}' > "$run_tmp/main-only/main.ns"

printf '%s\n' \
    'schema = "ns.mod/v1"' \
    'entry = "entry.ns"' > "$run_tmp/manifest-priority/ns.mod"
printf '%s\n' 'use std' 'fn main() {' '}' > "$run_tmp/manifest-priority/entry.ns"
# This remains project source, but must not replace the manifest entry.
printf '%s\n' 'fn project_helper() i32 {' '    return 1' '}' > "$run_tmp/manifest-priority/main.ns"

(cd "$run_tmp/main-only" && "$ns" run)
(cd "$run_tmp/manifest-priority" && "$ns" run)

if (cd "$run_tmp/missing" && "$ns" run > "$run_tmp/missing.out" 2>&1); then
    printf '%s\n' 'FAIL: ns run succeeded without a local ns.mod or main.ns.' >&2
    exit 1
fi

if ! grep -q 'neither ns.mod nor main.ns was found' "$run_tmp/missing.out"; then
    printf '%s\n' 'FAIL: ns run did not report both missing implicit inputs.' >&2
    exit 1
fi

printf '%s\n' 'PASS: ns run selects cwd/ns.mod, then cwd/main.ns, without walking upward.'

mkdir -p "$run_tmp/recursive/src/nested" "$run_tmp/recursive/src/ignored" "$run_tmp/recursive/src/wild" "$run_tmp/recursive/src/test"
printf '%s\n' \
    'schema = "ns.mod/v1"' \
    'name = "recursive-project"' \
    'version = "0.1.0"' \
    'type = "app"' \
    'source = "src"' \
    'entry = "main.ns"' \
    'exclude = ["ignored/", "nested/excluded.ns", "wild/*.ns"]' > "$run_tmp/recursive/ns.mod"
printf '%s\n' \
    'fn main() i32 {' \
    '    assert recursive_answer() == 42' \
    '    return 0' \
    '}' > "$run_tmp/recursive/src/main.ns"
printf '%s\n' \
    'use std' \
    'fn recursive_answer() i32 {' \
    '    return 42' \
    '}' > "$run_tmp/recursive/src/nested/answer.ns"
printf '%s\n' 'this excluded source must not be compiled' > "$run_tmp/recursive/src/nested/excluded.ns"
printf '%s\n' 'this excluded directory must not be compiled' > "$run_tmp/recursive/src/ignored/broken.ns"
printf '%s\n' 'this glob-excluded source must not be compiled' > "$run_tmp/recursive/src/wild/broken.ns"
printf '%s\n' 'this default-excluded test directory must not be compiled' > "$run_tmp/recursive/src/test/broken.ns"
printf '%s\n' 'this default-excluded test file must not be compiled' > "$run_tmp/recursive/src/unused_test.ns"

(cd "$run_tmp/recursive" && "$ns" run)

printf '%s\n' 'PASS: ns projects recursively link sources and honor manifest/default test excludes.'

mkdir -p "$test_tmp/project/test"
printf '%s\n' \
    'schema = "ns.mod/v1"' \
    'name = "test-discovery"' \
    'version = "0.1.0"' \
    'type = "app"' \
    'source = "."' \
    'entry = "main.ns"' > "$test_tmp/project/ns.mod"
printf '%s\n' \
    'fn project_answer() i32 {' \
    '    return 42' \
    '}' > "$test_tmp/project/answer.ns"
# The application entry must not shadow the selected test entry's main.
printf '%s\n' \
    'fn main() i32 {' \
    '    return 1' \
    '}' > "$test_tmp/project/main.ns"
printf '%s\n' \
    'use answer' \
    'fn main() i32 {' \
    '    if project_answer() == 42 {' \
    '        return 0' \
    '    }' \
    '    return 1' \
    '}' > "$test_tmp/project/test/answer_test.ns"
# Root-level *_test.ns files are not project tests under the convention.
printf '%s\n' 'this file must not be selected' > "$test_tmp/project/root_test.ns"
# Non-test modules in test/ are ignored too.
printf '%s\n' 'this helper must not be selected' > "$test_tmp/project/test/helper.ns"

(cd "$test_tmp/project" && "$ns" test)
(cd "$test_tmp/project/test" && "$ns" test)
"$ns" test "$test_tmp/project"
"$ns" test "$test_tmp/project/test"
"$ns" test "$test_tmp/project/test/answer_test.ns"

printf '%s\n' 'PASS: ns test discovers <project>/test/*_test.ns without manifest entries.'

if [ "$(uname -s)" = "Darwin" ]; then
    build_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-build-native.XXXXXX")
    trap 'rm -rf "$scaffold_tmp" "$run_tmp" "$test_tmp" "$build_tmp"' EXIT HUP INT TERM

    printf '%s\n' \
        'fn add(a: i32, b: i32) i32 {' \
        '    return a + b' \
        '}' \
        'fn main() i32 {' \
        '    return add(40, 2)' \
        '}' > "$build_tmp/add.ns"
    "$ns" build --exe "$build_tmp/add.ns" -o "$build_tmp/add"
    if ! file "$build_tmp/add" | grep -q 'Mach-O 64-bit executable arm64'; then
        printf '%s\n' 'FAIL: ns build --exe did not emit a Mach-O arm64 executable.' >&2
        exit 1
    fi
    if strings "$build_tmp/add" | grep -q 'nscode-native launcher'; then
        printf '%s\n' 'FAIL: ns build --exe still embeds the ns-run launcher.' >&2
        exit 1
    fi
    set +e
    "$build_tmp/add"
    add_status=$?
    set -e
    if [ "$add_status" -ne 42 ]; then
        printf '%s\n' "FAIL: compiled add binary exited $add_status, expected 42." >&2
        exit 1
    fi

    mkdir -p "$build_tmp/app"
    printf '%s\n' \
        'schema = "ns.mod/v1"' \
        'name = "tiny-app"' \
        'version = "0.1.0"' \
        'type = "app"' \
        'source = "."' \
        'entry = "main.ns"' > "$build_tmp/app/ns.mod"
    printf '%s\n' \
        'fn main() i32 {' \
        '    return 7' \
        '}' > "$build_tmp/app/main.ns"
    "$ns" build "$build_tmp/app"
    app_bin="$build_tmp/app/bin/tiny-app.app/Contents/MacOS/tiny-app"
    if [ ! -x "$app_bin" ]; then
        printf '%s\n' 'FAIL: ns build app did not produce Contents/MacOS/tiny-app.' >&2
        exit 1
    fi
    if ! file "$app_bin" | grep -q 'Mach-O 64-bit executable arm64'; then
        printf '%s\n' 'FAIL: ns build app did not emit a Mach-O arm64 executable.' >&2
        exit 1
    fi
    if strings "$app_bin" | grep -Eq 'nscode-native launcher|execl\(ns'; then
        printf '%s\n' 'FAIL: ns build app still wraps ns run instead of compiling.' >&2
        exit 1
    fi
    set +e
    "$app_bin"
    app_status=$?
    set -e
    if [ "$app_status" -ne 7 ]; then
        printf '%s\n' "FAIL: compiled app exited $app_status, expected 7." >&2
        exit 1
    fi

    printf '%s\n' \
        'use std' \
        'struct point { x: i32, y: i32 }' \
        'let acc = 1' \
        'fn main() i32 {' \
        '    let s = "hi" + "!"' \
        '    let a = [i32](2)' \
        '    a[0] = 10' \
        '    a[1] = 20' \
        '    let p = point { 3, 4 }' \
        '    let x: f64 = 1.5' \
        '    acc = acc + a[0] + p.x' \
        '    if s.len == 3 && a[1] == 20 && x + 2.5 == 4.0 && acc == 14 {' \
        '        return 0' \
        '    }' \
        '    return 1' \
        '}' > "$build_tmp/cover.ns"
    "$ns" build --exe "$build_tmp/cover.ns" -o "$build_tmp/cover"
    set +e
    "$build_tmp/cover"
    cover_status=$?
    set -e
    if [ "$cover_status" -ne 0 ]; then
        printf '%s\n' "FAIL: compiled string/array/struct/float program exited $cover_status." >&2
        exit 1
    fi

    printf '%s\n' 'PASS: ns build compiles native machine code for --exe and type=app.'
fi
