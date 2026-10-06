#!/bin/sh
set -eu
# No ARKit or Swift code is linked into the ns language executable.
# Typecheck the runtime source against every available Apple platform SDK.
command -v xcrun >/dev/null 2>&1 || exit 0
compile_for_sdk() {
    sdk=$1
    target=$2
    if ! sdk_path=$(xcrun --sdk "$sdk" --show-sdk-path 2>/dev/null); then return; fi
    xcrun --sdk "$sdk" swiftc -parse-as-library -warnings-as-errors -typecheck \
        -target "$target" -sdk "$sdk_path" lib/src/view.hands.vision.swift
}
compile_for_sdk macosx arm64-apple-macos12.0
compile_for_sdk iphoneos arm64-apple-ios16.0
compile_for_sdk xros arm64-apple-xros2.0
compile_for_sdk xrsimulator arm64-apple-xros2.0-simulator
