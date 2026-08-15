#!/bin/sh

set -eu

zstd_commit=f8745da6ff1ad1e7bab384bd1f9d742439278e99
zstd_sha256=4b0bd1f0cfb25e61b9103c35f27395530ff5b4c0d2513a00fd745849e85ea52c
zstd_url="https://github.com/facebook/zstd/archive/${zstd_commit}.tar.gz"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "${script_dir}/.." && pwd)
third_party_dir="${project_dir}/third_party"
zstd_dir="${third_party_dir}/zstd"
marker="${zstd_dir}/.ns-zstd-commit"

if test -f "$marker" &&
    test "$(cat "$marker")" = "$zstd_commit" &&
    test -f "${zstd_dir}/lib/zstd.h" &&
    test -f "${zstd_dir}/lib/compress/zstd_compress.c" &&
    test -f "${zstd_dir}/LICENSE"; then
    printf 'Zstandard %s is already downloaded.\n' "$zstd_commit"
    exit 0
fi

command -v curl >/dev/null 2>&1 || {
    printf 'error: curl is required to download Zstandard\n' >&2
    exit 1
}
command -v tar >/dev/null 2>&1 || {
    printf 'error: tar is required to unpack Zstandard\n' >&2
    exit 1
}

mkdir -p "$third_party_dir"
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/ns-zstd.XXXXXX")
archive="${temporary_dir}/zstd.tar.gz"
unpacked_dir="${temporary_dir}/unpacked"

cleanup() {
    rm -rf -- "$temporary_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

printf 'Downloading Zstandard %s...\n' "$zstd_commit"
curl -fL --retry 3 --output "$archive" "$zstd_url"

if command -v sha256sum >/dev/null 2>&1; then
    actual_sha256=$(sha256sum "$archive" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    actual_sha256=$(shasum -a 256 "$archive" | awk '{print $1}')
else
    printf 'error: sha256sum or shasum is required to verify Zstandard\n' >&2
    exit 1
fi

if test "$actual_sha256" != "$zstd_sha256"; then
    printf 'error: Zstandard archive checksum mismatch\n' >&2
    printf 'expected: %s\nactual:   %s\n' "$zstd_sha256" "$actual_sha256" >&2
    exit 1
fi

mkdir -p "$unpacked_dir"
tar -xzf "$archive" --strip-components=1 -C "$unpacked_dir"

if ! test -f "${unpacked_dir}/lib/zstd.h" ||
    ! test -f "${unpacked_dir}/lib/compress/zstd_compress.c" ||
    ! test -f "${unpacked_dir}/LICENSE"; then
    printf 'error: downloaded Zstandard archive is incomplete\n' >&2
    exit 1
fi

rm -rf -- "$zstd_dir"
mv "$unpacked_dir" "$zstd_dir"
printf '%s\n' "$zstd_commit" > "$marker"
printf 'Downloaded Zstandard to %s\n' "$zstd_dir"
