#!/bin/sh
# Build the prebuilt releases and publish them with the installers.
#
#   sh scripts/release.sh [--dry-run] [platform...]
#   sh scripts/release.sh --installers
#
# Platforms are darwin-arm64 (built on this Mac), linux-x86_64 and
# windows-x86_64 (built in amd64 Linux containers with Apple's `container`
# tool from the images in scripts/release/). With none named, all three build.
# --dry-run packages into bin/release without uploading. --installers builds
# nothing and uploads only the installers and the install page, leaving the
# published release and its latest marker as they are.
#
# Each build compiles a clean export of the working tree (tracked and
# untracked, non-ignored files) with NS_DEBUG=0, installs it into a staging
# root and packs that root: ns-<platform>.tar.gz, or .zip for Windows, plus a
# .sha256. Uploading puts the archives under <dir>/<version>/, then the
# installers, the install page (scripts/release/index.html) and the latest
# marker under <dir>/, so an installer only ever sees a complete release.
# <version> is `git describe --dirty` of HEAD.
#
# Environment:
#   NS_RELEASE_HOST  ssh host to upload to (default: lang)
#   NS_RELEASE_DIR   web directory on that host (default: /var/www/ns)
#   MACOSX_DEPLOYMENT_TARGET  oldest macOS the build runs on (default: 14.0)
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
host=${NS_RELEASE_HOST:-lang}
dir=${NS_RELEASE_DIR:-/var/www/ns}
dry_run=0
installers_only=0
platforms=""
for arg in "$@"; do
    case "$arg" in
    --dry-run) dry_run=1 ;;
    --installers) installers_only=1 ;;
    darwin-arm64 | linux-x86_64 | windows-x86_64) platforms="$platforms $arg" ;;
    *) echo "release: unknown argument $arg" >&2; exit 2 ;;
    esac
done
[ -n "$platforms" ] || platforms="darwin-arm64 linux-x86_64 windows-x86_64"

version=$(git -C "$repo" describe --tags --always --dirty)
out=$repo/bin/release
mkdir -p "$out/$version"

# The installers and the page, as they are served beside the releases.
stage_installers() {
    cp "$repo/scripts/install.sh" "$repo/scripts/install.ps1" "$repo/scripts/release/index.html" \
        "$repo/sample/ns.svg" "$out/"
    # cmd.exe expects CRLF line endings.
    sed 's/\r*$/\r/' "$repo/scripts/install.bat" >"$out/install.bat"
}
upload_installers() {
    scp "$out/install.sh" "$out/install.ps1" "$out/install.bat" "$out/index.html" \
        "$out/ns.svg" "$@" "$host:$dir/"
    ssh "$host" "chmod -R a+rX '$dir'"
}

if [ "$installers_only" = 1 ]; then
    stage_installers
    [ "$dry_run" = 1 ] && exit 0
    upload_installers
    echo "release: published the installers to $host:$dir"
    exit 0
fi

# Copy the working tree as git sees it into $1.
export_source() {
    rm -rf "$1"
    mkdir -p "$1"
    (cd "$repo" && git ls-files -z --cached --others --exclude-standard |
        xargs -0 tar -cf -) | tar -xf - -C "$1"
    # The pinned zlib download is ignored by git; reuse it rather than fetch
    # it again for every build.
    if [ -f "$repo/third_party/zlib/zlib.h" ]; then
        mkdir -p "$1/third_party"
        cp -R "$repo/third_party/zlib" "$1/third_party/zlib"
    fi
}

# Build or refresh the builder image for linux or windows.
builder_image() {
    container build --arch amd64 -t "ns-release-$1" \
        -f "$repo/scripts/release/$1.Dockerfile" "$repo/scripts/release"
}

build() {
    platform=$1
    work=$out/$platform
    rm -rf "$work"
    export_source "$work/src"
    echo "release: building $version for $platform"
    case "$platform" in
    darwin-arm64)
        [ "$(uname -s)-$(uname -m)" = Darwin-arm64 ] || {
            echo "release: darwin-arm64 builds on an arm64 Mac" >&2
            exit 1
        }
        export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-14.0}
        (cd "$work/src" && make NS_DEBUG=0 install NS_HOME="$work/home")
        ;;
    linux-x86_64 | windows-x86_64)
        os=${platform%-*}
        builder_image "$os"
        container run --rm --arch amd64 --cpus 8 --memory 8g -v "$work:/work" "ns-release-$os" \
            sh /work/src/scripts/release/build_in_container.sh "$os"
        ;;
    esac

    stage=$work/home/.ns
    case "$platform" in
    windows-*)
        archive=ns-$platform.zip
        rm -f "$out/$version/$archive"
        (cd "$stage" && zip -qr -X "$out/$version/$archive" .)
        ;;
    *)
        archive=ns-$platform.tar.gz
        # Darwin tar would otherwise add AppleDouble files and xattrs.
        COPYFILE_DISABLE=1 tar -czf "$out/$version/$archive" -C "$stage" .
        ;;
    esac
    (cd "$out/$version" && shasum -a 256 "$archive" >"$archive.sha256")
    rm -rf "$work"
    echo "release: packaged $out/$version/$archive"
}

for platform in $platforms; do
    build "$platform"
done

stage_installers
printf '%s\n' "$version" >"$out/latest"

[ "$dry_run" = 1 ] && exit 0

# A release must carry every platform the installers point at.
for platform in darwin-arm64 linux-x86_64 windows-x86_64; do
    case "$platform" in windows-*) archive=ns-$platform.zip ;; *) archive=ns-$platform.tar.gz ;; esac
    [ -f "$out/$version/$archive" ] || {
        echo "release: $version has no $archive; build it before publishing" >&2
        exit 1
    }
done

ssh "$host" "mkdir -p '$dir/$version'"
scp "$out/$version"/ns-* "$host:$dir/$version/"
upload_installers "$out/latest"
echo "release: published $version to $host:$dir"
