#!/bin/sh
set -eu

if [ "$(uname -s)" != Darwin ]; then
    exit 0
fi

mkdir -p bin
for test_name in gpu_metal_dispatch_test gpu_metal_texture_test; do
    xcrun clang -DNS_DARWIN \
        -Iinclude -Iinclude/asm -Iinclude/os -Ilib/include \
        "test/$test_name.m" bin/lib/gpu.o bin/libns.a \
        -framework Foundation -framework Metal -framework MetalKit -framework QuartzCore \
        -lffi -lreadline -o "bin/$test_name"
    MTL_DEBUG_LAYER=1 "bin/$test_name"
done
