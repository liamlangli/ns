#!/bin/sh

# End-to-end coverage for the incremental `ns build` and for `ns clean`. A Wasm
# project is used because it builds on every host; the cache and the clean set
# are the same for a native artifact.

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

ns=$1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-build-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

project="$tmp/app"
mkdir -p "$project"
cat > "$project/ns.mod" <<'EOF'
schema = "ns.mod/v1"
name = "demo"
version = "0.1.0"
type = "app"
target = "wasm"
source = "."
entry = "main.ns"
EOF
cat > "$project/main.ns" <<'EOF'
use std

fn main() {
    print("hello")
}
EOF

build() {
    "$ns" build "$project" "$@" > "$tmp/build.log" 2>&1 || {
        cat "$tmp/build.log" >&2
        printf '%s\n' 'FAIL: ns build failed.' >&2
        exit 1
    }
}

expect_built() {
    if grep -q 'up to date' "$tmp/build.log"; then
        cat "$tmp/build.log" >&2
        printf 'FAIL: %s\n' "$1" >&2
        exit 1
    fi
}

expect_up_to_date() {
    if ! grep -q 'up to date' "$tmp/build.log"; then
        cat "$tmp/build.log" >&2
        printf 'FAIL: %s\n' "$1" >&2
        exit 1
    fi
}

build
expect_built 'the first build of a project must compile.'
test -f "$project/bin/demo.wasm"
test -f "$project/bin/.ns-build/demo.wasm.cache"

build
expect_up_to_date 'an unchanged project must not be recompiled.'

# The artifact of an up-to-date build is kept, not removed and re-emitted.
stamp=$(ls -l "$project/bin/demo.wasm")
build
expect_up_to_date 'a repeated build must stay up to date.'
test "$stamp" = "$(ls -l "$project/bin/demo.wasm")"

touch "$project/main.ns"
build
expect_up_to_date 'a touched source with unchanged contents must not be recompiled.'

build --force
expect_built '--force must rebuild an up-to-date project.'

cat > "$project/main.ns" <<'EOF'
use std

fn main() {
    print("hello world")
}
EOF
build
expect_built 'an edited source must be recompiled.'
build
expect_up_to_date 'the rebuild must record the edited source.'

printf 'fn helper() i32 {\n    return 1\n}\n' > "$project/extra.ns"
build
expect_built 'a source added to the project must be recompiled.'
build
expect_up_to_date 'the rebuild must record the added source.'

rm "$project/extra.ns"
build
expect_built 'a source removed from the project must be recompiled.'

rm "$project/bin/demo.wasm"
build
expect_built 'a missing artifact must be rebuilt.'

printf 'ns.profile\n' > "$project/ns.profile"
printf '{}\n' > "$project/ns.profile.json"
"$ns" clean "$project" > "$tmp/clean.log" 2>&1
test ! -e "$project/bin"
test ! -e "$project/ns.profile"
test ! -e "$project/ns.profile.json"
test -f "$project/main.ns"
test -f "$project/ns.mod"

"$ns" clean "$project" > "$tmp/clean.log" 2>&1
if ! grep -q 'nothing to remove' "$tmp/clean.log"; then
    cat "$tmp/clean.log" >&2
    printf '%s\n' 'FAIL: a clean project must report that there is nothing to remove.' >&2
    exit 1
fi

build
expect_built 'a cleaned project must be rebuilt.'

# Without a path, `ns clean` finds the project of the current directory.
mkdir -p "$project/nested"
(cd "$project/nested" && "$ns" clean > "$tmp/clean.log" 2>&1)
test ! -e "$project/bin"

printf '%s\n' 'PASS: ns build stays incremental and ns clean removes generated files.'

# A manifest that declares targets gives each one `bin/<target name>`, so two
# browser bundles that both package an index page and a runtime never overwrite
# each other the way they would in a shared bin/.
multi="$tmp/multi"
mkdir -p "$multi/src"
cat > "$multi/ns.mod" <<'EOF'
schema = "ns.mod/v1"
name = "multi"
version = "0.1.0"
type = "app"
target = "wasm"
source = "src"

[[targets]]
name = "page"
entry = "page_main.ns"
default = true

[[targets]]
name = "docs"
entry = "docs_main.ns"
EOF
cat > "$multi/src/page_main.ns" <<'EOF'
use std

fn main() {
    if multi_shared() == 42 { print("page\n") }
}
EOF
cat > "$multi/src/docs_main.ns" <<'EOF'
use std

fn main() {
    if multi_shared() == 42 { print("docs\n") }
}
EOF
cat > "$multi/src/shared.ns" <<'EOF'
fn multi_shared() i32 {
    return 42
}
EOF

"$ns" build "$multi" > "$tmp/multi.log" 2>&1 || {
    cat "$tmp/multi.log" >&2
    printf '%s\n' 'FAIL: ns build failed for a multi-target project.' >&2
    exit 1
}
for artifact in page/page.wasm page/index.html page/.ns-build/page.wasm.cache \
                docs/docs.wasm docs/index.html docs/.ns-build/docs.wasm.cache; do
    if [ ! -f "$multi/bin/$artifact" ]; then
        cat "$tmp/multi.log" >&2
        printf 'FAIL: ns build did not write bin/%s for its target.\n' "$artifact" >&2
        exit 1
    fi
done
if [ -e "$multi/bin/index.html" ] || [ -e "$multi/bin/page.wasm" ]; then
    printf '%s\n' 'FAIL: a declared target left files directly in bin/.' >&2
    ls "$multi/bin" >&2
    exit 1
fi

# A named target rebuilds inside its own directory and leaves the sibling alone.
stamp=$(ls -l "$multi/bin/docs/docs.wasm")
(cd "$multi" && "$ns" build page --force) > "$tmp/multi.log" 2>&1 || {
    cat "$tmp/multi.log" >&2
    printf '%s\n' 'FAIL: ns build <target> failed for a multi-target project.' >&2
    exit 1
}
test "$stamp" = "$(ls -l "$multi/bin/docs/docs.wasm")"

printf '%s\n' 'PASS: every ns.mod target owns bin/<target name>.'
