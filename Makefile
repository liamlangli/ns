# FLAGS
MAKEFLAGS += --no-print-directory -j
.DEFAULT_GOAL := all

# PLATFORNM
OS := $(shell uname -s 2>/dev/null || echo Windows)
NS_PLATFORM_DEF =
NS_SUFFIX =
NS_DYLIB_SUFFIX =
NS_LIB_SUFFIX = 
NS_OS = 
NS_LIB = bin/libns.a
NS_INC = -I/usr/include -Iinclude -Iinclude/asm -Iinclude/os

NS_DARWIN = darwin
NS_LINUX = linux
NS_WIN = windows

NS_CC = clang
NS_LD = clang -fuse-ld=lld
NS_AR = ar
NS_MKDIR = mkdir -p
NS_RMDIR = rm -rf
NS_CP = cp -r
NS_HOME = $(HOME)
NS_INSTALL_ROOT = $(NS_HOME)/ns
NS_INSTALL_DISPLAY = ~/ns

ifeq ($(OS), Linux)
	NS_DYLIB_SUFFIX = .so
	NS_LIB_SUFFIX = .a
	NS_PLATFORM_DEF = -DNS_LINUX
	NS_OS =	$(NS_LINUX)
	NS_CC = gcc
	NS_LD = gcc
else ifeq ($(OS), Darwin)
	NS_DYLIB_SUFFIX = .dylib
	NS_LIB_SUFFIX = .a
	NS_PLATFORM_DEF = -DNS_DARWIN
	NS_OS = $(NS_DARWIN)
	NS_LD = clang
else
	NS_DYLIB_SUFFIX = .so
	NS_LIB_SUFFIX = .a
	NS_SUFFIX = .exe
	NS_PLATFORM_DEF = -DNS_WIN
	NS_OS = $(NS_WIN)
	NS_HOME = $(USERPROFILE)
endif
NS_INC += $(NS_PLATFORM_DEF)
NS_HEADERS = $(wildcard include/*.h include/asm/*.h include/os/*.h)

# OPTIONS
NS_DEBUG ?= 1
NS_WERROR ?= 1

# Extra include path for the Metal backend's external mapper headers
# (foundation/*, gpu/*, metal.h). Override on Apple if they live elsewhere.
NS_GPU_INC ?=

NS_WERROR_CFLAGS =
ifeq ($(NS_WERROR), 1)
	NS_WERROR_CFLAGS += -Werror
endif

NS_WARN_CFLAGS = -Wall -Wextra -Wunused-result $(NS_WERROR_CFLAGS)

ifeq ($(NS_OS), $(NS_WIN))
NS_LDFLAGS = -LD:/msys64/ucrt64/lib -lmsvcrt -lm -lreadline -lffi -ldl -lws2_32
NS_GPU_LDFLAGS = -ld3d12 -ldxgi -ldxguid -ld3dcompiler
else ifeq ($(NS_OS), $(NS_DARWIN))
NS_LDFLAGS = -L/usr/lib -lm -lreadline -lffi -ldl
NS_COCOA_LDFLAGS = -framework Cocoa
NS_METAL_LDFLAGS = -framework Metal -framework MetalKit -framework QuartzCore
else
# -rdynamic exports bin/ns symbols to FFI-loaded modules (bin/*.so reference
# runtime helpers like _ns_malloc; macOS resolves these against the host
# executable automatically, Linux only with an exported dynamic symbol table).
# -pthread: the task runtime (src/ns_task.c) runs tasks on worker threads.
NS_LDFLAGS = -L/usr/lib -lm -lreadline -lffi -ldl -rdynamic -pthread
# Headers and linker stubs from `make deps` (no sudo) or a conda env. System
# /usr packages still win when libreadline-dev, libffi-dev, and libsqlite3-dev
# are installed.
NS_LINUX_DEPS ?= $(HOME)/.local/ns-deps
NS_LINUX_MULTIARCH := $(shell gcc -print-multiarch 2>/dev/null)
ifeq ($(wildcard /usr/include/readline/readline.h),)
ifneq ($(wildcard $(NS_LINUX_DEPS)/usr/include/readline/readline.h),)
	NS_INC += -I$(NS_LINUX_DEPS)/usr/include
ifneq ($(NS_LINUX_MULTIARCH),)
	NS_INC += -I$(NS_LINUX_DEPS)/usr/include/$(NS_LINUX_MULTIARCH)
	NS_LDFLAGS := -L$(NS_LINUX_DEPS)/usr/lib/$(NS_LINUX_MULTIARCH) $(NS_LDFLAGS)
endif
else ifneq ($(CONDA_PREFIX),)
ifneq ($(wildcard $(CONDA_PREFIX)/include/readline/readline.h),)
	NS_INC += -I$(CONDA_PREFIX)/include
	NS_LDFLAGS := -L$(CONDA_PREFIX)/lib -Wl,-rpath,$(CONDA_PREFIX)/lib $(NS_LDFLAGS)
endif
endif
endif
endif

NS_DEBUG_CFLAGS = -g -Og $(NS_WARN_CFLAGS) -DNS_DEBUG
NS_RELEASE_CFLAGS = -Os $(NS_WARN_CFLAGS)

ifeq ($(NS_DEBUG), 1)
	NS_CFLAGS = $(NS_DEBUG_CFLAGS)
else
	NS_CFLAGS = $(NS_RELEASE_CFLAGS)
endif
# The GPU backend is forced per platform in lib/include/gpu.h (Metal on Apple,
# DX12 on Windows); the Metal source needs its external mapper headers on the
# include path.
NS_CFLAGS += $(NS_GPU_INC)

NS_BINDIR = bin
NS_AGENTS_HEADER = $(NS_BINDIR)/ns_agents_md.h
NS_NATIVE_RT_OBJ = $(NS_BINDIR)/ns_native_rt.o
NS_INC += -I$(NS_BINDIR)

NS_LIB_SRCS = src/ns_fmt.c \
	src/ns_type.c \
	src/ns_profile.c \
	src/ns_profile_live.c \
	src/ns_os.c \
	src/ns_token.c \
	src/ns_ast.c \
	src/ns_ast_stmt.c \
	src/ns_ast_expr.c \
	src/ns_ast_print.c \
	src/ns_ssa.c \
	src/ns_native_rt.c \
	src/ns_aarch.c \
	src/ns_macho.c \
	src/ns_wasm.c \
	src/ns_amd64.c \
	src/ns_pe.c \
	src/ns_vm_parse.c \
	src/ns_vm_eval.c \
	src/ns_task.c \
	src/ns_vm_lib.c \
	src/ns_vm_print.c \
	src/ns_net.c \
	src/ns_json.c \
	src/ns_shader.c \
	src/ns_lint.c \
	src/ns_project.c \
	src/ns_build_cache.c \
	src/ns_project_xcode.c \
	src/ns_project_vs.c \
	src/ns_repl.c \
	src/ns_def.c \
	src/ns_asm.c

# Language-only runtime copied into generated Apple IDE projects. Keep native
# UI, terminal, view, GPU, network/HTTP modules, the REPL, and object emitters
# out of this list.
NS_EMBED_RUNTIME_SRCS = src/ns_fmt.c \
	src/ns_type.c \
	src/ns_profile.c \
	src/ns_os.c \
	src/ns_token.c \
	src/ns_ast.c \
	src/ns_ast_stmt.c \
	src/ns_ast_expr.c \
	src/ns_ast_print.c \
	src/ns_vm_parse.c \
	src/ns_vm_eval.c \
	src/ns_task.c \
	src/ns_vm_lib.c \
	src/ns_embedded_ffi.c \
	src/ns_vm_print.c \
	src/ns_json.c \
	src/ns_shader.c \
	src/ns_native_rt.c \
	src/ns_def.c

# The generator is Nano Script (tools/gen_embedded_ffi.ns); the embedded
# module and native-source lists live in its main(). It needs the interpreter
# plus the os/term feature dylibs it reads and writes files through.
.PHONY: regen-embedded-ffi
regen-embedded-ffi: $(TARGET) $(NS_BINDIR)/os$(NS_DYLIB_SUFFIX) $(NS_BINDIR)/term$(NS_DYLIB_SUFFIX)
	$(TARGET)$(NS_SUFFIX) run tools/gen_embedded_ffi.ns

# iOS subset: exclude the REPL (readline is unavailable). NS_XCLIB keeps the
# VM's libffi/dlopen path out and selects its generated, statically bound FFI
# table instead; the native runtime is included for AOT archives.
NS_IOS_LIB_SRCS = src/ns_fmt.c \
	src/ns_type.c \
	src/ns_profile.c \
	src/ns_os.c \
	src/ns_token.c \
	src/ns_ast.c \
	src/ns_ast_stmt.c \
	src/ns_ast_expr.c \
	src/ns_ast_print.c \
	src/ns_ssa.c \
	src/ns_aarch.c \
	src/ns_macho.c \
	src/ns_vm_parse.c \
	src/ns_vm_eval.c \
	src/ns_task.c \
	src/ns_vm_lib.c \
	src/ns_embedded_ffi.c \
	src/ns_vm_print.c \
	src/ns_net.c \
	src/ns_json.c \
	src/ns_shader.c \
	src/ns_native_rt.c \
	src/ns_def.c \
	src/ns_asm.c

NS_LIB_OBJS = $(NS_LIB_SRCS:%.c=$(NS_BINDIR)/%.o)

# Native feature modules (lib/*) are compiled position-independent and built as
# dylibs/so files. Keep them out of bin/ns so the interpreter remains
# language-only; ref fn calls resolve them through dlopen()/dlsym().
NS_LIBFN_SRCS = lib/src/io.c lib/src/gpu.c lib/src/view.c lib/src/os.c lib/src/net.c lib/src/http.c lib/src/wasm_dev.c lib/src/ui.c lib/src/storage.db.c lib/src/storage.cache.c
ifeq ($(NS_OS), $(NS_LINUX))
	NS_LIBFN_SRCS += lib/src/view.linux.c lib/src/os.linux.c lib/src/term.posix.c lib/src/storage.json.c
else ifeq ($(NS_OS), $(NS_DARWIN))
	# Apple: force the Metal backend.
	NS_LIBFN_SRCS += lib/src/view.osx.m lib/src/os.osx.m lib/src/os.haptic.apple.m lib/src/term.posix.c lib/src/gpu.metal.m lib/src/audio.apple.m lib/src/storage.apple.m
else ifeq ($(NS_OS), $(NS_WIN))
	# Windows: force the DirectX 12 backend.
	NS_LIBFN_SRCS += lib/src/view.win.c lib/src/os.win.c lib/src/term.win.c lib/src/gpu.dx12.c lib/src/storage.json.c
endif
NS_LIBFN_OBJS = $(NS_LIBFN_SRCS:lib/src/%=$(NS_BINDIR)/lib/%)
NS_LIBFN_OBJS := $(NS_LIBFN_OBJS:.c=.o)
NS_LIBFN_OBJS := $(NS_LIBFN_OBJS:.m=.o)

NS_TEST_SRCS = test/ns_json_test.c test/ns_expr_test.c test/ns_compile_test.c test/ns_shader_test.c test/ns_ssa_test.c test/ns_token_test.c test/ns_buffer_test.c test/ns_os_test.c test/ns_project_test.c test/ns_build_cache_test.c test/ns_lint_test.c test/ns_profile_test.c
NS_TEST_TARGETS = $(NS_TEST_SRCS:test/%.c=$(NS_BINDIR)/%)

NS_ENTRY = src/ns.c 
NS_ENTRY_OBJ = $(NS_BINDIR)/src/ns.o

TARGET = $(NS_BINDIR)/ns

NS_SRCS = $(NS_LIB_SRCS) $(NS_ENTRY)
NS_DIRS = bin bin/src bin/lib

ifeq ($(NS_OS), $(NS_DARWIN))
all: $(NS_DIRS) $(TARGET) $(NS_LIB) std profiler
else
all: $(NS_DIRS) $(TARGET) $(NS_LIB) std
endif

.PHONY: profiler
profiler: $(TARGET) std
	$(CURDIR)/$(TARGET)$(NS_SUFFIX) build $(CURDIR)/nscode/profile
	$(NS_RMDIR) $(NS_BINDIR)/nscode-profile.app
	$(NS_CP) nscode/profile/bin/nscode-profile.app $(NS_BINDIR)/nscode-profile.app

$(NS_DIRS):
	$(NS_MKDIR) $(NS_DIRS)

$(NS_AGENTS_HEADER): AGENTS.md | $(NS_DIRS)
	{ \
		printf '%s\n' 'static const char ns_scaffold_agents_markdown[] ='; \
		sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n"/' $<; \
		printf '%s\n' ';'; \
	} > $@

$(NS_ENTRY_OBJ): $(NS_ENTRY) $(NS_HEADERS) $(NS_AGENTS_HEADER) | $(NS_DIRS)
	$(NS_CC) -c $< -o $@ $(NS_INC) $(NS_CFLAGS)

$(TARGET): $(NS_LIB_OBJS) $(NS_ENTRY_OBJ) $(NS_NATIVE_RT_OBJ) | $(NS_BINDIR)
	$(NS_LD) $(NS_LIB_OBJS) $(NS_ENTRY_OBJ) -o $(TARGET)$(NS_SUFFIX) $(NS_LDFLAGS)

$(NS_NATIVE_RT_OBJ): src/ns_native_rt.c include/ns_native_rt.h | $(NS_BINDIR)
	$(NS_CC) -c $< -o $@ $(NS_INC) $(NS_RELEASE_CFLAGS)

$(NS_LIB_OBJS): $(NS_BINDIR)/%.o : %.c $(NS_HEADERS) | $(NS_DIRS)
	$(NS_CC) -c $< -o $@ $(NS_INC) $(NS_CFLAGS)

run: all
	$(TARGET)

clean:
	$(NS_RMDIR) $(NS_BINDIR)

# utility
count:
	cloc src include sample

# pack source files
pack:
	git ls-files -z | tar --null -T - -czvf bin/ns.tar.gz

$(NS_LIB): $(NS_LIB_OBJS)
	ar rcs $(NS_BINDIR)/libns$(NS_LIB_SUFFIX) $(NS_LIB_OBJS)

so: $(NS_LIB_OBJS)
	$(NS_CC) -shared $(NS_LIB_OBJS) -o $(NS_BINDIR)/ns$(NS_DYLIB_SUFFIX) $(NS_LDFLAGS)

$(NS_TEST_TARGETS): $(NS_BINDIR)/%: test/%.c $(NS_HEADERS) $(NS_LIB)
# libns.a must precede $(NS_LDFLAGS): with ld's default --as-needed, shared
# libs listed before the archive that references them (ffi, readline) are
# dropped and the link fails with undefined references.
	$(NS_CC) -o $@ $< $(NS_INC) $(NS_CFLAGS) -Itest -L$(NS_BINDIR) -lns $(NS_LDFLAGS)

.PHONY: test
# CI runs `make test` without `make all`. Depend on `std` so every native
# module the .ns suites load (io, dynamic, os, gpu, ...) is built first.
test: $(NS_TEST_TARGETS) $(TARGET) std
	$(NS_BINDIR)/ns_json_test
	$(NS_BINDIR)/ns_expr_test
	$(NS_BINDIR)/ns_compile_test
	$(NS_BINDIR)/ns_shader_test
	$(NS_BINDIR)/ns_ssa_test
	$(NS_BINDIR)/ns_token_test
	$(NS_BINDIR)/ns_buffer_test
	$(NS_BINDIR)/ns_os_test
	$(NS_BINDIR)/ns_project_test
	$(NS_BINDIR)/ns_build_cache_test
	$(NS_BINDIR)/ns_lint_test
	$(NS_BINDIR)/ns_profile_test
	sh test/ns_project_cli_test.sh "$(CURDIR)/$(TARGET)$(NS_SUFFIX)"
	sh test/ns_update_test.sh "$(CURDIR)/$(TARGET)$(NS_SUFFIX)"
	sh test/ns_lint_test.sh "$(CURDIR)/$(TARGET)$(NS_SUFFIX)"
	sh test/ns_run_test.sh "$(CURDIR)/$(TARGET)$(NS_SUFFIX)"
	sh test/ns_build_test.sh "$(CURDIR)/$(TARGET)$(NS_SUFFIX)"
	sh test/ns_parity_test.sh "$(CURDIR)/$(TARGET)$(NS_SUFFIX)"
	sh test/ns_scope_test.sh "$(CURDIR)/$(TARGET)$(NS_SUFFIX)"
	sh test/ns_profile_test.sh "$(CURDIR)/$(TARGET)$(NS_SUFFIX)"
	sh test/ns_wasm_project_test.sh "$(CURDIR)/$(TARGET)$(NS_SUFFIX)"
	sh test/storage_apple_compile.sh
	node test/ns_wasm_runtime_test.mjs
	node test/ns_wasm_ui_test.mjs "$(CURDIR)/$(TARGET)$(NS_SUFFIX)"

include lib/Makefile
include sample/c/Makefile

.PHONY: deps
deps:
ifeq ($(NS_OS), $(NS_LINUX))
	sh scripts/install_linux_deps.sh
else
	@echo "make deps is only needed on Linux/WSL."
endif

install: all
	$(NS_MKDIR) $(NS_INSTALL_ROOT)/bin $(NS_INSTALL_ROOT)/lib $(NS_INSTALL_ROOT)/ref \
		$(NS_INSTALL_ROOT)/share/ns-runtime/src $(NS_INSTALL_ROOT)/share/ns-runtime/include \
		$(NS_INSTALL_ROOT)/share/ns-runtime/ref $(NS_INSTALL_ROOT)/share/ns-runtime/feature/src \
		$(NS_INSTALL_ROOT)/share/ns-runtime/feature/include $(NS_INSTALL_ROOT)/share/ns-runtime/feature/assets \
		$(NS_INSTALL_ROOT)/share/ns-runtime/feature/src/zstd/common \
		$(NS_INSTALL_ROOT)/share/ns-runtime/feature/src/zstd/compress \
		$(NS_INSTALL_ROOT)/share/ns-runtime/feature/src/zstd/decompress \
		$(NS_INSTALL_ROOT)/share/ns-runtime/feature/include/zstd/common \
		$(NS_INSTALL_ROOT)/share/ns-runtime/feature/include/zstd/compress \
		$(NS_INSTALL_ROOT)/share/ns-runtime/feature/include/zstd/decompress \
		$(NS_INSTALL_ROOT)/share/licenses/box3d \
		$(NS_INSTALL_ROOT)/share/licenses/zlib \
		$(NS_INSTALL_ROOT)/share/licenses/zstd \
		$(NS_INSTALL_ROOT)/share/nscode/profile
	cp $(TARGET)$(NS_SUFFIX) $(NS_INSTALL_ROOT)/bin/ns$(NS_SUFFIX).new
	mv -f $(NS_INSTALL_ROOT)/bin/ns$(NS_SUFFIX).new $(NS_INSTALL_ROOT)/bin/ns$(NS_SUFFIX)
	$(NS_CP) lib/*.ns $(NS_INSTALL_ROOT)/ref
	cp lib/ns-wasm.js $(NS_INSTALL_ROOT)/ref/ns-wasm.js
	cp sample/ns.svg $(NS_INSTALL_ROOT)/ref/ns.svg
	$(NS_CP) lib/assets $(NS_INSTALL_ROOT)/ref
	cp $(NS_EMBED_RUNTIME_SRCS) $(NS_INSTALL_ROOT)/share/ns-runtime/src/
	cp $(NS_NATIVE_RT_OBJ) $(NS_INSTALL_ROOT)/lib/ns_native_rt.o
	$(NS_CP) include/. $(NS_INSTALL_ROOT)/share/ns-runtime/include/
	cp lib/std.ns lib/shader.ns lib/simd.ns lib/task.ns lib/view.ns lib/ui.ns lib/os.ns lib/gpu.ns lib/io.ns \
		lib/net.ns lib/dynamic.ns lib/compress.ns lib/storage.ns lib/audio.ns \
		$(NS_INSTALL_ROOT)/share/ns-runtime/ref/
	cp lib/src/io.c lib/src/net.c lib/src/os.c lib/src/os.osx.m lib/src/os.ios.m lib/src/os.haptic.apple.m \
		lib/src/view.c lib/src/view.osx.m lib/src/view.ios.m lib/src/gpu.c lib/src/gpu.metal.m \
		lib/src/ui.c lib/src/storage.db.c lib/src/storage.cache.c lib/src/storage.apple.m lib/src/compress.c \
		lib/src/audio.apple.m \
		$(NS_INSTALL_ROOT)/share/ns-runtime/feature/src/
	cp lib/include/net.h lib/include/os.h lib/include/view.h lib/include/gpu.h lib/include/gpu_const.h \
		lib/include/storage.h lib/include/storage.internal.h lib/include/compress.h lib/include/audio.h \
		lib/include/stb_image.h lib/include/stb_image_resize2.h lib/include/stb_image_write.h \
		$(NS_INSTALL_ROOT)/share/ns-runtime/feature/include/
	cp third_party/zstd/lib/zstd.h third_party/zstd/lib/zstd_errors.h \
		$(NS_INSTALL_ROOT)/share/ns-runtime/feature/include/zstd/
	cp third_party/zstd/lib/common/*.h $(NS_INSTALL_ROOT)/share/ns-runtime/feature/include/zstd/common/
	cp third_party/zstd/lib/compress/*.h $(NS_INSTALL_ROOT)/share/ns-runtime/feature/include/zstd/compress/
	cp third_party/zstd/lib/decompress/*.h $(NS_INSTALL_ROOT)/share/ns-runtime/feature/include/zstd/decompress/
	cp third_party/zstd/lib/common/*.c $(NS_INSTALL_ROOT)/share/ns-runtime/feature/src/zstd/common/
	cp third_party/zstd/lib/compress/*.c $(NS_INSTALL_ROOT)/share/ns-runtime/feature/src/zstd/compress/
	cp third_party/zstd/lib/decompress/*.c $(NS_INSTALL_ROOT)/share/ns-runtime/feature/src/zstd/decompress/
	cp lib/assets/latin_mono.json lib/assets/latin_mono.webp lib/assets/latin_mono.png \
		lib/assets/bitmap_font.json lib/assets/bitmap_font.png \
		lib/assets/bitmap_zh_cn.json lib/assets/bitmap_zh_cn.png \
		$(NS_INSTALL_ROOT)/share/ns-runtime/feature/assets/
	cp third_party/box3d/LICENSE $(NS_INSTALL_ROOT)/share/licenses/box3d/LICENSE
	cp third_party/zlib/LICENSE $(NS_INSTALL_ROOT)/share/licenses/zlib/LICENSE
	cp third_party/zstd/LICENSE $(NS_INSTALL_ROOT)/share/licenses/zstd/LICENSE
	cp nscode/profile/ns.mod nscode/profile/main.ns nscode/profile/live.ns $(NS_INSTALL_ROOT)/share/nscode/profile/
	if [ -d nscode/profile/bin/nscode-profile.app ]; then \
		$(NS_RMDIR) $(NS_INSTALL_ROOT)/share/nscode/profile/nscode-profile.app; \
		$(NS_CP) nscode/profile/bin/nscode-profile.app $(NS_INSTALL_ROOT)/share/nscode/profile/nscode-profile.app; \
		$(NS_RMDIR) $(NS_INSTALL_ROOT)/bin/nscode-profile.app; \
		$(NS_CP) nscode/profile/bin/nscode-profile.app $(NS_INSTALL_ROOT)/bin/nscode-profile.app; \
	fi
	find $(NS_BINDIR) -maxdepth 1 -type f \( -name '*.a' -o -name '*.so' -o -name '*.dylib' -o -name '*.dll' \) -exec sh -c '\
		for ns_lib_file do ns_lib_name=$$(basename "$$ns_lib_file"); \
			cp "$$ns_lib_file" "$(NS_INSTALL_ROOT)/lib/$$ns_lib_name.new"; \
			mv -f "$(NS_INSTALL_ROOT)/lib/$$ns_lib_name.new" "$(NS_INSTALL_ROOT)/lib/$$ns_lib_name"; \
		done' sh {} +
	@echo "Installed ns to $(NS_INSTALL_DISPLAY)"
	@echo "Please add $(NS_INSTALL_DISPLAY)/bin to your system PATH."
	@case "$${SHELL##*/}" in \
		zsh) ns_shell_rc="~/.zshrc" ;; \
		bash) ns_shell_rc="~/.bashrc" ;; \
		*) ns_shell_rc="~/.profile" ;; \
	esac; \
	printf 'Run this to append it: `echo '\''export PATH="$$HOME/ns/bin:$$PATH"'\'' >> %s`\n' "$$ns_shell_rc"

# ===== Apple (Darwin) XCFramework packing (macOS arm64 + iOS arm64) =====
# Unique target names to avoid clashes with other included makefiles.

ifeq ($(NS_OS), $(NS_DARWIN))

APPLE_CC        = $(shell xcrun -find clang)
APPLE_LIBTOOL   = $(shell xcrun -find libtool)
MACOS_SDK       = $(shell xcrun --sdk macosx --show-sdk-path)
IOS_SDK         = $(shell xcrun --sdk iphoneos --show-sdk-path)

MACOS_MIN_VER   ?= 12.0
IOS_MIN_VER     ?= 13.0

APPLE_OUTDIR    := $(NS_BINDIR)/apple
MACOS_OBJDIR    := $(APPLE_OUTDIR)/macos-arm64/obj
IOS_OBJDIR      := $(APPLE_OUTDIR)/ios-arm64/obj
MACOS_LIB       := $(APPLE_OUTDIR)/macos-arm64/libns.a
IOS_LIB         := $(APPLE_OUTDIR)/ios-arm64/libns.a

NS_XCFRAMEWORK  := $(NS_BINDIR)/ns.xcframework
NS_HEADERS_DIR  := include

MACOS_CFLAGS = -target arm64-apple-macos$(MACOS_MIN_VER) -isysroot $(MACOS_SDK) -fPIC -g -O0 -DNS_DEBUG -DNS_XCLIB
IOS_CFLAGS   = -target arm64-apple-ios$(IOS_MIN_VER)   -isysroot $(IOS_SDK)   -fembed-bitcode -fPIC -g -O0 -DNS_DEBUG -DNS_XCLIB

MACOS_OBJS := $(NS_LIB_SRCS:%.c=$(MACOS_OBJDIR)/%.o)
IOS_OBJS   := $(NS_IOS_LIB_SRCS:%.c=$(IOS_OBJDIR)/%.o)

# Native feature modules are separate from the language runtime. Build one
# archive per module for device linking; consumers select only the modules
# their Nano Script source imports and add the corresponding Apple frameworks.
IOS_FEATURE_OBJDIR := $(APPLE_OUTDIR)/ios-arm64/feature-obj
IOS_FEATURE_LIBDIR := $(APPLE_OUTDIR)/ios-arm64
IOS_FEATURE_INC := $(NS_INC) -Ilib/include -Ithird_party/box3d/include -Ithird_party/box3d/src \
	-Ithird_party/zlib -Ithird_party/zstd/lib
IOS_FEATURE_HEADERS := $(wildcard lib/include/*.h include/*.h include/asm/*.h include/os/*.h)
IOS_FEATURE_CFLAGS := -target arm64-apple-ios$(IOS_MIN_VER) -isysroot $(IOS_SDK) -fembed-bitcode -fPIC -g -O0 \
	-DNS_DEBUG -DNS_XCLIB -DNS_DARWIN

IOS_IO_OBJS := $(IOS_FEATURE_OBJDIR)/lib/src/io.o
IOS_OS_OBJS := $(IOS_FEATURE_OBJDIR)/lib/src/os.o $(IOS_FEATURE_OBJDIR)/lib/src/os.ios.o \
	$(IOS_FEATURE_OBJDIR)/lib/src/os.haptic.apple.o
IOS_NET_OBJS := $(IOS_FEATURE_OBJDIR)/lib/src/net.o
IOS_HTTP_OBJS := $(IOS_FEATURE_OBJDIR)/lib/src/http.o
IOS_WASM_DEV_OBJS := $(IOS_FEATURE_OBJDIR)/lib/src/wasm_dev.o
IOS_TERM_OBJS := $(IOS_FEATURE_OBJDIR)/lib/src/term.posix.o
IOS_VIEW_OBJS := $(IOS_FEATURE_OBJDIR)/lib/src/view.o $(IOS_FEATURE_OBJDIR)/lib/src/view.ios.o
IOS_GPU_OBJS := $(IOS_FEATURE_OBJDIR)/lib/src/gpu.o $(IOS_FEATURE_OBJDIR)/lib/src/gpu.metal.o
IOS_UI_OBJS := $(IOS_FEATURE_OBJDIR)/lib/src/ui.o
IOS_AUDIO_OBJS := $(IOS_FEATURE_OBJDIR)/lib/src/audio.apple.o
IOS_STORAGE_OBJS := $(IOS_FEATURE_OBJDIR)/lib/src/storage.db.o $(IOS_FEATURE_OBJDIR)/lib/src/storage.cache.o \
	$(IOS_FEATURE_OBJDIR)/lib/src/storage.apple.o
IOS_DYNAMIC_OBJS := $(patsubst third_party/box3d/src/%.c,$(IOS_FEATURE_OBJDIR)/third_party/box3d/src/%.o,$(NS_BOX3D_SRCS)) \
	$(IOS_FEATURE_OBJDIR)/lib/src/dynamic.o
IOS_ZLIB_OBJS := $(patsubst third_party/zlib/%.c,$(IOS_FEATURE_OBJDIR)/third_party/zlib/%.o,$(NS_ZLIB_SRCS))
IOS_ZSTD_OBJS := $(patsubst third_party/zstd/lib/%.c,$(IOS_FEATURE_OBJDIR)/third_party/zstd/lib/%.o,$(NS_ZSTD_SRCS))
IOS_COMPRESS_OBJS := $(IOS_FEATURE_OBJDIR)/lib/src/compress.o $(IOS_ZLIB_OBJS) $(IOS_ZSTD_OBJS)

IOS_FEATURE_LIBS := $(addprefix $(IOS_FEATURE_LIBDIR)/lib,io.a os.a net.a http.a wasm_dev.a term.a view.a gpu.a ui.a \
	audio.a storage.a dynamic.a compress.a)

.PHONY: ns_xcframework ns_apple_dirs ns_apple_clean macos_arm64 ios_arm64 ios_static xcframework apple-xcframework

# Public entrypoint
xc: ns_xcframework

ns_xcframework: macos_arm64 ios_arm64
	rm -rf $(NS_XCFRAMEWORK)
	xcodebuild -create-xcframework \
		-library $(MACOS_LIB) -headers $(NS_HEADERS_DIR) \
		-library $(IOS_LIB) -headers $(NS_HEADERS_DIR) \
		-output $(NS_XCFRAMEWORK)
	@echo "📦 Signing xcframework..."
	codesign --force --sign - --timestamp=none $(NS_XCFRAMEWORK)
	@echo "✅ Built and signed $(NS_XCFRAMEWORK)"

ns_apple_dirs:
	mkdir -p $(MACOS_OBJDIR)
	mkdir -p $(IOS_OBJDIR)
	mkdir -p $(APPLE_OUTDIR)/macos-arm64
	mkdir -p $(APPLE_OUTDIR)/ios-arm64

# macOS arm64 static lib
macos_arm64: $(MACOS_LIB)

$(MACOS_LIB): ns_apple_dirs $(MACOS_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $(MACOS_OBJS)
	codesign --force --sign - --timestamp=none $@
	@echo "📦 macOS arm64 static lib -> $@"

$(MACOS_OBJDIR)/%.o: %.c | ns_apple_dirs
	mkdir -p $(dir $@)
	$(APPLE_CC) -c $< -o $@ $(NS_INC) $(MACOS_CFLAGS)

# iOS arm64 static lib
ios_arm64: $(IOS_LIB)

ios_static: ios_arm64 $(IOS_FEATURE_LIBS)

$(IOS_LIB): ns_apple_dirs $(IOS_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $(IOS_OBJS)
	codesign --force --sign - --timestamp=none $@
	@echo "📦 iOS arm64 static lib -> $@"

$(IOS_OBJDIR)/%.o: %.c | ns_apple_dirs
	mkdir -p $(dir $@)
	$(APPLE_CC) -c $< -o $@ $(NS_INC) $(IOS_CFLAGS)

$(IOS_FEATURE_OBJDIR)/lib/src/%.o: lib/src/%.c $(IOS_FEATURE_HEADERS)
	mkdir -p $(dir $@)
	$(APPLE_CC) -c $< -o $@ $(IOS_FEATURE_INC) $(IOS_FEATURE_CFLAGS)

$(IOS_FEATURE_OBJDIR)/lib/src/%.o: lib/src/%.m $(IOS_FEATURE_HEADERS)
	mkdir -p $(dir $@)
	$(APPLE_CC) -c $< -o $@ $(IOS_FEATURE_INC) $(IOS_FEATURE_CFLAGS)

$(IOS_FEATURE_OBJDIR)/lib/src/compress.o: lib/src/compress.c $(IOS_FEATURE_HEADERS) $(NS_ZLIB_HEADERS) $(NS_ZSTD_HEADERS)
	mkdir -p $(dir $@)
	$(APPLE_CC) -c $< -o $@ $(IOS_FEATURE_INC) $(IOS_FEATURE_CFLAGS) $(NS_ZLIB_DEF) $(NS_ZSTD_DEF)

$(IOS_FEATURE_OBJDIR)/third_party/box3d/src/%.o: third_party/box3d/src/%.c $(NS_BOX3D_HEADERS)
	mkdir -p $(dir $@)
	$(APPLE_CC) -c $< -o $@ $(IOS_FEATURE_INC) $(IOS_FEATURE_CFLAGS) -std=gnu17

$(IOS_FEATURE_OBJDIR)/third_party/zlib/%.o: third_party/zlib/%.c $(NS_ZLIB_HEADERS)
	mkdir -p $(dir $@)
	$(APPLE_CC) -c $< -o $@ $(IOS_FEATURE_INC) $(IOS_FEATURE_CFLAGS) $(NS_ZLIB_DEF) -fvisibility=hidden

$(IOS_FEATURE_OBJDIR)/third_party/zstd/lib/%.o: third_party/zstd/lib/%.c $(NS_ZSTD_HEADERS)
	mkdir -p $(dir $@)
	$(APPLE_CC) -c $< -o $@ $(IOS_FEATURE_INC) $(IOS_FEATURE_CFLAGS) $(NS_ZSTD_DEF) -fvisibility=hidden

$(IOS_FEATURE_LIBDIR)/libio.a: $(IOS_IO_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $^
$(IOS_FEATURE_LIBDIR)/libos.a: $(IOS_OS_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $^
$(IOS_FEATURE_LIBDIR)/libnet.a: $(IOS_NET_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $^
$(IOS_FEATURE_LIBDIR)/libhttp.a: $(IOS_HTTP_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $^
$(IOS_FEATURE_LIBDIR)/libwasm_dev.a: $(IOS_WASM_DEV_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $^
$(IOS_FEATURE_LIBDIR)/libterm.a: $(IOS_TERM_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $^
$(IOS_FEATURE_LIBDIR)/libview.a: $(IOS_VIEW_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $^
$(IOS_FEATURE_LIBDIR)/libgpu.a: $(IOS_GPU_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $^
$(IOS_FEATURE_LIBDIR)/libui.a: $(IOS_UI_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $^
$(IOS_FEATURE_LIBDIR)/libaudio.a: $(IOS_AUDIO_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $^
$(IOS_FEATURE_LIBDIR)/libstorage.a: $(IOS_STORAGE_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $^
ifeq ($(NS_BOX3D_PRESENT),)
$(IOS_FEATURE_LIBDIR)/libdynamic.a: box3d
	$(MAKE) $@
else
$(IOS_FEATURE_LIBDIR)/libdynamic.a: $(IOS_DYNAMIC_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $^
endif
ifeq ($(NS_COMPRESS_PRESENT),)
$(IOS_FEATURE_LIBDIR)/libcompress.a: compress_deps
	$(MAKE) $@
else
$(IOS_FEATURE_LIBDIR)/libcompress.a: $(IOS_COMPRESS_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $^
endif

# Clean only Apple artifacts
ns_apple_clean:
	rm -rf $(APPLE_OUTDIR) $(NS_XCFRAMEWORK)

endif
# ===== end Apple (Darwin) XCFramework block =====
