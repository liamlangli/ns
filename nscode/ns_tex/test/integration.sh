#!/bin/sh
# End-to-end packaging using Xcode's encoder and real PNGs, without Pillow.
set -eu
tool=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
root=$(CDPATH= cd -- "$tool/../.." && pwd)
launcher=${1:-"$tool/ns_tex"}
NS_EXECUTABLE=${NS_EXECUTABLE:-"$root/bin/ns"}
export NS_EXECUTABLE
if ! xcrun --find textureconverter >/dev/null 2>&1; then
    echo 'ns_tex integration: skipped (Xcode textureconverter unavailable)'
    exit 0
fi
temporary=$(mktemp -d "${TMPDIR:-/tmp}/ns_tex cook's test.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
mkdir -p "$temporary/res/tex" "$temporary/bin/output/res/tex"
cp "$root/lib/assets/bitmap_font.png" "$temporary/res/tex/icon's name.png"
cp "$root/lib/assets/bitmap_font.png" "$temporary/res/tex/skip.png"
cp "$temporary/res/tex/skip.png" "$temporary/bin/output/res/tex/skip.png"
printf stale > "$temporary/bin/output/res/tex/skip.png.nstex"
cat > "$temporary/ns.mod" <<'EOF'
schema = "ns.mod/v2"
name = "ns-tex-run-test"
version = "0.1.0"
type = "app"
target = "exec"
source = "src"
entry = "main.ns"
assets = ["res"]
[cook.ios]
format = "astc_6x6"
max_size = 32
include = ["res/tex/*"]
exclude = ["res/tex/skip.png"]
[cook.macos]
format = "astc_4x4"
max_size = 0
srgb = true
include = ["res/tex/*"]
exclude = ["res/tex/skip.png"]
EOF
mkdir -p "$temporary/src"
cat > "$temporary/src/main.ns" <<'EOF'
use os
use std
fn main() {
    assert os_file_size("res/tex/icon's name.png.nstex") > 32
    assert os_file_size("res/tex/icon's name.png") < 0
    assert os_file_size("res/tex/skip.png") > 0
    print("ns_tex native run passed\n")
}
EOF
cp "$temporary/res/tex/icon's name.png" "$temporary/bin/output/res/tex/icon's name.png"
NS_COOK_PYTHON=/missing/python NS_ASTCENC=/missing/astcenc \
    "$launcher" "$temporary" iphonesimulator "$temporary/bin/output"
test -s "$temporary/bin/output/res/tex/icon's name.png.nstex"
test ! -e "$temporary/bin/output/res/tex/icon's name.png"
test ! -e "$temporary/bin/output/res/tex/skip.png.nstex"
test -s "$temporary/bin/output/res/tex/skip.png"
cmp "$root/lib/assets/bitmap_font.png" "$temporary/res/tex/icon's name.png"
cp "$temporary/bin/output/res/tex/icon's name.png.nstex" "$temporary/first.nstex"
"$launcher" "$temporary" ios "$temporary/bin/output" > "$temporary/cached.log"
test ! -s "$temporary/cached.log"
cmp "$temporary/first.nstex" "$temporary/bin/output/res/tex/icon's name.png.nstex"
"$launcher" "$temporary" macosx "$temporary/bin/mac-output"
format=$(od -An -tu4 -j8 -N4 "$temporary/bin/mac-output/res/tex/icon's name.png.nstex" | tr -d ' ')
test "$format" = 69
# Public ns cook and native app runs must reach the same tool without Python.
NS_COOK_PYTHON=/missing/python NS_ASTCENC=/missing/astcenc \
    "$NS_EXECUTABLE" cook "$temporary" --platform macos --output "$temporary/bin/driver-output"
cmp "$temporary/bin/mac-output/res/tex/icon's name.png.nstex" "$temporary/bin/driver-output/res/tex/icon's name.png.nstex"
NS_COOK_PYTHON=/missing/python NS_ASTCENC=/missing/astcenc \
    "$NS_EXECUTABLE" run "$temporary" > "$temporary/run.log" 2>&1
grep -q 'ns_tex native run passed' "$temporary/run.log"
sed 's/target = "exec"/target = "eval"/' "$temporary/ns.mod" > "$temporary/patch.mod"
mv "$temporary/patch.mod" "$temporary/ns.mod"
NS_COOK_PYTHON=/missing/python NS_ASTCENC=/missing/astcenc \
    "$NS_EXECUTABLE" patch --cook-platform ios "$temporary" > "$temporary/patch.log" 2>&1
test -s "$temporary/bin/ns-tex-run-test_patch/ns-tex-run-test.nsapp"
test ! -e "$temporary/bin/ns-tex-run-test_patch/cook-staging"
printf bad > "$temporary/bad.ktx"
if "$NS_EXECUTABLE" run "$tool" --pack "$temporary/bad.ktx" "$temporary/bad.nstex" astc_6x6 false 96 96 > "$temporary/invalid.log" 2>&1; then
    echo 'Malformed KTX unexpectedly accepted' >&2
    exit 1
fi
test ! -e "$temporary/bad.nstex"
mkdir -p "$temporary/bin/linked-output"
ln -s "$temporary/res" "$temporary/bin/linked-output/res"
if "$launcher" "$temporary" ios "$temporary/bin/linked-output" > "$temporary/symlink.log" 2>&1; then
    echo 'Symlink into source assets unexpectedly accepted' >&2
    exit 1
fi
test ! -e "$temporary/res/tex/icon's name.png.nstex"
cmp "$root/lib/assets/bitmap_font.png" "$temporary/res/tex/icon's name.png"
echo 'native texture cooking: all assertions passed'
