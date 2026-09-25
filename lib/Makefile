NS_LIB_INC = $(NS_INC) -Ilib/include

NS_ZLIB_DIR = third_party/zlib
NS_ZLIB_INC = -I$(NS_ZLIB_DIR)
# Z_PREFIX renames every zlib entry point to z_*, so the copy statically linked
# into compress$(NS_DYLIB_SUFFIX) cannot collide with a system libz that is
# already in the process -- the module is dlopen'd with RTLD_GLOBAL.
NS_ZLIB_DEF = -DZ_PREFIX=1
ifneq ($(NS_OS), $(NS_WIN))
	# zlib's gz* file layer calls read/write/close/lseek directly; without this
	# its own zconf.h never pulls in <unistd.h> to declare them.
	NS_ZLIB_DEF += -DZ_HAVE_UNISTD_H
endif
NS_ZLIB_OBJDIR = $(NS_BINDIR)/zlib
NS_ZLIB_HEADERS = $(wildcard $(NS_ZLIB_DIR)/*.h)
NS_ZLIB_SCRIPT = scripts/download_zlib.sh
NS_COMPRESS_PRESENT = $(wildcard $(NS_ZLIB_DIR)/zlib.h)

ifeq ($(NS_OS), $(NS_DARWIN))
	NS_DYLIB_CREATE = -dynamiclib
	NS_DYLIB_COMMON = -fPIC -g -Wl,-install_name,@rpath/$(@F) -Wl,-rpath,@loader_path -undefined dynamic_lookup
	NS_DYLIB_DEPS = $(NS_LDFLAGS)
	NS_OS_DYLIB_DEPS = $(NS_DYLIB_DEPS) $(NS_COCOA_LDFLAGS) -framework CoreServices -framework CoreHaptics
	NS_VIEW_DYLIB_DEPS = $(NS_LDFLAGS) $(NS_COCOA_LDFLAGS) $(NS_METAL_LDFLAGS) -framework GameController
	NS_GPU_DYLIB_DEPS = $(NS_LDFLAGS) $(NS_METAL_LDFLAGS)
	NS_AUDIO_DYLIB_DEPS = $(NS_LDFLAGS) -framework AVFAudio -framework Foundation
	NS_STORAGE_DYLIB_DEPS = $(NS_LDFLAGS) -framework Foundation -lsqlite3
else ifeq ($(NS_OS), $(NS_LINUX))
	NS_DYLIB_CREATE = -shared
	NS_DYLIB_COMMON = -fPIC -g -Wl,-soname,$(@F) -Wl,-rpath,'$$ORIGIN'
	NS_DYLIB_DEPS = $(NS_LDFLAGS)
	# Wayland and Vulkan are the Linux view and GPU backends. shaderc compiles
	# the GLSL the shader module emits into SPIR-V at runtime, exactly as the
	# Metal backend compiles MSL.
	# The LunarG SDK ships the shaderc runtime as libshaderc_shared.so; distro
	# packages (libshaderc-dev) name it libshaderc.so. Link whichever is here.
	NS_SHADERC_LIB := $(if $(filter-out libshaderc_shared.so,$(shell $(NS_CC) -print-file-name=libshaderc_shared.so)),-lshaderc_shared,-lshaderc)
	NS_WAYLAND_LDFLAGS = -lwayland-client -lwayland-cursor
	NS_VULKAN_LDFLAGS = -lvulkan $(NS_SHADERC_LIB)
	NS_VIEW_DYLIB_DEPS = $(NS_DYLIB_DEPS) $(NS_WAYLAND_LDFLAGS)
	NS_GPU_DYLIB_DEPS = $(NS_DYLIB_DEPS) $(NS_VULKAN_LDFLAGS)
else
	NS_DYLIB_CREATE = -shared
	NS_DYLIB_COMMON = -fPIC -g
	NS_DYLIB_DEPS = $(NS_LDFLAGS)
	NS_OS_DYLIB_DEPS = $(NS_DYLIB_DEPS) -lcomdlg32 -lshell32 -lole32
	NS_VIEW_DYLIB_DEPS = $(NS_DYLIB_DEPS) -lxinput
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
	$(Q)$(call ns_step,CC,$<,quiet) $(NS_CC) -c -fPIC -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

$(NS_BINDIR)/lib/%.o: lib/src/%.m
	$(Q)$(call ns_step,CC,$<,quiet) $(NS_CC) -c -fPIC -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

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
NS_COMPRESS_DYLIB = $(NS_BINDIR)/compress$(NS_DYLIB_SUFFIX)
NS_STORAGE_DYLIB = $(NS_BINDIR)/storage$(NS_DYLIB_SUFFIX)

NS_IO_STATIC = $(NS_BINDIR)/libio.a
NS_OS_STATIC = $(NS_BINDIR)/libos.a
NS_NET_STATIC = $(NS_BINDIR)/libnet.a
NS_HTTP_STATIC = $(NS_BINDIR)/libhttp.a
NS_WASM_DEV_STATIC = $(NS_BINDIR)/libwasm_dev.a
NS_TERM_STATIC = $(NS_BINDIR)/libterm.a
NS_VIEW_STATIC = $(NS_BINDIR)/libview.a
NS_GPU_STATIC = $(NS_BINDIR)/libgpu.a
NS_UI_STATIC = $(NS_BINDIR)/libui.a
NS_AUDIO_STATIC = $(NS_BINDIR)/libaudio.a
NS_COMPRESS_STATIC = $(NS_BINDIR)/libcompress.a
NS_STORAGE_STATIC = $(NS_BINDIR)/libstorage.a

NS_IO_OBJS = $(NS_BINDIR)/lib/io.o
NS_OS_OBJS = $(NS_BINDIR)/lib/os.o
NS_NET_OBJS = $(NS_BINDIR)/lib/net.o
NS_HTTP_OBJS = $(NS_BINDIR)/lib/http.o
NS_WASM_DEV_OBJS = $(NS_BINDIR)/lib/wasm_dev.o
NS_VIEW_OBJS = $(NS_BINDIR)/lib/view.o
NS_GPU_OBJS = $(NS_BINDIR)/lib/gpu.o
NS_UI_OBJS = $(NS_BINDIR)/lib/ui.o
NS_AUDIO_OBJS =
NS_ZLIB_SRCS = $(wildcard $(NS_ZLIB_DIR)/*.c)
NS_ZLIB_OBJS = $(NS_ZLIB_SRCS:$(NS_ZLIB_DIR)/%.c=$(NS_ZLIB_OBJDIR)/%.o)
NS_COMPRESS_OBJS = $(NS_BINDIR)/lib/compress.o $(NS_ZLIB_OBJS)
NS_STORAGE_OBJS = $(NS_BINDIR)/lib/storage.db.o $(NS_BINDIR)/lib/storage.cache.o

ifeq ($(NS_OS), $(NS_LINUX))
	NS_OS_OBJS += $(NS_BINDIR)/lib/os.linux.o
	NS_TERM_OBJS = $(NS_BINDIR)/lib/term.posix.o
	NS_VIEW_OBJS += $(NS_BINDIR)/lib/view.linux.o $(NS_BINDIR)/lib/xdg-shell-protocol.o
	NS_GPU_OBJS += $(NS_BINDIR)/lib/gpu.vulkan.o
	NS_STORAGE_OBJS += $(NS_BINDIR)/lib/storage.json.o
else ifeq ($(NS_OS), $(NS_DARWIN))
	NS_OS_OBJS += $(NS_BINDIR)/lib/os.osx.o $(NS_BINDIR)/lib/os.haptic.apple.o
	NS_TERM_OBJS = $(NS_BINDIR)/lib/term.posix.o
	NS_VIEW_OBJS += $(NS_BINDIR)/lib/view.osx.o $(NS_BINDIR)/lib/view.gamepad.apple.o
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

# The adapter itself sees the zlib headers, and must be built with the same
# Z_PREFIX renaming as the zlib objects it links against.
$(NS_BINDIR)/lib/compress.o: lib/src/compress.c lib/include/compress.h $(NS_ZLIB_HEADERS) | $(NS_DIRS)
	$(Q)$(call ns_step,CC,$<,quiet) $(NS_CC) -c -fPIC -g $< -o $@ $(NS_LIB_INC) $(NS_ZLIB_INC) $(NS_ZLIB_DEF) $(NS_CFLAGS)

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
	$(NS_COMPRESS_DYLIB) \
	$(NS_STORAGE_DYLIB)

NS_STD_STATIC = \
	$(NS_IO_STATIC) \
	$(NS_OS_STATIC) \
	$(NS_NET_STATIC) \
	$(NS_HTTP_STATIC) \
	$(NS_WASM_DEV_STATIC) \
	$(NS_TERM_STATIC) \
	$(NS_GPU_STATIC) \
	$(NS_VIEW_STATIC) \
	$(NS_UI_STATIC) \
	$(NS_COMPRESS_STATIC) \
	$(NS_STORAGE_STATIC)

ifeq ($(NS_OS), $(NS_DARWIN))
	NS_STD_SHARED += $(NS_AUDIO_DYLIB)
	NS_STD_STATIC += $(NS_AUDIO_STATIC)
else
	# Other hosts keep the module resolvable with a silent stub, so a program
	# written for Apple audio still links and runs.
	NS_AUDIO_OBJS = $(NS_BINDIR)/lib/audio.stub.o
	NS_STD_SHARED += $(NS_AUDIO_DYLIB)
	NS_STD_STATIC += $(NS_AUDIO_STATIC)
endif

std: $(NS_STD_SHARED) $(NS_STD_STATIC)

shared: $(NS_STD_SHARED)

.PHONY: static
static: $(NS_STD_STATIC)

.PHONY: zlib compress_deps
zlib:
	$(NS_ZLIB_SCRIPT)

compress_deps: zlib

$(NS_ZLIB_OBJDIR):
	$(Q)$(NS_MKDIR) $@

$(NS_ZLIB_OBJDIR)/%.o: $(NS_ZLIB_DIR)/%.c $(NS_ZLIB_HEADERS) | $(NS_ZLIB_OBJDIR)
	$(Q)$(call ns_step,CC,$<,hide) $(NS_CC) -c -fPIC -g -O2 -fvisibility=hidden $< -o $@ $(NS_ZLIB_INC) $(NS_ZLIB_DEF) $(NS_PLATFORM_DEF) -Wno-error

$(NS_IO_DYLIB): $(NS_IO_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,LINK,$@,show) $(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_DYLIB_DEPS)

$(NS_OS_DYLIB): $(NS_OS_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,LINK,$@,show) $(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_OS_DYLIB_DEPS)

$(NS_NET_DYLIB): $(NS_NET_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,LINK,$@,show) $(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_DYLIB_DEPS)

$(NS_HTTP_DYLIB): $(NS_HTTP_OBJS) $(NS_NET_DYLIB) | $(NS_BINDIR)
	$(Q)$(call ns_step,LINK,$@,show) $(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_HTTP_OBJS) $(NS_NET_DYLIB) -o $@ $(NS_DYLIB_DEPS)

$(NS_WASM_DEV_DYLIB): $(NS_WASM_DEV_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,LINK,$@,show) $(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_DYLIB_DEPS)

$(NS_TERM_DYLIB): $(NS_TERM_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,LINK,$@,show) $(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_DYLIB_DEPS)

$(NS_GPU_DYLIB): $(NS_GPU_OBJS) $(if $(filter $(NS_WIN),$(NS_OS)),$(NS_VIEW_DYLIB),) | $(NS_BINDIR)
	$(Q)$(call ns_step,LINK,$@,show) $(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_GPU_OBJS) $(if $(filter $(NS_WIN),$(NS_OS)),$(NS_VIEW_DYLIB),) -o $@ $(NS_GPU_DYLIB_DEPS)

$(NS_VIEW_DYLIB): $(NS_VIEW_OBJS) $(if $(filter $(NS_DARWIN),$(NS_OS)),$(NS_GPU_DYLIB),) | $(NS_BINDIR)
	$(Q)$(call ns_step,LINK,$@,show) $(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_VIEW_OBJS) $(if $(filter $(NS_DARWIN),$(NS_OS)),$(NS_GPU_DYLIB),) -o $@ $(NS_VIEW_DYLIB_DEPS)

$(NS_UI_DYLIB): $(NS_UI_OBJS) $(NS_IO_DYLIB) $(NS_GPU_DYLIB) $(NS_VIEW_DYLIB) | $(NS_BINDIR)
	$(Q)$(call ns_step,LINK,$@,show) $(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_UI_OBJS) $(NS_IO_DYLIB) $(NS_GPU_DYLIB) $(NS_VIEW_DYLIB) -o $@ $(NS_UI_DYLIB_DEPS)

$(NS_AUDIO_DYLIB): $(NS_AUDIO_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,LINK,$@,show) $(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_AUDIO_DYLIB_DEPS)

$(NS_STORAGE_DYLIB): $(NS_STORAGE_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,LINK,$@,show) $(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_STORAGE_DYLIB_DEPS)

$(NS_IO_STATIC): $(NS_IO_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,AR,$@,hide) $(NS_AR) rcs $@ $^
$(NS_OS_STATIC): $(NS_OS_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,AR,$@,hide) $(NS_AR) rcs $@ $^
$(NS_NET_STATIC): $(NS_NET_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,AR,$@,hide) $(NS_AR) rcs $@ $^
$(NS_HTTP_STATIC): $(NS_HTTP_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,AR,$@,hide) $(NS_AR) rcs $@ $^
$(NS_WASM_DEV_STATIC): $(NS_WASM_DEV_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,AR,$@,hide) $(NS_AR) rcs $@ $^
$(NS_TERM_STATIC): $(NS_TERM_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,AR,$@,hide) $(NS_AR) rcs $@ $^
$(NS_GPU_STATIC): $(NS_GPU_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,AR,$@,hide) $(NS_AR) rcs $@ $^
$(NS_VIEW_STATIC): $(NS_VIEW_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,AR,$@,hide) $(NS_AR) rcs $@ $^
$(NS_UI_STATIC): $(NS_UI_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,AR,$@,hide) $(NS_AR) rcs $@ $^
$(NS_AUDIO_STATIC): $(NS_AUDIO_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,AR,$@,hide) $(NS_AR) rcs $@ $^
$(NS_STORAGE_STATIC): $(NS_STORAGE_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,AR,$@,hide) $(NS_AR) rcs $@ $^

# The source wildcards above are expanded before the download runs, so the first
# build downloads the sources and then re-enters make to pick them up.
ifeq ($(NS_COMPRESS_PRESENT),)
$(NS_COMPRESS_DYLIB): compress_deps | $(NS_BINDIR)
	$(MAKE) $@
else
$(NS_COMPRESS_DYLIB): $(NS_COMPRESS_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,LINK,$@,show) $(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_COMPRESS_OBJS) -o $@ $(NS_DYLIB_DEPS)
endif

ifeq ($(NS_COMPRESS_PRESENT),)
$(NS_COMPRESS_STATIC): compress_deps | $(NS_BINDIR)
	$(MAKE) $@
else
$(NS_COMPRESS_STATIC): $(NS_COMPRESS_OBJS) | $(NS_BINDIR)
	$(Q)$(call ns_step,AR,$@,hide) $(NS_AR) rcs $@ $^
endif

# Camera remains a feature library; never link AVFoundation into the interpreter.
ifeq ($(NS_OS), $(NS_DARWIN))
NS_STD_SHARED += $(NS_BINDIR)/camera$(NS_DYLIB_SUFFIX)
NS_STD_STATIC += $(NS_BINDIR)/libcamera.a
std: $(NS_BINDIR)/camera$(NS_DYLIB_SUFFIX) $(NS_BINDIR)/libcamera.a
shared: $(NS_BINDIR)/camera$(NS_DYLIB_SUFFIX)
static: $(NS_BINDIR)/libcamera.a
$(NS_BINDIR)/lib/camera.apple.o: lib/src/camera.apple.m lib/include/camera.h | $(NS_DIRS)
	$(Q)$(call ns_step,CC,$<,quiet) $(NS_CC) -c -fPIC -fobjc-arc -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)
$(NS_BINDIR)/camera$(NS_DYLIB_SUFFIX): $(NS_BINDIR)/lib/camera.apple.o
	$(Q)$(call ns_step,LINK,$@,show) $(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework Foundation
$(NS_BINDIR)/libcamera.a: $(NS_BINDIR)/lib/camera.apple.o
	$(Q)$(call ns_step,AR,$@,hide) $(NS_AR) rcs $@ $^
endif

# TLS and credentials remain an FFI feature, never part of the interpreter.
NS_SECURE_DYLIB = $(NS_BINDIR)/secure$(NS_DYLIB_SUFFIX)
NS_SECURE_STATIC = $(NS_BINDIR)/libsecure.a
NS_STD_SHARED += $(NS_SECURE_DYLIB)
NS_STD_STATIC += $(NS_SECURE_STATIC)
ifeq ($(NS_OS), $(NS_DARWIN))
NS_SECURE_DEPS = -framework Security -framework CoreFoundation
else ifeq ($(NS_OS), $(NS_WIN))
NS_SECURE_DEPS = -lssl -lcrypto -lws2_32 -ladvapi32
else
NS_SECURE_DEPS = -lssl -lcrypto -ldl
endif
$(NS_SECURE_DYLIB): $(NS_BINDIR)/lib/secure.o | $(NS_BINDIR)
	$(Q)$(call ns_step,LINK,$@,show) $(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_SECURE_DEPS)
$(NS_SECURE_STATIC): $(NS_BINDIR)/lib/secure.o | $(NS_BINDIR)
	$(Q)$(call ns_step,AR,$@,hide) ar rcs $@ $^

std: $(NS_SECURE_DYLIB) $(NS_SECURE_STATIC)
shared: $(NS_SECURE_DYLIB)
