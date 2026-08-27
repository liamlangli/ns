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
for file in ns.mod src/main.ns README.md AGENTS.md .gitignore; do
    test -f "created/$file"
done
if [ -f created/main.ns ]; then
    printf '%s\n' 'FAIL: scaffold wrote main.ns at the project root instead of src/.' >&2
    exit 1
fi
if ! grep -q '^source = "src"$' created/ns.mod; then
    printf '%s\n' 'FAIL: scaffold manifest does not point source at src/.' >&2
    exit 1
fi
(cd created && "$ns" run > run.out 2>&1) || { cat created/run.out >&2; exit 1; }
if ! grep -q 'hello, created' created/run.out; then
    printf '%s\n' 'FAIL: scaffolded project does not run from src/main.ns.' >&2
    cat created/run.out >&2
    exit 1
fi
rm -f created/run.out
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
"$ns" run test/io_glb_test.ns
"$ns" run test/gpu_pipeline_test.ns
"$ns" run test/gpu_v2_test.ns
"$ns" run test/shader_host_test.ns
"$ns" run test/dynamic_test.ns
"$ns" run test/compress_test.ns
"$ns" run test/storage_test.ns
"$ns" run test/task_net_test.ns
"$ns" run test/net_udp_test.ns
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

mkdir -p "$run_tmp/targets/src" "$run_tmp/target-exclude/src/broken"
printf '%s\n' \
    'schema = "ns.mod/v1"' \
    'name = "multi-target"' \
    'version = "0.1.0"' \
    'type = "app"' \
    'source = "src"' \
    '' \
    '[[targets]]' \
    'name = "cli"' \
    'entry = "main.ns"' \
    'default = true' \
    '' \
    '[[targets]]' \
    'name = "tool"' \
    'entry = "tool_main.ns"' \
    'output = "tool-artifact"' > "$run_tmp/targets/ns.mod"
# Both entries declare main: linking one target must drop the other's entry.
printf '%s\n' \
    'use std' \
    'fn main() {' \
    '    assert target_shared_answer() == 42' \
    '    print("ran cli\n")' \
    '}' > "$run_tmp/targets/src/main.ns"
printf '%s\n' \
    'use std' \
    'fn main() {' \
    '    assert target_shared_answer() == 42' \
    '    print("ran tool\n")' \
    '}' > "$run_tmp/targets/src/tool_main.ns"
printf '%s\n' \
    'fn target_shared_answer() i32 {' \
    '    return 42' \
    '}' > "$run_tmp/targets/src/shared.ns"

run_target_out() {
    (cd "$run_tmp/targets" && "$ns" run ${1:+"$1"} > "$run_tmp/targets.out" 2>&1) \
        || { cat "$run_tmp/targets.out" >&2; exit 1; }
    if ! grep -q "$2" "$run_tmp/targets.out"; then
        printf '%s\n' "FAIL: ns run ${1:-<default>} did not run the $2 target." >&2
        cat "$run_tmp/targets.out" >&2
        exit 1
    fi
}

# No argument selects `default = true`, a bare name selects by target name, and
# the declared entry path of a target selects the same target.
run_target_out '' 'ran cli'
run_target_out cli 'ran cli'
run_target_out tool 'ran tool'
run_target_out src/tool_main.ns 'ran tool'

if (cd "$run_tmp/targets" && "$ns" run missing > "$run_tmp/targets.out" 2>&1); then
    printf '%s\n' 'FAIL: ns run accepted a target that ns.mod does not declare.' >&2
    exit 1
fi
if ! grep -q 'unknown target `missing`' "$run_tmp/targets.out" || ! grep -q 'cli, tool' "$run_tmp/targets.out"; then
    printf '%s\n' 'FAIL: ns run did not report the declared targets for an unknown name.' >&2
    cat "$run_tmp/targets.out" >&2
    exit 1
fi

# A per-target exclude is added to the project exclude list for that target.
printf '%s\n' \
    'schema = "ns.mod/v1"' \
    'name = "target-exclude"' \
    'version = "0.1.0"' \
    'type = "app"' \
    'source = "src"' \
    '' \
    '[[targets]]' \
    'name = "narrow"' \
    'entry = "main.ns"' \
    'exclude = ["broken/"]' > "$run_tmp/target-exclude/ns.mod"
printf '%s\n' \
    'use std' \
    'fn main() {' \
    '}' > "$run_tmp/target-exclude/src/main.ns"
printf '%s\n' 'this target-excluded source must not be compiled' > "$run_tmp/target-exclude/src/broken/broken.ns"

(cd "$run_tmp/target-exclude" && "$ns" run narrow)

printf '%s\n' 'PASS: ns run selects ns.mod targets, drops the other target entries, and honors per-target excludes.'

# Extra arguments after the target are program args, not extra input paths.
mkdir -p "$run_tmp/args/src"
printf '%s\n' \
    'schema = "ns.mod/v1"' \
    'name = "run-args"' \
    'version = "0.1.0"' \
    'type = "app"' \
    'source = "src"' \
    '' \
    '[[targets]]' \
    'name = "echo"' \
    'entry = "main.ns"' \
    'type = "cli"' > "$run_tmp/args/ns.mod"
printf '%s\n' \
    'use std' \
    'use os' \
    '' \
    'fn main() {' \
    '    print(os_env("NS_ARGC"))' \
    '    print("\n")' \
    '    print(os_env("NS_ARG0"))' \
    '    print("\n")' \
    '    print(os_env("NS_ARG1"))' \
    '    print("\n")' \
    '    print(os_env("NS_ARG2"))' \
    '    print("\n")' \
    '}' > "$run_tmp/args/src/main.ns"

if ! (cd "$run_tmp/args" && "$ns" run echo --size 256 model.glb out.vox > "$run_tmp/args.out" 2>&1); then
    printf '%s\n' 'FAIL: ns run rejected extra arguments after the target.' >&2
    cat "$run_tmp/args.out" >&2
    exit 1
fi
if ! grep -q 'too many input paths' "$run_tmp/args.out"; then
    :
else
    printf '%s\n' 'FAIL: ns run still treats program arguments as input paths.' >&2
    cat "$run_tmp/args.out" >&2
    exit 1
fi
if ! awk 'BEGIN{n=0} {n++} END{if(n<4) exit 1}' "$run_tmp/args.out"; then
    printf '%s\n' 'FAIL: ns run did not print the published program arguments.' >&2
    cat "$run_tmp/args.out" >&2
    exit 1
fi
arg_count=$(sed -n '1p' "$run_tmp/args.out")
arg0=$(sed -n '2p' "$run_tmp/args.out")
arg1=$(sed -n '3p' "$run_tmp/args.out")
arg2=$(sed -n '4p' "$run_tmp/args.out")
if [ "$arg_count" != 4 ] || [ "$arg0" != --size ] || [ "$arg1" != 256 ] || [ "$arg2" != model.glb ]; then
    printf '%s\n' 'FAIL: ns run did not publish NS_ARG* from the command line.' >&2
    cat "$run_tmp/args.out" >&2
    exit 1
fi
printf '%s\n' 'PASS: ns run publishes remaining arguments as NS_ARG*.'

mkdir -p "$test_tmp/project/test" "$test_tmp/project/browser"
printf '%s\n' \
    'schema = "ns.mod/v1"' \
    'name = "test-discovery"' \
    'version = "0.1.0"' \
    'type = "app"' \
    'source = "."' \
    '' \
    '[[targets]]' \
    'name = "app"' \
    'entry = "main.ns"' \
    'default = true' \
    'exclude = ["browser/"]' \
    '' \
    '[[targets]]' \
    'name = "browser"' \
    'entry = "browser_main.ns"' \
    'exclude = ["answer.ns"]' > "$test_tmp/project/ns.mod"
printf '%s\n' \
    'fn project_answer() i32 {' \
    '    return 42' \
    '}' > "$test_tmp/project/answer.ns"
# The application entry must not shadow the selected test entry's main.
printf '%s\n' \
    'fn main() i32 {' \
    '    return 1' \
    '}' > "$test_tmp/project/main.ns"
# An ordinary test uses the default target's source set, while a test named
# after another target uses that target's source set. The duplicate helper
# makes either test fail if the wrong exclusion list is selected.
printf '%s\n' \
    'fn project_answer() i32 {' \
    '    return 24' \
    '}' > "$test_tmp/project/browser/answer.ns"
printf '%s\n' \
    'fn main() i32 {' \
    '    return 1' \
    '}' > "$test_tmp/project/browser_main.ns"
printf '%s\n' \
    'use answer' \
    'fn main() i32 {' \
    '    if project_answer() == 42 {' \
    '        return 0' \
    '    }' \
    '    return 1' \
    '}' > "$test_tmp/project/test/answer_test.ns"
printf '%s\n' \
    'fn main() i32 {' \
    '    if project_answer() == 24 {' \
    '        return 0' \
    '    }' \
    '    return 1' \
    '}' > "$test_tmp/project/test/browser_test.ns"
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

    (cd "$run_tmp/targets" && "$ns" build tool)
    (cd "$run_tmp/targets" && "$ns" build cli)
    # Each target owns bin/<target name>; `output` renames the artifact inside it.
    for artifact in "tool/tool-artifact.app/Contents/MacOS/tool-artifact" "cli/cli.app/Contents/MacOS/cli"; do
        if [ ! -x "$run_tmp/targets/bin/$artifact" ]; then
            printf '%s\n' "FAIL: ns build did not write bin/$artifact for its target." >&2
            exit 1
        fi
    done
    set +e
    "$run_tmp/targets/bin/tool/tool-artifact.app/Contents/MacOS/tool-artifact" > "$run_tmp/targets.out" 2>&1
    tool_status=$?
    set -e
    if [ "$tool_status" -ne 0 ] || ! grep -q 'ran tool' "$run_tmp/targets.out"; then
        printf '%s\n' "FAIL: the compiled tool target exited $tool_status without running its own main." >&2
        cat "$run_tmp/targets.out" >&2
        exit 1
    fi

    # One manifest, three target types: a bare `ns build` builds them all, and a
    # named target builds alone.
    mkdir -p "$build_tmp/kinds/src"
    printf '%s\n' \
        'schema = "ns.mod/v1"' \
        'name = "kinds"' \
        'version = "0.1.0"' \
        'type = "app"' \
        'source = "src"' \
        '' \
        '[[targets]]' \
        'name = "kinds-app"' \
        'entry = "app_main.ns"' \
        'type = "app"' \
        'default = true' \
        '' \
        '[[targets]]' \
        'name = "kinds-cli"' \
        'entry = "cli_main.ns"' \
        'type = "cli"' \
        '' \
        '[[targets]]' \
        'name = "kinds-lib"' \
        'entry = "lib_root.ns"' \
        'type = "lib"' > "$build_tmp/kinds/ns.mod"
    printf '%s\n' 'fn main() i32 {' '    return kinds_shared() - 42' '}' > "$build_tmp/kinds/src/app_main.ns"
    printf '%s\n' \
        'use std' \
        'fn main() i32 {' \
        '    print("kinds cli\n")' \
        '    return 0' \
        '}' > "$build_tmp/kinds/src/cli_main.ns"
    printf '%s\n' 'fn kinds_api() i32 {' '    return kinds_shared()' '}' > "$build_tmp/kinds/src/lib_root.ns"
    printf '%s\n' 'fn kinds_shared() i32 {' '    return 42' '}' > "$build_tmp/kinds/src/shared.ns"

    (cd "$build_tmp/kinds" && "$ns" build)
    for artifact in "kinds-app/kinds-app.app/Contents/MacOS/kinds-app" "kinds-cli/kinds-cli" "kinds-lib/libkinds-lib.a"; do
        if [ ! -e "$build_tmp/kinds/bin/$artifact" ]; then
            printf '%s\n' "FAIL: ns build did not write bin/$artifact for its target type." >&2
            ls "$build_tmp/kinds/bin" >&2
            exit 1
        fi
    done
    if file "$build_tmp/kinds/bin/kinds-cli/kinds-cli" | grep -q 'Mach-O 64-bit executable arm64'; then
        :
    else
        printf '%s\n' 'FAIL: type = "cli" did not emit a plain Mach-O executable.' >&2
        exit 1
    fi
    if ! file "$build_tmp/kinds/bin/kinds-lib/libkinds-lib.a" | grep -q 'archive'; then
        printf '%s\n' 'FAIL: type = "lib" did not emit a static library archive.' >&2
        exit 1
    fi
    set +e
    "$build_tmp/kinds/bin/kinds-cli/kinds-cli" > "$build_tmp/kinds.out" 2>&1
    kinds_status=$?
    set -e
    if [ "$kinds_status" -ne 0 ] || ! grep -q 'kinds cli' "$build_tmp/kinds.out"; then
        printf '%s\n' "FAIL: the compiled cli target exited $kinds_status without running its own main." >&2
        cat "$build_tmp/kinds.out" >&2
        exit 1
    fi

    # A named target rebuilds only itself.
    rm -rf "$build_tmp/kinds/bin"
    (cd "$build_tmp/kinds" && "$ns" build kinds-cli)
    if [ ! -x "$build_tmp/kinds/bin/kinds-cli/kinds-cli" ]; then
        printf '%s\n' 'FAIL: ns build kinds-cli did not build the named target.' >&2
        exit 1
    fi
    if [ -e "$build_tmp/kinds/bin/kinds-app" ] || [ -e "$build_tmp/kinds/bin/kinds-lib/libkinds-lib.a" ]; then
        printf '%s\n' 'FAIL: ns build kinds-cli also built the other targets.' >&2
        ls "$build_tmp/kinds/bin" >&2
        exit 1
    fi

    # One process, several targets: a native target's non-portable `use` must
    # not follow the compiler into a browser target that excludes it. `--profile`
    # keeps the targets serial in this process, which is where they share an AST
    # context.
    mkdir -p "$build_tmp/mixed/src/web"
    printf '%s\n' \
        'schema = "ns.mod/v1"' \
        'name = "mixed"' \
        'version = "0.1.0"' \
        'type = "app"' \
        'source = "src"' \
        '' \
        '[[targets]]' \
        'name = "mixed-cli"' \
        'entry = "cli_main.ns"' \
        'type = "cli"' \
        'exclude = ["src/web"]' \
        '' \
        '[[targets]]' \
        'name = "mixed-web"' \
        'entry = "web/web_main.ns"' \
        'platform = "wasm"' \
        'exclude = ["src/*.ns"]' > "$build_tmp/mixed/ns.mod"
    printf '%s\n' \
        'use std' \
        'use io' \
        'fn main() {' \
        '    print("mixed cli\n")' \
        '}' > "$build_tmp/mixed/src/cli_main.ns"
    printf '%s\n' \
        'use std' \
        'fn main() {' \
        '    print("mixed web\n")' \
        '}' > "$build_tmp/mixed/src/web/web_main.ns"
    if ! (cd "$build_tmp/mixed" && "$ns" build --profile > "$build_tmp/mixed.out" 2>&1); then
        cat "$build_tmp/mixed.out" >&2
        printf '%s\n' 'FAIL: a serial multi-target build did not build every target.' >&2
        exit 1
    fi
    if grep -q 'does not support module' "$build_tmp/mixed.out"; then
        cat "$build_tmp/mixed.out" >&2
        printf '%s\n' 'FAIL: a native target leaked its `use` into the browser target.' >&2
        exit 1
    fi
    for artifact in "mixed-cli/mixed-cli" "mixed-web/mixed-web.wasm"; do
        if [ ! -e "$build_tmp/mixed/bin/$artifact" ]; then
            cat "$build_tmp/mixed.out" >&2
            printf 'FAIL: a serial multi-target build did not write bin/%s.\n' "$artifact" >&2
            exit 1
        fi
    done
    printf '%s\n' 'PASS: targets compiled in one process keep their own module set.'

    if (cd "$build_tmp/kinds" && "$ns" build -o bin/single > "$build_tmp/kinds.out" 2>&1); then
        printf '%s\n' 'FAIL: ns build -o accepted a manifest with several targets.' >&2
        exit 1
    fi
    if ! grep -q '\-o takes a single target' "$build_tmp/kinds.out"; then
        printf '%s\n' 'FAIL: ns build -o did not explain the multi-target conflict.' >&2
        cat "$build_tmp/kinds.out" >&2
        exit 1
    fi

    printf '%s\n' 'PASS: ns build compiles native machine code for --exe and type=app.'
    printf '%s\n' 'PASS: ns build writes one artifact per ns.mod target.'
    printf '%s\n' 'PASS: ns build emits app/cli/library artifacts per target type and builds one by name.'
fi
