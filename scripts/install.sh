#!/bin/sh
# Install Nano Script into ~/.ns.
#
#   curl -fsSL https://liamlangli.com/ns/install.sh | sh
#
# Darwin arm64 and Linux x86_64 download the prebuilt release published
# beside this script. Every other host clones the source and runs
# `make install`, the same as a checkout would. Windows uses install.ps1.
#
# Environment:
#   NS_VERSION         release to install (default: the one named by latest)
#   NS_RELEASE_URL     where releases live (default: https://liamlangli.com/ns)
#   NS_FROM_SOURCE=1   build from source even where a prebuilt release exists
#   NS_GIT_REF         branch or tag a source build checks out (default: main)
#   NS_NO_MODIFY_PATH=1  leave the shell rc file alone

# Everything runs from main at the end, so a truncated download never runs a
# partial script.
set -eu

NS_RELEASE_URL=${NS_RELEASE_URL:-https://liamlangli.com/ns}
NS_REPO_URL=${NS_REPO_URL:-https://github.com/liamlangli/ns.git}
NS_GIT_REF=${NS_GIT_REF:-main}
NS_ROOT="$HOME/.ns"

say() { printf '\033[1;32mns:\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mns:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31mns:\033[0m %s\n' "$*" >&2; exit 1; }

need() {
    command -v "$1" >/dev/null 2>&1 || die "$1 is required"
}

fetch() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then
        wget -q "$1" -O "$2"
    else
        die "curl or wget is required"
    fi
}

sha256_of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        die "shasum or sha256sum is required"
    fi
}

host_platform() {
    os=$(uname -s)
    arch=$(uname -m)
    case "$os" in
    Darwin) os=darwin ;;
    Linux) os=linux ;;
    *) os=unknown ;;
    esac
    case "$arch" in
    arm64 | aarch64) arch=arm64 ;;
    x86_64 | amd64) arch=x86_64 ;;
    esac
    # A shell under Rosetta still reports x86_64 on an arm64 Mac.
    if [ "$os" = darwin ] && [ "$arch" = x86_64 ] &&
        [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = 1 ]; then
        arch=arm64
    fi
    printf '%s-%s' "$os" "$arch"
}

# Copy the staged tree over NS_ROOT. App bundles are replaced whole so no file
# from an older bundle survives; anything else already in NS_ROOT is kept.
install_tree() {
    staged=$1
    mkdir -p "$NS_ROOT"
    for app in "$staged"/bin/*.app; do
        [ -d "$app" ] || continue
        rm -rf "$NS_ROOT/bin/$(basename "$app")"
    done
    cp -R "$staged"/. "$NS_ROOT"/
}

install_prebuilt() {
    platform=$1
    work=$2
    version=${NS_VERSION:-}
    if [ -z "$version" ]; then
        fetch "$NS_RELEASE_URL/latest" "$work/latest" || die "cannot reach $NS_RELEASE_URL/latest"
        version=$(tr -d ' \r\n' <"$work/latest")
    fi
    [ -n "$version" ] || die "no release version published"
    archive="ns-$platform.tar.gz"
    base="$NS_RELEASE_URL/$version"

    say "downloading $version for $platform"
    fetch "$base/$archive" "$work/$archive" || die "no prebuilt release at $base/$archive"
    fetch "$base/$archive.sha256" "$work/$archive.sha256" || die "missing checksum $base/$archive.sha256"
    expected=$(cut -d' ' -f1 <"$work/$archive.sha256")
    actual=$(sha256_of "$work/$archive")
    [ "$expected" = "$actual" ] || die "checksum mismatch for $archive"

    mkdir -p "$work/stage"
    tar -xzf "$work/$archive" -C "$work/stage"
    [ -x "$work/stage/bin/ns" ] || die "archive has no bin/ns"
    install_tree "$work/stage"
}

install_from_source() {
    work=$1
    need git
    need make
    say "building from source ($NS_REPO_URL, $NS_GIT_REF)"
    git clone --depth 1 --branch "$NS_GIT_REF" "$NS_REPO_URL" "$work/src"
    if [ "$(uname -s)" = Linux ]; then
        sh "$work/src/scripts/install_linux_deps.sh" ||
            warn "could not install every Linux build package; continuing"
    fi
    # The Makefile installs into $(NS_HOME)/.ns, which is NS_ROOT.
    (cd "$work/src" && make NS_DEBUG=0 install NS_HOME="$HOME") ||
        die "make install failed; see the output above"
}

# The prebuilt Linux release links the distribution's shared libraries; name
# the ones this machine lacks instead of failing on the first one.
check_linux_libraries() {
    command -v ldd >/dev/null 2>&1 || return 0
    missing=$(for f in "$NS_ROOT"/bin/ns "$NS_ROOT"/lib/*.so; do
        [ -f "$f" ] && ldd "$f" 2>/dev/null | sed -n 's/^[[:space:]]*\([^ ]*\) => not found.*/\1/p'
    done | sort -u | tr '\n' ' ')
    [ -n "$missing" ] || return 0
    warn "missing shared libraries: $missing"
    warn "on Debian/Ubuntu: sudo apt-get install libreadline8 libffi8 libsqlite3-0 libssl3 libvulkan1 libwayland-client0 libwayland-cursor0"
    warn "ns build also needs a C toolchain: sudo apt-get install gcc"
}

shell_rc() {
    case "${SHELL:-}" in
    */zsh) printf '%s' "${ZDOTDIR:-$HOME}/.zshrc" ;;
    */bash)
        if [ "$(uname -s)" = Darwin ]; then
            printf '%s' "$HOME/.bash_profile"
        else
            printf '%s' "$HOME/.bashrc"
        fi
        ;;
    */fish) printf '%s' "$HOME/.config/fish/config.fish" ;;
    *) printf '%s' "$HOME/.profile" ;;
    esac
}

add_to_path() {
    bin="$NS_ROOT/bin"
    case ":$PATH:" in *":$bin:"*) return 0 ;; esac
    rc=$(shell_rc)
    case "$rc" in
    *.fish) line="fish_add_path $bin" ;;
    *) line="export PATH=\"$bin:\$PATH\"" ;;
    esac
    if [ "${NS_NO_MODIFY_PATH:-0}" = 1 ]; then
        say "add ns to PATH with:"
        printf '        echo '\''%s'\'' >> %s\n' "$line" "$rc"
        return 0
    fi
    if [ -f "$rc" ] && grep -F "$bin" "$rc" >/dev/null 2>&1; then
        :
    else
        mkdir -p "$(dirname "$rc")"
        printf '\n# Nano Script\n%s\n' "$line" >>"$rc"
        say "added $bin to PATH in $rc"
    fi
    say "restart the shell or run: $line"
}

main() {
    need uname
    need tar
    platform=$(host_platform)
    work=$(mktemp -d "${TMPDIR:-/tmp}/ns-install.XXXXXX")
    trap 'rm -rf "$work"' EXIT
    trap 'exit 1' HUP INT TERM

    case "$platform" in
    darwin-arm64 | linux-x86_64)
        if [ "${NS_FROM_SOURCE:-0}" = 1 ]; then
            install_from_source "$work"
        else
            install_prebuilt "$platform" "$work"
        fi
        ;;
    darwin-* | linux-*)
        install_from_source "$work"
        ;;
    *)
        die "unsupported host $platform; on Windows build from source with MSYS2"
        ;;
    esac

    if [ "$platform" = linux-x86_64 ]; then
        check_linux_libraries
    fi
    "$NS_ROOT/bin/ns" --version >/dev/null 2>&1 || die "installed ns does not run"
    say "installed $("$NS_ROOT/bin/ns" --version 2>&1 | sed 's/^.*: *//') into $NS_ROOT"
    add_to_path
    say "get started: ns create hello && cd hello && ns run"
}

main "$@"
