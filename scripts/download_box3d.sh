#!/bin/sh

set -eu

box3d_commit=8441b4a06d6d09dcfb0b0f704df4d847d1437b92
box3d_sha256=7c88abd80e3106d70221a3873128e5e4b5afdf637ec2a1f7a23091ad0993d2a0
box3d_url="https://github.com/erincatto/box3d/archive/${box3d_commit}.tar.gz"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "${script_dir}/.." && pwd)
third_party_dir="${project_dir}/third_party"
box3d_dir="${third_party_dir}/box3d"
marker="${box3d_dir}/.ns-box3d-commit"

if test -f "$marker" &&
    test "$(cat "$marker")" = "$box3d_commit" &&
    test -f "${box3d_dir}/include/box3d/box3d.h" &&
    test -f "${box3d_dir}/src/physics_world.c" &&
    test -f "${box3d_dir}/LICENSE"; then
    printf 'Box3D %s is already downloaded.\n' "$box3d_commit"
    exit 0
fi

command -v curl >/dev/null 2>&1 || {
    printf 'error: curl is required to download Box3D\n' >&2
    exit 1
}
command -v tar >/dev/null 2>&1 || {
    printf 'error: tar is required to unpack Box3D\n' >&2
    exit 1
}

mkdir -p "$third_party_dir"
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/ns-box3d.XXXXXX")
archive="${temporary_dir}/box3d.tar.gz"
unpacked_dir="${temporary_dir}/unpacked"

cleanup() {
    rm -rf -- "$temporary_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

printf 'Downloading Box3D %s...\n' "$box3d_commit"
curl -fL --retry 3 --output "$archive" "$box3d_url"

if command -v sha256sum >/dev/null 2>&1; then
    actual_sha256=$(sha256sum "$archive" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    actual_sha256=$(shasum -a 256 "$archive" | awk '{print $1}')
else
    printf 'error: sha256sum or shasum is required to verify Box3D\n' >&2
    exit 1
fi

if test "$actual_sha256" != "$box3d_sha256"; then
    printf 'error: Box3D archive checksum mismatch\n' >&2
    printf 'expected: %s\nactual:   %s\n' "$box3d_sha256" "$actual_sha256" >&2
    exit 1
fi

mkdir -p "$unpacked_dir"
tar -xzf "$archive" --strip-components=1 -C "$unpacked_dir"

if ! test -f "${unpacked_dir}/include/box3d/box3d.h" ||
    ! test -f "${unpacked_dir}/src/physics_world.c" ||
    ! test -f "${unpacked_dir}/LICENSE"; then
    printf 'error: downloaded Box3D archive is incomplete\n' >&2
    exit 1
fi

rm -rf -- "$box3d_dir"
mv "$unpacked_dir" "$box3d_dir"
printf '%s\n' "$box3d_commit" > "$marker"
printf 'Downloaded Box3D to %s\n' "$box3d_dir"
