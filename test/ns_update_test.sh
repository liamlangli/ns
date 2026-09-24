#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

ns=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
update_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-update-test.XXXXXX")
trap 'rm -rf "$update_tmp"' EXIT HUP INT TERM

mkdir -p "$update_tmp/legacy/subdir"
printf '%s\n' \
    '# keep this comment' \
    'schema = "ns.mod/v0" # legacy schema' \
    'name = "legacy-project"' \
    'version = "2.3.4"' \
    'type = "library"' \
    'source = "src"' \
    'custom = "preserved"' > "$update_tmp/legacy/ns.mod"
printf '%s\n' '# old generated agent guide' > "$update_tmp/legacy/AGENTS.md"
printf '%s\n' 'dist/' > "$update_tmp/legacy/.gitignore"
printf '%s\n' 'source stays untouched' > "$update_tmp/legacy/main.ns"
(cd "$update_tmp/legacy/subdir" && "$ns" update)

grep -q '^schema = "ns.mod/v2" # legacy schema$' "$update_tmp/legacy/ns.mod"
grep -q '^custom = "preserved"$' "$update_tmp/legacy/ns.mod"
test "$(grep -c '^schema =' "$update_tmp/legacy/ns.mod")" -eq 1
cmp "$root/AGENTS.md" "$update_tmp/legacy/AGENTS.md"
grep -q '^dist/$' "$update_tmp/legacy/.gitignore"
grep -q '^bin$' "$update_tmp/legacy/.gitignore"
grep -q '^ns.profile$' "$update_tmp/legacy/.gitignore"
test "$(cat "$update_tmp/legacy/main.ns")" = 'source stays untouched'
grep -q '^schema = "ns.mod/v0"' "$update_tmp/legacy/bin/ns-update-backup/ns.mod"
test "$(cat "$update_tmp/legacy/bin/ns-update-backup/AGENTS.md")" = '# old generated agent guide'
test "$(cat "$update_tmp/legacy/bin/ns-update-backup/.gitignore")" = 'dist/'

cp "$update_tmp/legacy/ns.mod" "$update_tmp/manifest.after-first-update"
cp "$update_tmp/legacy/AGENTS.md" "$update_tmp/agents.after-first-update"
cp "$update_tmp/legacy/.gitignore" "$update_tmp/gitignore.after-first-update"
"$ns" update "$update_tmp/legacy" > "$update_tmp/idempotent.out"
cmp "$update_tmp/manifest.after-first-update" "$update_tmp/legacy/ns.mod"
cmp "$update_tmp/agents.after-first-update" "$update_tmp/legacy/AGENTS.md"
cmp "$update_tmp/gitignore.after-first-update" "$update_tmp/legacy/.gitignore"
grep -q 'already up to date' "$update_tmp/idempotent.out"

mkdir "$update_tmp/unversioned"
printf '%s\n' 'name = "unversioned"' 'type = "app"' > "$update_tmp/unversioned/ns.mod"
printf '%s\n' 'bin/' > "$update_tmp/unversioned/.gitignore"
"$ns" update "$update_tmp/unversioned/ns.mod"
test "$(sed -n '1p' "$update_tmp/unversioned/ns.mod")" = 'schema = "ns.mod/v2"'
grep -q '^name = "unversioned"$' "$update_tmp/unversioned/ns.mod"
test "$(grep -Ec '^bin/?$' "$update_tmp/unversioned/.gitignore")" -eq 1

# v1 run modes: `link` and machine triples become `target` / target_os /
# target_arch, per table, and every other line stays as written.
mkdir -p "$update_tmp/modes"
printf '%s\n' \
    'schema = "ns.mod/v1"' \
    'name = "modes"' \
    'link = true' \
    '' \
    '[[targets]]' \
    'name = "desktop"' \
    'entry = "main.ns"' \
    '' \
    '[[targets]]' \
    'name = "server"' \
    'entry = "main.ns"' \
    'platform = "x86_64-linux-gnu"' \
    '' \
    '[[targets]]' \
    'name = "tool"' \
    'entry = "main.ns"' \
    'platform = "x86_64-linux-gnu"' \
    'link = false' \
    '' \
    '[[targets]]' \
    'name = "web"' \
    'entry = "main.ns"' \
    'target = "wasm"' > "$update_tmp/modes/ns.mod"
"$ns" update "$update_tmp/modes" > /dev/null
cat > "$update_tmp/modes.expected" <<'EXPECTED'
schema = "ns.mod/v2"
name = "modes"
target = "exec"

[[targets]]
name = "desktop"
entry = "main.ns"

[[targets]]
name = "server"
entry = "main.ns"
target = "exec"
target_os = "linux"
target_arch = "x86_64"

[[targets]]
name = "tool"
entry = "main.ns"
target = "eval"
target_os = "linux"
target_arch = "x86_64"

[[targets]]
name = "web"
entry = "main.ns"
target = "wasm"
EXPECTED
if ! cmp -s "$update_tmp/modes.expected" "$update_tmp/modes/ns.mod"; then
    printf '%s\n' 'FAIL: ns update did not rewrite v1 run modes into target / target_os / target_arch.' >&2
    diff "$update_tmp/modes.expected" "$update_tmp/modes/ns.mod" >&2 || true
    exit 1
fi
if grep -q '^link\|^platform' "$update_tmp/modes/ns.mod"; then
    printf '%s\n' 'FAIL: ns update left a v1 link or platform key.' >&2
    exit 1
fi

mkdir "$update_tmp/future"
printf '%s\n' 'schema = "ns.mod/v3"' 'name = "future"' > "$update_tmp/future/ns.mod"
if "$ns" update "$update_tmp/future" > "$update_tmp/future.out" 2>&1; then
    printf '%s\n' 'FAIL: ns update accepted an unknown future manifest schema.' >&2
    exit 1
fi
grep -q 'unsupported manifest schema' "$update_tmp/future.out"
test ! -e "$update_tmp/future/AGENTS.md"

mkdir "$update_tmp/malformed"
printf '%s\n' "schema = 'ns.mod/v0'" 'name = "malformed"' > "$update_tmp/malformed/ns.mod"
if "$ns" update "$update_tmp/malformed" > "$update_tmp/malformed.out" 2>&1; then
    printf '%s\n' 'FAIL: ns update accepted a malformed schema assignment.' >&2
    exit 1
fi
grep -q 'cannot parse schema' "$update_tmp/malformed.out"
test "$(grep -c '^schema =' "$update_tmp/malformed/ns.mod")" -eq 1
test ! -e "$update_tmp/malformed/AGENTS.md"

printf '%s\n' '# local instructions after first update' > "$update_tmp/legacy/AGENTS.md"
"$ns" update "$update_tmp/legacy" > /dev/null
test "$(cat "$update_tmp/legacy/bin/ns-update-backup/AGENTS.md.1")" = '# local instructions after first update'

mkdir "$update_tmp/not-a-project"
if "$ns" update "$update_tmp/not-a-project" > "$update_tmp/not-a-project.out" 2>&1; then
    printf '%s\n' 'FAIL: ns update succeeded without an ns.mod.' >&2
    exit 1
fi
grep -q 'no ns.mod found' "$update_tmp/not-a-project.out"

printf '%s\n' 'PASS: ns update migrates project metadata, preserves custom content, and is idempotent.'
