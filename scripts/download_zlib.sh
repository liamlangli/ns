#!/bin/sh

set -eu

zlib_commit=51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf
zlib_sha256=d9e270d46252734aa49770fbc544125391617956266f220bd63216c834f3a522
zlib_url="https://github.com/madler/zlib/archive/${zlib_commit}.tar.gz"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "${script_dir}/.." && pwd)
third_party_dir="${project_dir}/third_party"
zlib_dir="${third_party_dir}/zlib"
marker="${zlib_dir}/.ns-zlib-commit"

if test -f "$marker" &&
    test "$(cat "$marker")" = "$zlib_commit" &&
    test -f "${zlib_dir}/zlib.h" &&
    test -f "${zlib_dir}/deflate.c" &&
    test -f "${zlib_dir}/LICENSE"; then
    printf 'zlib %s is already downloaded.\n' "$zlib_commit"
    exit 0
fi

command -v curl >/dev/null 2>&1 || {
    printf 'error: curl is required to download zlib\n' >&2
    exit 1
}
command -v tar >/dev/null 2>&1 || {
    printf 'error: tar is required to unpack zlib\n' >&2
    exit 1
}

mkdir -p "$third_party_dir"
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/ns-zlib.XXXXXX")
archive="${temporary_dir}/zlib.tar.gz"
unpacked_dir="${temporary_dir}/unpacked"

cleanup() {
    rm -rf -- "$temporary_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

printf 'Downloading zlib %s...\n' "$zlib_commit"
curl -fL --retry 3 --output "$archive" "$zlib_url"

if command -v sha256sum >/dev/null 2>&1; then
    actual_sha256=$(sha256sum "$archive" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    actual_sha256=$(shasum -a 256 "$archive" | awk '{print $1}')
else
    printf 'error: sha256sum or shasum is required to verify zlib\n' >&2
    exit 1
fi

if test "$actual_sha256" != "$zlib_sha256"; then
    printf 'error: zlib archive checksum mismatch\n' >&2
    printf 'expected: %s\nactual:   %s\n' "$zlib_sha256" "$actual_sha256" >&2
    exit 1
fi

mkdir -p "$unpacked_dir"
tar -xzf "$archive" --strip-components=1 -C "$unpacked_dir"

if ! test -f "${unpacked_dir}/zlib.h" ||
    ! test -f "${unpacked_dir}/deflate.c" ||
    ! test -f "${unpacked_dir}/LICENSE"; then
    printf 'error: downloaded zlib archive is incomplete\n' >&2
    exit 1
fi

rm -rf -- "$zlib_dir"
mv "$unpacked_dir" "$zlib_dir"
printf '%s\n' "$zlib_commit" > "$marker"
printf 'Downloaded zlib to %s\n' "$zlib_dir"
