#!/bin/sh
# Reproducible in-process Git for generated Apple projects. No host Git binary
# or private Apple frameworks are used on the device. Output is build-only.
set -eu
sdk=${1:-iphoneos}
root=${2:?usage: os.git.build.sh SDK OUTPUT_ROOT}
arch=${3:-arm64}
case "$sdk:$arch" in
    iphoneos:arm64) system=iOS; openssl_target=ios64-cross; minimum=16.0; deployment=-miphoneos-version-min=16.0 ;;
    iphonesimulator:arm64) system=iOS; openssl_target=iossimulator-xcrun; minimum=16.0; deployment=-mios-simulator-version-min=16.0 ;;
    macosx:arm64) system=Darwin; openssl_target=darwin64-arm64-cc; minimum=12.0; deployment=-mmacosx-version-min=12.0 ;;
    *) echo "Unsupported Git SDK/architecture: $sdk/$arch" >&2; exit 1 ;;
esac
# Xcode GUI does not inherit a Homebrew shell's PATH.
PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
export PATH
command -v cmake >/dev/null || { echo 'Install CMake to build Apple Git dependencies' >&2; exit 1; }
mkdir -p "$root"
root=$(cd "$root" && pwd)
prefix="$root/$sdk-$arch"
revision=libgit2-1.9.1-libssh2-1.11.1-openssl-3.5.4-v3
if [ -f "$prefix/.complete" ] && [ "$(cat "$prefix/.complete")" = "$revision" ]; then exit 0; fi
# Separate platform roots make device and simulator builds independent.
work="$root/work-$sdk-$arch"
mkdir -p "$work" "$prefix" "$root/downloads"
fetch() {
    name=$1; url=$2; digest=$3
    archive="$root/downloads/$name.tar.gz"
    if [ ! -f "$archive" ]; then
        partial=$(mktemp "$root/downloads/$name.XXXXXX")
        curl -fL --retry 3 "$url" -o "$partial"
        echo "$digest  $partial" | shasum -a 256 -c - >/dev/null
        mv "$partial" "$archive"
    fi
    echo "$digest  $archive" | shasum -a 256 -c - >/dev/null
    if [ ! -f "$work/$name/.unpacked" ]; then
        mkdir -p "$work/$name"
        tar -xzf "$archive" --strip-components=1 -C "$work/$name"
        touch "$work/$name/.unpacked"
    fi
}
fetch libgit2 https://codeload.github.com/libgit2/libgit2/tar.gz/refs/tags/v1.9.1 14cab3014b2b7ad75970ff4548e83615f74d719afe00aa479b4a889c1e13fc00
fetch libssh2 https://codeload.github.com/libssh2/libssh2/tar.gz/refs/tags/libssh2-1.11.1 82b35c61c78b475647bdc981a183c5b5ab0d979e1caee94186e8f9150f2b0d0d
fetch openssl https://codeload.github.com/openssl/openssl/tar.gz/refs/tags/openssl-3.5.4 758b69feed5787dc12d34b0eb29b60d3c9d73d5a64760c62d93a6d26b344d65d
sysroot=$(xcrun --sdk "$sdk" --show-sdk-path)
cc=$(xcrun --sdk "$sdk" --find clang)
jobs=$(sysctl -n hw.logicalcpu)
if [ ! -f "$prefix/.openssl-deployment" ] || [ "$(cat "$prefix/.openssl-deployment")" != "$deployment" ]; then
    (
        cd "$work/openssl"
        export CROSS_TOP="$(dirname "$(dirname "$sysroot")")" CROSS_SDK="$(basename "$sysroot")"
        export SDKROOT="$sysroot" CC="$cc"
        if [ -f Makefile ]; then make clean; fi
        ./Configure "$openssl_target" no-shared no-tests no-apps no-docs no-module no-dso \
            --prefix="$prefix" --libdir=lib "-isysroot $sysroot" "$deployment"
        make -j "$jobs" build_libs
        make install_dev
        printf '%s\n' "$deployment" > "$prefix/.openssl-deployment"
    )
fi
# Avoid accidentally finding host Homebrew libraries during a cross build.
PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR PKG_CONFIG_PATH
cmake -S "$work/libssh2" -B "$work/ssh-build" \
    -DCMAKE_SYSTEM_NAME="$system" -DCMAKE_OSX_SYSROOT="$sysroot" -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$minimum" -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DCRYPTO_BACKEND=OpenSSL \
    -DOPENSSL_ROOT_DIR="$prefix" -DOPENSSL_INCLUDE_DIR="$prefix/include" \
    -DOPENSSL_CRYPTO_LIBRARY="$prefix/lib/libcrypto.a" -DOPENSSL_SSL_LIBRARY="$prefix/lib/libssl.a" -DOPENSSL_USE_STATIC_LIBS=TRUE -DENABLE_ZLIB_COMPRESSION=OFF
cmake --build "$work/ssh-build" -j "$jobs"
cmake --install "$work/ssh-build"
cmake -S "$work/libgit2" -B "$work/git-build" \
    -DCMAKE_SYSTEM_NAME="$system" -DCMAKE_OSX_SYSROOT="$sysroot" -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$minimum" -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_FIND_ROOT_PATH="$prefix" \
    -DLIBSSH2_INCLUDE_DIR="$prefix/include" -DLIBSSH2_LIBRARY="$prefix/lib/libssh2.a" -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_CLI=OFF \
    -U HAVE_LIBSSH2_MEMORY_CREDENTIALS -DCMAKE_REQUIRED_LIBRARIES="$prefix/lib/libcrypto.a" \
    -DUSE_SSH=libssh2 -DUSE_HTTPS=SecureTransport -DUSE_AUTH_NTLM=OFF -DUSE_AUTH_NEGOTIATE=OFF \
    -DUSE_ICONV=OFF -DREGEX_BACKEND=builtin -DOPENSSL_ROOT_DIR="$prefix"
cmake --build "$work/git-build" -j "$jobs"
cmake --install "$work/git-build"
# One archive avoids order-dependent transitive static linkage in Xcode.
xcrun libtool -static -o "$prefix/libns_git.a" "$prefix/lib/libgit2.a" "$prefix/lib/libssh2.a" "$prefix/lib/libcrypto.a"
mkdir -p "$prefix/licenses"
cp "$work/libgit2/COPYING" "$prefix/licenses/libgit2.txt"
cp "$work/libssh2/COPYING" "$prefix/licenses/libssh2.txt"
cp "$work/openssl/LICENSE.txt" "$prefix/licenses/openssl.txt"
printf '%s\n' "$revision" > "$prefix/.complete"
