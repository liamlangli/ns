# Windows x86_64 release cross builder for scripts/release.sh.
# llvm-mingw compiles against the MSYS2 ucrt64 libraries unpacked at /ucrt64,
# the path the Makefile links from; wine runs the built ns.exe when make needs
# it (the nscode apps `make install` builds).
FROM docker.io/library/debian:bookworm
RUN dpkg --add-architecture i386 && apt-get update && apt-get install -y --no-install-recommends \
        make git curl ca-certificates xz-utils zstd python3 wine wine64 \
    && rm -rf /var/lib/apt/lists/*
ARG LLVM_MINGW=llvm-mingw-20241217-ucrt-ubuntu-20.04-x86_64
RUN curl -fsSL https://github.com/mstorsjo/llvm-mingw/releases/download/20241217/$LLVM_MINGW.tar.xz \
        | tar -xJ -C /opt && mv /opt/$LLVM_MINGW /opt/llvm-mingw
# The Makefile calls plain clang and ar; make them the Windows cross tools.
RUN mkdir -p /opt/xbin && \
    ln -s /opt/llvm-mingw/bin/x86_64-w64-mingw32-clang /opt/xbin/clang && \
    ln -s /opt/llvm-mingw/bin/x86_64-w64-mingw32-clang /opt/xbin/cc && \
    ln -s /opt/llvm-mingw/bin/llvm-ar /opt/xbin/ar && \
    ln -s /opt/llvm-mingw/bin/llvm-ranlib /opt/xbin/ranlib
ENV PATH=/opt/xbin:/opt/llvm-mingw/bin:$PATH
COPY msys2_fetch.py /opt/msys2_fetch.py
RUN python3 /opt/msys2_fetch.py / libffi readline dlfcn openssl sqlite3
# MSYS2 clang searches /ucrt64 by default; llvm-mingw needs to be told.
ENV CPATH=/ucrt64/include LIBRARY_PATH=/ucrt64/lib
ENV WINEDEBUG=-all WINEPATH=Z:\\ucrt64\\bin
RUN wineboot --init >/dev/null 2>&1 || true
WORKDIR /work
