#!/bin/sh
# Build the Linux feature modules with this machine's gcc. Run on the Linux
# host, from a tree laid out like the ns repo (include/, lib/, src/, third_party/).
# The gcc wrapper from bootstrap_linux_gcc.sh drives the system GNU ld.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"

gcc="${NS_LINUX_GCC:-${HOME}/.ns/toolchain/bin/gcc}"
out="${root}/bin/linux-x86_64"
obj="${out}/obj"
mkdir -p "$obj" "$obj/zlib"

cflags="-fPIC -g -O2 -Wall -Wextra -Werror -DNS_LINUX -I${root}/include -I${root}/include/os -I${root}/include/asm -I${root}/lib/include -I${root}/lib/src"
soflags="-shared -fPIC -Wl,-rpath,\$ORIGIN -Wl,--export-dynamic -pthread -ldl -lm"

cc() {
    echo "cc $1"
    # shellcheck disable=SC2086
    "$gcc" -c $cflags $2 -o "$1" "$3"
}

"$gcc" -c $cflags -I"${root}/third_party/zlib" -DZ_PREFIX=1 -DZ_HAVE_UNISTD_H \
    -o "$obj/compress.o" "${root}/lib/src/compress.c"

for src in io os os.linux net http wasm_dev term.posix view view.linux xdg-shell-protocol gpu gpu.vulkan ui audio.stub storage.db storage.cache storage.json; do
    cc "$obj/${src}.o" "" "${root}/lib/src/${src}.c"
done

for src in "${root}/third_party/zlib"/*.c; do
    base=$(basename "$src" .c)
    "$gcc" -c -fPIC -g -O2 -fvisibility=hidden -DZ_PREFIX=1 -DZ_HAVE_UNISTD_H -DNS_LINUX \
        -I"${root}/third_party/zlib" -Wno-error -o "$obj/zlib/${base}.o" "$src"
done

link() {
    name=$1
    shift
    echo "link ${name}.so"
    # shellcheck disable=SC2086
    "$gcc" $soflags -Wl,-soname,"${name}.so" "$@" -o "$out/${name}.so"
}

link io "$obj/io.o"
link os "$obj/os.o" "$obj/os.linux.o"
link net "$obj/net.o"
link http "$obj/http.o" "$out/net.so"
link wasm_dev "$obj/wasm_dev.o"
link term "$obj/term.posix.o"
link view "$obj/view.o" "$obj/view.linux.o" "$obj/xdg-shell-protocol.o" -lwayland-client -lwayland-cursor
link gpu "$obj/gpu.o" "$obj/gpu.vulkan.o" -lvulkan -lshaderc_shared
link ui "$obj/ui.o" "$out/io.so" "$out/gpu.so" "$out/view.so"
link audio "$obj/audio.stub.o"
link storage "$obj/storage.db.o" "$obj/storage.cache.o" "$obj/storage.json.o" -lsqlite3
link compress "$obj/compress.o" "$obj/zlib"/*.o

# The launcher `ns build` puts in front of every Linux AppImage.
echo "cc ns-appimage-runtime"
"$gcc" -O2 -Wall -Wextra -Werror -I"${root}/third_party/zlib" -DZ_PREFIX=1 -DZ_HAVE_UNISTD_H -DNS_ZLIB \
    -o "$out/ns-appimage-runtime" "${root}/src/ns_appimage_runtime.c" "$obj/zlib"/*.o

echo "modules ${out}"
