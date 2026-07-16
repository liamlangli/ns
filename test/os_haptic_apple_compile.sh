#!/bin/sh

set -eu

compile_for_sdk() {
    sdk=$1
    target=$2
    if ! sdk_path=$(xcrun --sdk "$sdk" --show-sdk-path 2>/dev/null); then
        return
    fi
    xcrun --sdk "$sdk" clang -fsyntax-only -fobjc-arc -Wall -Wextra \
        -Wunused-result -Werror -target "$target" -isysroot "$sdk_path" \
        -DNS_DARWIN -Iinclude -Iinclude/asm -Iinclude/os -Ilib/include \
        lib/src/os.haptic.apple.m
}

compile_for_sdk macosx arm64-apple-macos12.0
compile_for_sdk iphoneos arm64-apple-ios16.0
compile_for_sdk xros arm64-apple-xros1.0
