#!/bin/sh

set -eu

# Install the Linux/WSL packages `make` needs to compile ns:
#   build-essential libreadline-dev libffi-dev libsqlite3-dev
# curl is used by the third_party download scripts; pkg-config is optional.
# nodejs is used by `make test` wasm checks and is installed when apt can.
#
# With passwordless sudo this uses apt-get. Otherwise the -dev packages are
# downloaded and extracted into $HOME/.local/ns-deps (override with
# NS_LINUX_DEPS). The Makefile searches that prefix automatically.

pkgs="build-essential libreadline-dev libffi-dev libsqlite3-dev pkg-config curl"
optional_pkgs="nodejs"
dev_pkgs="libreadline-dev libffi-dev libsqlite3-dev"
prefix="${NS_LINUX_DEPS:-${HOME}/.local/ns-deps}"
multiarch=$(gcc -print-multiarch 2>/dev/null || true)
if test -z "$multiarch"; then
    multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || true)
fi
if test -z "$multiarch"; then
    multiarch="x86_64-linux-gnu"
fi
libdir="${prefix}/usr/lib/${multiarch}"
syslib="/usr/lib/${multiarch}"

have_system_headers() {
    test -f /usr/include/readline/readline.h &&
        test -f /usr/include/sqlite3.h &&
        {
            test -f /usr/include/ffi.h ||
                test -f /usr/include/${multiarch}/ffi.h
        }
}

have_prefix_headers() {
    test -f "${prefix}/usr/include/readline/readline.h" &&
        test -f "${prefix}/usr/include/sqlite3.h" &&
        {
            test -f "${prefix}/usr/include/ffi.h" ||
                test -f "${prefix}/usr/include/${multiarch}/ffi.h"
        }
}

# The -dev packages ship libfoo.so as a relative symlink to libfoo.so.N, which
# lives in the runtime package. Point the linker stub at the system shared
# library and drop the static archive so ld does not fall back to a non-PIC .a.
link_system_shared() {
    stub="$1"
    soname="$2"
    mkdir -p "$libdir"
    if test -e "${syslib}/${soname}"; then
        ln -sfn "${syslib}/${soname}" "${libdir}/${stub}"
    elif test -e "${libdir}/${soname}"; then
        ln -sfn "${soname}" "${libdir}/${stub}"
    else
        printf 'error: %s not found under %s or %s\n' "$soname" "$syslib" "$libdir" >&2
        return 1
    fi
    rm -f "${libdir}/${stub%.so}.a"
}

fix_prefix_linker_stubs() {
    link_system_shared libreadline.so libreadline.so.8
    link_system_shared libffi.so libffi.so.8
    link_system_shared libsqlite3.so libsqlite3.so.0
}

if have_system_headers; then
    printf 'Linux build headers are already installed under /usr.\n'
    exit 0
fi

if command -v apt-get >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y $pkgs
    sudo apt-get install -y $optional_pkgs || true
    if have_system_headers; then
        printf 'Installed Linux build packages with apt-get.\n'
        exit 0
    fi
    printf 'error: apt-get finished but readline/ffi/sqlite headers are still missing\n' >&2
    exit 1
fi

if have_prefix_headers; then
    fix_prefix_linker_stubs
    printf 'Linux -dev packages are already extracted in %s\n' "$prefix"
    printf 'The Makefile searches this prefix automatically.\n'
    exit 0
fi

command -v apt-get >/dev/null 2>&1 || {
    printf 'error: apt-get is required to download Linux -dev packages\n' >&2
    exit 1
}
command -v dpkg-deb >/dev/null 2>&1 || {
    printf 'error: dpkg-deb is required to unpack Linux -dev packages\n' >&2
    exit 1
}

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/ns-linux-deps.XXXXXX")
cleanup() {
    rm -rf -- "$temporary_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

printf 'sudo is unavailable; extracting %s into %s\n' "$dev_pkgs" "$prefix"
mkdir -p "$temporary_dir" "$prefix"
(
    cd "$temporary_dir"
    # shellcheck disable=SC2086
    apt-get download $dev_pkgs
    for deb in *.deb; do
        test -f "$deb" || continue
        dpkg-deb -x "$deb" "$prefix"
    done
)

if ! have_prefix_headers; then
    printf 'error: failed to extract readline/ffi/sqlite headers into %s\n' "$prefix" >&2
    exit 1
fi
fix_prefix_linker_stubs

printf 'Extracted Linux -dev packages to %s\n' "$prefix"
printf 'The Makefile searches this prefix automatically.\n'
printf 'To install system-wide later: sudo apt-get install -y %s\n' "$pkgs"
