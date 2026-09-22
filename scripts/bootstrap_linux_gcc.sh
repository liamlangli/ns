#!/bin/sh
# Install a user-space copy of the Linux machine's own gcc. SteamOS ships
# GNU ld, crt objects and libc, but not the gcc driver or the /usr/include
# tree. The packages are the ones that machine's pacman would install, so
# the driver is the distro gcc and it links with that machine's GNU ld.
# No root. Usage, on the Linux machine:
#   scripts/bootstrap_linux_gcc.sh
# Or from the Mac:
#   ssh deck 'sh -s' < scripts/bootstrap_linux_gcc.sh
set -eu

prefix="${NS_LINUX_GCC_PREFIX:-${HOME}/ns-linux-toolchain}"
mkdir -p "${prefix}"
work=$(mktemp -d "${TMPDIR:-/tmp}/ns-gcc.XXXXXX")
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

if ! command -v pacman >/dev/null 2>&1; then
    echo "bootstrap_linux_gcc: pacman is required to locate this distro's gcc" >&2
    exit 1
fi

# shellcheck disable=SC2086
urls=$(pacman -Sp gcc libmpc libisl glibc linux-api-headers wayland shaderc vulkan-headers sqlite)
for url in $urls; do
    name=$(basename "$url")
    echo "fetch ${name}"
    curl -fsSL "$url" -o "${work}/${name}"
    tar --zstd -xf "${work}/${name}" -C "${prefix}"
done

gcc="${prefix}/usr/bin/gcc"
if [ ! -x "$gcc" ]; then
    echo "bootstrap_linux_gcc: gcc was not in the extracted package" >&2
    exit 1
fi

# libmpc and libisl are not installed system-wide; the distro gcc is linked
# against them. cc1 is found relative to usr/bin/gcc, so the driver stays the
# one this distro ships and collect2 still invokes the system GNU ld.
wrapper="${prefix}/bin"
mkdir -p "$wrapper"
cat > "${wrapper}/gcc" <<EOF
#!/bin/sh
export LD_LIBRARY_PATH="${prefix}/usr/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
exec "${gcc}" -isystem "${prefix}/usr/include" "\$@"
EOF
chmod 755 "${wrapper}/gcc"

echo 'int main(void){return 0;}' | "${wrapper}/gcc" -x c - -o "${work}/hi"
if ! "${work}/hi"; then
    echo "bootstrap_linux_gcc: a program linked with this gcc did not run" >&2
    exit 1
fi
echo "gcc ${wrapper}/gcc"
file "${work}/hi"
ldd "${work}/hi"
