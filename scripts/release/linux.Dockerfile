# Linux x86_64 release builder for scripts/release.sh.
# Debian bookworm keeps the glibc floor at 2.36 so the build runs on older
# distributions than the host that builds it.
FROM docker.io/library/debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang lld git curl ca-certificates pkg-config \
        libreadline-dev libffi-dev libsqlite3-dev libssl-dev \
        libwayland-dev wayland-protocols libxkbcommon-dev libvulkan-dev libshaderc-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
