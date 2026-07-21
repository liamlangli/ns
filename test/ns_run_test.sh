#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

ns=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

scaffold_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-scaffold.XXXXXX")
run_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-run-test.XXXXXX")
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-test-test.XXXXXX")
trap 'rm -rf "$scaffold_tmp" "$run_tmp" "$test_tmp"' EXIT HUP INT TERM

cd "$scaffold_tmp"
"$ns" create created
for file in ns.mod main.ns README.md AGENTS.md .gitignore; do
    test -f "created/$file"
done
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
"$ns" run test/os_file_test.ns
"$ns" run test/gpu_pipeline_test.ns
"$ns" run test/gpu_v2_test.ns
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
printf '%s\n' 'this main file must not be selected' > "$run_tmp/manifest-priority/main.ns"

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
