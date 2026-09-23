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

# A packaged file - a browser script, a model, an image - is copied beside the
# module, not compiled into it, so editing one re-packages the bundle without
# recompiling a module that did not change. Sources still recompile.
repack="$tmp/repack"
mkdir -p "$repack/web"
cat > "$repack/ns.mod" <<'EOF'
schema = "ns.mod/v1"
name = "repack"
version = "0.1.0"
type = "app"
target = "wasm"
source = "."
entry = "main.ns"
assets = ["web/viewer.js"]
EOF
cat > "$repack/main.ns" <<'EOF'
use std

fn main() {
    print("repack")
}
EOF
printf '%s\n' 'const viewer = 1' > "$repack/web/viewer.js"

build_bundle() {
    "$ns" build "$repack" > "$tmp/build.log" 2>&1 || {
        cat "$tmp/build.log" >&2
        printf '%s\n' 'FAIL: ns build failed for a project with a packaged file.' >&2
        exit 1
    }
}

build_bundle
module=$(ls -l "$repack/bin/repack.wasm")

touch "$repack/web/viewer.js"
build_bundle
expect_up_to_date 'a touched packaged file with unchanged contents must not re-package a bundle.'

printf '%s\n' 'const viewer = 2' > "$repack/web/viewer.js"
build_bundle
if grep -q 'up to date' "$tmp/build.log"; then
    cat "$tmp/build.log" >&2
    printf '%s\n' 'FAIL: an edited packaged file must refresh the bundle around it.' >&2
    exit 1
fi
if grep -q 'wasm bundle' "$tmp/build.log"; then
    cat "$tmp/build.log" >&2
    printf '%s\n' 'FAIL: an edited packaged file must not recompile the module.' >&2
    exit 1
fi
test "$module" = "$(ls -l "$repack/bin/repack.wasm")" || {
    printf '%s\n' 'FAIL: re-packaging a bundle must leave the module in place.' >&2
    exit 1
}
if ! grep -q 'const viewer = 2' "$repack/bin/web/viewer.js"; then
    printf '%s\n' 'FAIL: the re-packaged bundle must carry the edited file.' >&2
    exit 1
fi

cat > "$repack/main.ns" <<'EOF'
use std

fn main() {
    print("repack!")
}
EOF
build_bundle
if ! grep -q 'wasm bundle' "$tmp/build.log"; then
    cat "$tmp/build.log" >&2
    printf '%s\n' 'FAIL: an edited source must recompile the module.' >&2
    exit 1
fi
test "$module" != "$(ls -l "$repack/bin/repack.wasm")" || {
    printf '%s\n' 'FAIL: an edited source must rewrite the module.' >&2
    exit 1
}

build_bundle
expect_up_to_date 'a rebuilt bundle must record the recompiled source.'

printf '%s\n' 'PASS: a packaged edit re-packages a browser bundle without recompiling it.'

# A `link = false` target must build into something runnable on every host: a
# native executable where the host has a code generator for one, and otherwise a
# launcher that enters the project and hands the program to `ns run`. Darwin and
# Windows are covered by the native-artifact tests above.
case "$(uname -s)" in
Darwin|MINGW*|MSYS*|CYGWIN*)
    printf '%s\n' 'SKIP: the interpreted-target artifact test covers the other hosts.'
    ;;
*)
    if [ "$(uname -s)" = Linux ]; then
        native_app="$tmp/native-app"
        mkdir -p "$native_app/assets"
        cp "$(dirname "$0")/../sample/ns.png" "$native_app/icon.png"
        printf 'bundled asset\n' > "$native_app/assets/message.txt"
        cat > "$native_app/ns.mod" <<'EOF'
schema = "ns.mod/v1"
name = "native-app"
version = "0.1.0"
type = "app"
source = "."
entry = "main.ns"
icon = "icon.png"
EOF
        cat > "$native_app/main.ns" <<'EOF'
use os

fn main() {
    assert os_platform() == OS_PLATFORM_LINUX
}
EOF
        cat > "$tmp/appimagetool" <<'EOF'
#!/bin/sh
set -eu
test "$#" -eq 3
test "$1" = -n
app_dir=$2
test -x "$app_dir/AppRun"
test -x "$app_dir/usr/bin/app"
test -f "$app_dir/usr/bin/os.so"
test -f "$app_dir/usr/bin/assets/message.txt"
test -f "$app_dir/usr/bin/.ns-resources"
test -f "$app_dir/app.png"
test -L "$app_dir/.DirIcon"
grep -q '^Name=native-app$' "$app_dir/app.desktop"
grep -q '^Icon=app$' "$app_dir/app.desktop"
printf 'mock AppImage\n' > "$3"
chmod +x "$3"
EOF
        chmod +x "$tmp/appimagetool"
        NS_APPIMAGETOOL="$tmp/appimagetool" "$ns" build "$native_app" > "$tmp/native-app.log" 2>&1 || {
            cat "$tmp/native-app.log" >&2
            printf '%s\n' 'FAIL: ns build failed for a Linux app.' >&2
            exit 1
        }
        if [ ! -x "$native_app/bin/native-app.AppImage" ] ||
           ! grep -q 'appimage ' "$tmp/native-app.log"; then
            cat "$tmp/native-app.log" >&2
            printf '%s\n' 'FAIL: a Linux app must package an AppImage with its icon.' >&2
            exit 1
        fi
        NS_APPIMAGETOOL="$tmp/appimagetool" "$ns" build "$native_app" > "$tmp/native-app.log" 2>&1
        expect_up_to_date 'an unchanged Linux AppImage must not be rebuilt.'
    fi

    interpreted="$tmp/interpreted"
    mkdir -p "$interpreted/src"
    cat > "$interpreted/ns.mod" <<'EOF'
schema = "ns.mod/v1"
name = "interpreted"
version = "0.1.0"
type = "cli"
source = "src"
entry = "main.ns"
link = false
EOF
    cat > "$interpreted/src/main.ns" <<'EOF'
use std

fn main() {
    print("interpreted ran\n")
}
EOF

    "$ns" build "$interpreted" > "$tmp/interpreted.log" 2>&1 || {
        cat "$tmp/interpreted.log" >&2
        printf '%s\n' 'FAIL: ns build failed for an interpreted target.' >&2
        exit 1
    }
    launcher="$interpreted/bin/interpreted"
    test -x "$launcher" || {
        cat "$tmp/interpreted.log" >&2
        printf '%s\n' 'FAIL: an interpreted target must build a runnable artifact.' >&2
        exit 1
    }
    output=$("$launcher")
    test "$output" = "interpreted ran" || {
        printf 'FAIL: the launcher ran the wrong program: %s\n' "$output" >&2
        exit 1
    }

    "$ns" build "$interpreted" > "$tmp/interpreted.log" 2>&1 || {
        cat "$tmp/interpreted.log" >&2
        printf '%s\n' 'FAIL: a rebuilt interpreted target failed.' >&2
        exit 1
    }
    expect_up_to_date 'an unchanged interpreted target must not be rebuilt.'

    printf '%s\n' 'PASS: ns build produces a runnable artifact for a link = false target.'
    ;;
esac
