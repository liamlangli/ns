NS_LIB_INC = $(NS_INC) -Ilib/include -Ithird_party/box3d/include
NS_BOX3D_INC = -Ithird_party/box3d/include -Ithird_party/box3d/src
NS_BOX3D_OBJDIR = $(NS_BINDIR)/box3d
NS_BOX3D_HEADERS = $(wildcard third_party/box3d/include/box3d/*.h third_party/box3d/src/*.h third_party/box3d/src/*.inl)
NS_BOX3D_SCRIPT = scripts/download_box3d.sh
NS_BOX3D_PRESENT = $(wildcard third_party/box3d/include/box3d/box3d.h)

NS_ZLIB_DIR = third_party/zlib
NS_ZSTD_DIR = third_party/zstd
NS_ZLIB_INC = -I$(NS_ZLIB_DIR)
NS_ZSTD_INC = -I$(NS_ZSTD_DIR)/lib
# Z_PREFIX renames every zlib entry point to z_*, so the copy statically linked
# into compress$(NS_DYLIB_SUFFIX) cannot collide with a system libz that is
# already in the process -- the module is dlopen'd with RTLD_GLOBAL.
NS_ZLIB_DEF = -DZ_PREFIX=1
ifneq ($(NS_OS), $(NS_WIN))
	# zlib's gz* file layer calls read/write/close/lseek directly; without this
	# its own zconf.h never pulls in <unistd.h> to declare them.
	NS_ZLIB_DEF += -DZ_HAVE_UNISTD_H
endif
# XXH_NAMESPACE does the same for the xxHash symbols Zstandard embeds.
# ZSTD_DISABLE_ASM keeps the build on the portable C decoder: the x86-64
# huf_decompress_amd64.S is not covered by the *.c wildcard below.
NS_ZSTD_DEF = -DZSTD_DISABLE_ASM=1 -DZSTD_LEGACY_SUPPORT=0 -DXXH_NAMESPACE=ZSTD_
NS_ZLIB_OBJDIR = $(NS_BINDIR)/zlib
NS_ZSTD_OBJDIR = $(NS_BINDIR)/zstd
NS_ZLIB_HEADERS = $(wildcard $(NS_ZLIB_DIR)/*.h)
NS_ZSTD_HEADERS = $(wildcard $(NS_ZSTD_DIR)/lib/*.h $(NS_ZSTD_DIR)/lib/common/*.h \
	$(NS_ZSTD_DIR)/lib/compress/*.h $(NS_ZSTD_DIR)/lib/decompress/*.h)
NS_ZLIB_SCRIPT = scripts/download_zlib.sh
NS_ZSTD_SCRIPT = scripts/download_zstd.sh
NS_COMPRESS_PRESENT = $(and $(wildcard $(NS_ZLIB_DIR)/zlib.h),$(wildcard $(NS_ZSTD_DIR)/lib/zstd.h))

ifeq ($(NS_OS), $(NS_DARWIN))
	NS_DYLIB_CREATE = -dynamiclib
	NS_DYLIB_COMMON = -fPIC -g -Wl,-install_name,@rpath/$(@F) -Wl,-rpath,@loader_path -undefined dynamic_lookup
	NS_DYLIB_DEPS = $(NS_LDFLAGS)
	NS_OS_DYLIB_DEPS = $(NS_DYLIB_DEPS) $(NS_COCOA_LDFLAGS) -framework CoreServices -framework CoreHaptics
	NS_VIEW_DYLIB_DEPS = $(NS_LDFLAGS) $(NS_COCOA_LDFLAGS) $(NS_METAL_LDFLAGS)
	NS_GPU_DYLIB_DEPS = $(NS_LDFLAGS) $(NS_METAL_LDFLAGS)
	NS_AUDIO_DYLIB_DEPS = $(NS_LDFLAGS) -framework AVFAudio -framework Foundation
	NS_STORAGE_DYLIB_DEPS = $(NS_LDFLAGS) -framework Foundation -lsqlite3
else ifeq ($(NS_OS), $(NS_LINUX))
	NS_DYLIB_CREATE = -shared
	NS_DYLIB_COMMON = -fPIC -g -Wl,-soname,$(@F) -Wl,-rpath,'$$ORIGIN'
	NS_DYLIB_DEPS = $(NS_LDFLAGS)
else
	NS_DYLIB_CREATE = -shared
	NS_DYLIB_COMMON = -fPIC -g
	NS_DYLIB_DEPS = $(NS_LDFLAGS)
	NS_OS_DYLIB_DEPS = $(NS_DYLIB_DEPS) -lcomdlg32 -lshell32 -lole32
	NS_GPU_DYLIB_DEPS = $(NS_LDFLAGS) $(NS_GPU_LDFLAGS)
endif

NS_OS_DYLIB_DEPS ?= $(NS_DYLIB_DEPS)
NS_VIEW_DYLIB_DEPS ?= $(NS_DYLIB_DEPS)
NS_GPU_DYLIB_DEPS ?= $(NS_DYLIB_DEPS)
NS_UI_DYLIB_DEPS ?= $(NS_GPU_DYLIB_DEPS)
NS_AUDIO_DYLIB_DEPS ?= $(NS_DYLIB_DEPS)
NS_STORAGE_DYLIB_DEPS ?= $(NS_DYLIB_DEPS) -lsqlite3

# Position-independent compile rules for the C and Objective-C feature sources.
$(NS_BINDIR)/lib/%.o: lib/src/%.c
	$(NS_CC) -c -fPIC -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

$(NS_BINDIR)/lib/%.o: lib/src/%.m
	$(NS_CC) -c -fPIC -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

NS_IO_DYLIB = $(NS_BINDIR)/io$(NS_DYLIB_SUFFIX)
NS_OS_DYLIB = $(NS_BINDIR)/os$(NS_DYLIB_SUFFIX)
NS_NET_DYLIB = $(NS_BINDIR)/net$(NS_DYLIB_SUFFIX)
NS_HTTP_DYLIB = $(NS_BINDIR)/http$(NS_DYLIB_SUFFIX)
NS_WASM_DEV_DYLIB = $(NS_BINDIR)/wasm_dev$(NS_DYLIB_SUFFIX)
NS_TERM_DYLIB = $(NS_BINDIR)/term$(NS_DYLIB_SUFFIX)
NS_VIEW_DYLIB = $(NS_BINDIR)/view$(NS_DYLIB_SUFFIX)
NS_GPU_DYLIB = $(NS_BINDIR)/gpu$(NS_DYLIB_SUFFIX)
NS_UI_DYLIB = $(NS_BINDIR)/ui$(NS_DYLIB_SUFFIX)
NS_AUDIO_DYLIB = $(NS_BINDIR)/audio$(NS_DYLIB_SUFFIX)
NS_DYNAMIC_DYLIB = $(NS_BINDIR)/dynamic$(NS_DYLIB_SUFFIX)
NS_COMPRESS_DYLIB = $(NS_BINDIR)/compress$(NS_DYLIB_SUFFIX)
NS_STORAGE_DYLIB = $(NS_BINDIR)/storage$(NS_DYLIB_SUFFIX)

NS_IO_OBJS = $(NS_BINDIR)/lib/io.o
NS_OS_OBJS = $(NS_BINDIR)/lib/os.o
NS_NET_OBJS = $(NS_BINDIR)/lib/net.o
NS_HTTP_OBJS = $(NS_BINDIR)/lib/http.o
NS_WASM_DEV_OBJS = $(NS_BINDIR)/lib/wasm_dev.o
NS_VIEW_OBJS = $(NS_BINDIR)/lib/view.o
NS_GPU_OBJS = $(NS_BINDIR)/lib/gpu.o
NS_UI_OBJS = $(NS_BINDIR)/lib/ui.o
NS_AUDIO_OBJS =
NS_BOX3D_SRCS = $(wildcard third_party/box3d/src/*.c)
NS_BOX3D_OBJS = $(NS_BOX3D_SRCS:third_party/box3d/src/%.c=$(NS_BOX3D_OBJDIR)/%.o)
NS_DYNAMIC_OBJS = $(NS_BINDIR)/lib/dynamic.o $(NS_BOX3D_OBJS)
NS_ZLIB_SRCS = $(wildcard $(NS_ZLIB_DIR)/*.c)
NS_ZLIB_OBJS = $(NS_ZLIB_SRCS:$(NS_ZLIB_DIR)/%.c=$(NS_ZLIB_OBJDIR)/%.o)
NS_ZSTD_SRCS = $(wildcard $(NS_ZSTD_DIR)/lib/common/*.c $(NS_ZSTD_DIR)/lib/compress/*.c \
	$(NS_ZSTD_DIR)/lib/decompress/*.c)
NS_ZSTD_OBJS = $(NS_ZSTD_SRCS:$(NS_ZSTD_DIR)/lib/%.c=$(NS_ZSTD_OBJDIR)/%.o)
NS_COMPRESS_OBJS = $(NS_BINDIR)/lib/compress.o $(NS_ZLIB_OBJS) $(NS_ZSTD_OBJS)
NS_STORAGE_OBJS = $(NS_BINDIR)/lib/storage.db.o $(NS_BINDIR)/lib/storage.cache.o

ifeq ($(NS_OS), $(NS_LINUX))
	NS_OS_OBJS += $(NS_BINDIR)/lib/os.linux.o
	NS_TERM_OBJS = $(NS_BINDIR)/lib/term.posix.o
	NS_VIEW_OBJS += $(NS_BINDIR)/lib/view.linux.o
	NS_STORAGE_OBJS += $(NS_BINDIR)/lib/storage.json.o
else ifeq ($(NS_OS), $(NS_DARWIN))
	NS_OS_OBJS += $(NS_BINDIR)/lib/os.osx.o $(NS_BINDIR)/lib/os.haptic.apple.o
	NS_TERM_OBJS = $(NS_BINDIR)/lib/term.posix.o
	NS_VIEW_OBJS += $(NS_BINDIR)/lib/view.osx.o
	NS_GPU_OBJS += $(NS_BINDIR)/lib/gpu.metal.o
	NS_AUDIO_OBJS = $(NS_BINDIR)/lib/audio.apple.o
	NS_STORAGE_OBJS += $(NS_BINDIR)/lib/storage.apple.o
else ifeq ($(NS_OS), $(NS_WIN))
	NS_OS_OBJS += $(NS_BINDIR)/lib/os.win.o
	NS_TERM_OBJS = $(NS_BINDIR)/lib/term.win.o
	NS_VIEW_OBJS += $(NS_BINDIR)/lib/view.win.o
	NS_GPU_OBJS += $(NS_BINDIR)/lib/gpu.dx12.o
	NS_STORAGE_OBJS += $(NS_BINDIR)/lib/storage.json.o
endif

$(NS_BINDIR)/lib/dynamic.o: lib/include/dynamic.h $(NS_BOX3D_HEADERS)

# The adapter itself sees zlib and Zstandard headers, and must be built with the
# same Z_PREFIX renaming as the zlib objects it links against.
$(NS_BINDIR)/lib/compress.o: lib/src/compress.c lib/include/compress.h $(NS_ZLIB_HEADERS) $(NS_ZSTD_HEADERS) | $(NS_DIRS)
	$(NS_CC) -c -fPIC -g $< -o $@ $(NS_LIB_INC) $(NS_ZLIB_INC) $(NS_ZSTD_INC) $(NS_ZLIB_DEF) $(NS_CFLAGS)

NS_STD_SHARED = \
	$(NS_IO_DYLIB) \
	$(NS_OS_DYLIB) \
	$(NS_NET_DYLIB) \
	$(NS_HTTP_DYLIB) \
	$(NS_WASM_DEV_DYLIB) \
	$(NS_TERM_DYLIB) \
	$(NS_GPU_DYLIB) \
	$(NS_VIEW_DYLIB) \
	$(NS_UI_DYLIB) \
	$(NS_DYNAMIC_DYLIB) \
	$(NS_COMPRESS_DYLIB) \
	$(NS_STORAGE_DYLIB)

ifeq ($(NS_OS), $(NS_DARWIN))
	NS_STD_SHARED += $(NS_AUDIO_DYLIB)
endif

std: $(NS_STD_SHARED)

shared: std

.PHONY: box3d
box3d:
	$(NS_BOX3D_SCRIPT)

$(NS_BOX3D_OBJDIR):
	$(NS_MKDIR) $@

$(NS_BOX3D_OBJDIR)/%.o: third_party/box3d/src/%.c $(NS_BOX3D_HEADERS) | $(NS_BOX3D_OBJDIR)
	$(NS_CC) -c -fPIC -g -O2 -std=gnu17 $< -o $@ $(NS_BOX3D_INC) $(NS_PLATFORM_DEF) -Wno-error

.PHONY: zlib zstd compress_deps
zlib:
	$(NS_ZLIB_SCRIPT)

zstd:
	$(NS_ZSTD_SCRIPT)

compress_deps: zlib zstd

$(NS_ZLIB_OBJDIR):
	$(NS_MKDIR) $@

$(NS_ZLIB_OBJDIR)/%.o: $(NS_ZLIB_DIR)/%.c $(NS_ZLIB_HEADERS) | $(NS_ZLIB_OBJDIR)
	$(NS_CC) -c -fPIC -g -O2 -fvisibility=hidden $< -o $@ $(NS_ZLIB_INC) $(NS_ZLIB_DEF) $(NS_PLATFORM_DEF) -Wno-error

# Zstandard keeps its sources in common/, compress/ and decompress/, so the
# object tree mirrors those three directories.
NS_ZSTD_OBJDIRS = $(NS_ZSTD_OBJDIR)/common $(NS_ZSTD_OBJDIR)/compress $(NS_ZSTD_OBJDIR)/decompress

$(NS_ZSTD_OBJDIRS):
	$(NS_MKDIR) $@

$(NS_ZSTD_OBJDIR)/%.o: $(NS_ZSTD_DIR)/lib/%.c $(NS_ZSTD_HEADERS) | $(NS_ZSTD_OBJDIRS)
	$(NS_CC) -c -fPIC -g -O2 -fvisibility=hidden $< -o $@ $(NS_ZSTD_INC) $(NS_ZSTD_DEF) $(NS_PLATFORM_DEF) -Wno-error

$(NS_IO_DYLIB): $(NS_IO_OBJS) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_DYLIB_DEPS)

$(NS_OS_DYLIB): $(NS_OS_OBJS) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_OS_DYLIB_DEPS)

$(NS_NET_DYLIB): $(NS_NET_OBJS) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_DYLIB_DEPS)

$(NS_HTTP_DYLIB): $(NS_HTTP_OBJS) $(NS_NET_DYLIB) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_HTTP_OBJS) $(NS_NET_DYLIB) -o $@ $(NS_DYLIB_DEPS)

$(NS_WASM_DEV_DYLIB): $(NS_WASM_DEV_OBJS) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_DYLIB_DEPS)

$(NS_TERM_DYLIB): $(NS_TERM_OBJS) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_DYLIB_DEPS)

$(NS_GPU_DYLIB): $(NS_GPU_OBJS) $(if $(filter $(NS_WIN),$(NS_OS)),$(NS_VIEW_DYLIB),) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_GPU_OBJS) $(if $(filter $(NS_WIN),$(NS_OS)),$(NS_VIEW_DYLIB),) -o $@ $(NS_GPU_DYLIB_DEPS)

$(NS_VIEW_DYLIB): $(NS_VIEW_OBJS) $(if $(filter $(NS_DARWIN),$(NS_OS)),$(NS_GPU_DYLIB),) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_VIEW_OBJS) $(if $(filter $(NS_DARWIN),$(NS_OS)),$(NS_GPU_DYLIB),) -o $@ $(NS_VIEW_DYLIB_DEPS)

$(NS_UI_DYLIB): $(NS_UI_OBJS) $(NS_IO_DYLIB) $(NS_GPU_DYLIB) $(NS_VIEW_DYLIB) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_UI_OBJS) $(NS_IO_DYLIB) $(NS_GPU_DYLIB) $(NS_VIEW_DYLIB) -o $@ $(NS_UI_DYLIB_DEPS)

$(NS_AUDIO_DYLIB): $(NS_AUDIO_OBJS) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_AUDIO_DYLIB_DEPS)

$(NS_STORAGE_DYLIB): $(NS_STORAGE_OBJS) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_STORAGE_DYLIB_DEPS)

ifeq ($(NS_BOX3D_PRESENT),)
$(NS_DYNAMIC_DYLIB): box3d | $(NS_BINDIR)
	$(MAKE) $@
else
$(NS_DYNAMIC_DYLIB): $(NS_DYNAMIC_OBJS) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_DYNAMIC_OBJS) -o $@ $(NS_DYLIB_DEPS)
endif

# The source wildcards above are expanded before the download runs, so the first
# build downloads the sources and then re-enters make to pick them up.
ifeq ($(NS_COMPRESS_PRESENT),)
$(NS_COMPRESS_DYLIB): compress_deps | $(NS_BINDIR)
	$(MAKE) $@
else
$(NS_COMPRESS_DYLIB): $(NS_COMPRESS_OBJS) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_COMPRESS_OBJS) -o $@ $(NS_DYLIB_DEPS)
endif
