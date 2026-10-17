#!/bin/sh
# Build the pinned upstream encoder outside the interpreter and source tree.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
revision=46c2caa7537aa9cd30595490a3dd33b503faa47c
source_dir="$root/bin/astcenc-source"
build_dir="$root/bin/astcenc-build"
mkdir -p "$root/bin"
if [ ! -d "$source_dir/.git" ]; then
    git init "$source_dir"
    git -C "$source_dir" remote add origin https://github.com/ARM-software/astc-encoder.git
fi
git -C "$source_dir" fetch --depth 1 origin "$revision"
git -C "$source_dir" checkout --detach "$revision"
cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release \
    -DASTCENC_ISA_NATIVE=ON -DASTCENC_ISA_NEON=OFF -DASTCENC_ISA_AVX2=OFF \
    -DASTCENC_ISA_SSE41=OFF -DASTCENC_UNIVERSAL_BUILD=OFF
cmake --build "$build_dir" --target astcenc-native --parallel
cp "$build_dir/Source/astcenc-native" "$root/bin/astcenc"
printf '%s\n' 'ASTC encoder ready in bin/astcenc; make install also installs it.'
