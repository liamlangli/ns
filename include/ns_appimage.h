#pragma once

#include "ns_type.h"

// Linux app packaging without external tools. `ns build` lays out an AppDir
// (AppRun, a .desktop entry, the icon, the program under usr/bin) and packs it
// into a type 2 AppImage:
//
//   Offset  Content
//   0       ns-appimage-runtime, an ELF whose e_ident padding carries "AI\2"
//   elf end squashfs 4.0 image of the AppDir, padded to 4 KiB
//   ...     64-byte trailer: "NSAPPIMG", u64 FNV-1a hash of the image, name
//
// The image starts where the runtime's section header table ends, which is
// where AppImage tools look for it. Blocks are zlib compressed when the
// toolchain was built with third_party/zlib and stored raw otherwise; there
// are no fragments, xattrs, or export table.
//
// The runtime (src/ns_appimage_runtime.c) extracts the image once into
// `$XDG_CACHE_HOME/ns-appimage/<name>-<hash>` and executes its AppRun; the
// trailer hash names that directory, so a rebuilt app extracts afresh.

#define NS_APPIMAGE_TRAILER_MAGIC "NSAPPIMG"
#define NS_APPIMAGE_TRAILER_SIZE 64
#define NS_APPIMAGE_NAME_MAX 48

// Write `app_dir` behind a copy of `runtime` into `output`. `name` labels the
// cache directory; characters outside [A-Za-z0-9._-] become '_'. On failure
// returns false with a message in `error`.
ns_bool ns_appimage_write(const char *runtime, const char *app_dir, const char *name,
                          const char *output, char *error, szt error_len);
