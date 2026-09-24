#!/bin/sh
set -eu

ns=$1

if [ "$(uname -s)" != "Darwin" ]; then
    printf '%s\n' 'SKIP: ns project Apple target integration test requires Darwin.'
    exit 0
fi

test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-project-cli-test.XXXXXX")
trap 'rm -rf "$test_tmp"' EXIT INT TERM

mkdir -p "$test_tmp/app/src"
printf '%s\n' \
    'schema = "ns.mod/v2"' \
    'name = "Linked App"' \
    'version = "0.1.0"' \
    'type = "app"' \
    'source = "src"' \
    'entry = "main.ns"' \
    'target = "exec"' > "$test_tmp/app/ns.mod"
printf '%s\n' \
    'use std' \
    'fn main() {' \
    '    print("linked app\\n")' \
    '}' > "$test_tmp/app/src/main.ns"

"$ns" project "$test_tmp/app"

pbx="$test_tmp/app/bin/linked-app.xcodeproj/project.pbxproj"
test -f "$pbx"
grep -q 'isa = PBXNativeTarget;' "$pbx"
grep -q 'linked-app iOS' "$pbx"
grep -q 'SDKROOT = iphoneos;' "$pbx"
if grep -q 'isa = PBXLegacyTarget;' "$pbx"; then
    printf '%s\n' 'FAIL: target = "exec" generated host-only Xcode targets.' >&2
    exit 1
fi

printf '%s\n' 'PASS: target = "exec" app keeps the generated iOS target.'
