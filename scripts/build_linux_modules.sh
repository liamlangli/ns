#!/bin/sh
# Build bin/linux-x86_64/*.so on NS_LINUX_HOST with that machine's gcc.
set -eu

if [ -z "${NS_LINUX_HOST:-}" ]; then
    echo "build_linux_modules: set NS_LINUX_HOST to the Linux machine" >&2
    exit 1
fi

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
host=$NS_LINUX_HOST
remote="${NS_LINUX_REMOTE:-.ns/src}"

ssh "$host" "mkdir -p ${remote}/include ${remote}/lib/include ${remote}/lib/src ${remote}/third_party/zlib ${remote}/third_party/zstd/lib ${remote}/third_party/box3d ${remote}/scripts"
rsync -a --delete "${root}/include/" "${host}:${remote}/include/"
rsync -a --delete "${root}/lib/include/" "${host}:${remote}/lib/include/"
rsync -a --delete "${root}/lib/src/" "${host}:${remote}/lib/src/"
rsync -a --delete "${root}/third_party/zlib/" "${host}:${remote}/third_party/zlib/"
rsync -a --delete "${root}/third_party/zstd/lib/" "${host}:${remote}/third_party/zstd/lib/"
rsync -a --delete "${root}/third_party/box3d/" "${host}:${remote}/third_party/box3d/"
rsync -a "${root}/scripts/compile_linux_modules.sh" "${host}:${remote}/scripts/compile_linux_modules.sh"

ssh "$host" "sh ${remote}/scripts/compile_linux_modules.sh"
mkdir -p "${root}/bin/linux-x86_64"
rsync -a "${host}:${remote}/bin/linux-x86_64/"*.so "${root}/bin/linux-x86_64/"
echo "linux modules ${root}/bin/linux-x86_64"
