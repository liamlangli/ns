# FLAGS
MAKEFLAGS += --no-print-directory -j

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
endif

NS_DEBUG_CFLAGS = -g -O0 $(NS_WARN_CFLAGS) -DNS_DEBUG
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

NS_LIB_SRCS = src/ns_fmt.c \
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
	src/ns_project.c \
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
	src/ns_vm_print.c \
	src/ns_json.c \
	src/ns_shader.c \
	src/ns_def.c

# iOS subset: exclude repl and vm_lib (depend on readline/libffi not available on iOS)
NS_IOS_LIB_SRCS = src/ns_fmt.c \
	src/ns_type.c \
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
	src/ns_vm_print.c \
	src/ns_net.c \
	src/ns_json.c \
	src/ns_shader.c \
	src/ns_def.c \
	src/ns_asm.c

NS_LIB_OBJS = $(NS_LIB_SRCS:%.c=$(NS_BINDIR)/%.o)

# Native feature modules (lib/*) are compiled position-independent and built as
# dylibs/so files. Keep them out of bin/ns so the interpreter remains
# language-only; ref fn calls resolve them through dlopen()/dlsym().
NS_LIBFN_SRCS = lib/src/io.c lib/src/gpu.c lib/src/view.c lib/src/os.c lib/src/net.c lib/src/http.c lib/src/ui.c
ifeq ($(NS_OS), $(NS_LINUX))
	NS_LIBFN_SRCS += lib/src/view.linux.c lib/src/os.linux.c lib/src/term.posix.c
else ifeq ($(NS_OS), $(NS_DARWIN))
	# Apple: force the Metal backend.
	NS_LIBFN_SRCS += lib/src/view.osx.m lib/src/os.osx.m lib/src/term.posix.c lib/src/gpu.metal.m
else ifeq ($(NS_OS), $(NS_WIN))
	# Windows: force the DirectX 12 backend.
	NS_LIBFN_SRCS += lib/src/view.win.c lib/src/os.win.c lib/src/term.win.c lib/src/gpu.dx12.c
endif
NS_LIBFN_OBJS = $(NS_LIBFN_SRCS:lib/src/%=$(NS_BINDIR)/lib/%)
NS_LIBFN_OBJS := $(NS_LIBFN_OBJS:.c=.o)
NS_LIBFN_OBJS := $(NS_LIBFN_OBJS:.m=.o)

NS_TEST_SRCS = test/ns_json_test.c test/ns_expr_test.c test/ns_shader_test.c test/ns_token_test.c test/ns_buffer_test.c test/ns_os_test.c test/ns_project_test.c
NS_TEST_TARGETS = $(NS_TEST_SRCS:test/%.c=$(NS_BINDIR)/%)

NS_ENTRY = src/ns.c 
NS_ENTRY_OBJ = $(NS_BINDIR)/src/ns.o

TARGET = $(NS_BINDIR)/ns

NS_SRCS = $(NS_LIB_SRCS) $(NS_ENTRY)
NS_DIRS = bin bin/src bin/lib

all: $(NS_DIRS) $(TARGET) $(NS_LIB) std

$(NS_DIRS):
	$(NS_MKDIR) $(NS_DIRS)

$(NS_ENTRY_OBJ): $(NS_ENTRY) $(NS_HEADERS) | $(NS_DIRS)
	$(NS_CC) -c $< -o $@ $(NS_INC) $(NS_CFLAGS)

$(TARGET): $(NS_LIB_OBJS) $(NS_ENTRY_OBJ) | $(NS_BINDIR)
	$(NS_LD) $(NS_LIB_OBJS) $(NS_ENTRY_OBJ) -o $(TARGET)$(NS_SUFFIX) $(NS_LDFLAGS)

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
test: $(NS_TEST_TARGETS) $(TARGET) $(NS_BINDIR)/os$(NS_DYLIB_SUFFIX)
	$(NS_BINDIR)/ns_json_test
	$(NS_BINDIR)/ns_expr_test
	$(NS_BINDIR)/ns_shader_test
	$(NS_BINDIR)/ns_token_test
	$(NS_BINDIR)/ns_buffer_test
	$(NS_BINDIR)/ns_os_test
	$(NS_BINDIR)/ns_project_test
	sh test/ns_run_test.sh "$(CURDIR)/$(TARGET)$(NS_SUFFIX)"

include lib/Makefile
include sample/c/Makefile

install: all
	$(NS_MKDIR) $(NS_INSTALL_ROOT)/bin $(NS_INSTALL_ROOT)/lib $(NS_INSTALL_ROOT)/ref \
		$(NS_INSTALL_ROOT)/share/ns-runtime/src $(NS_INSTALL_ROOT)/share/ns-runtime/include \
		$(NS_INSTALL_ROOT)/share/ns-runtime/ref
	$(NS_CP) $(TARGET)$(NS_SUFFIX) $(NS_INSTALL_ROOT)/bin
	$(NS_CP) lib/*.ns $(NS_INSTALL_ROOT)/ref
	$(NS_CP) lib/assets $(NS_INSTALL_ROOT)/ref
	cp $(NS_EMBED_RUNTIME_SRCS) $(NS_INSTALL_ROOT)/share/ns-runtime/src/
	$(NS_CP) include/. $(NS_INSTALL_ROOT)/share/ns-runtime/include/
	cp lib/std.ns lib/shader.ns lib/simd.ns lib/task.ns $(NS_INSTALL_ROOT)/share/ns-runtime/ref/
	find $(NS_BINDIR) -maxdepth 1 -type f \( -name '*.a' -o -name '*.so' -o -name '*.dylib' -o -name '*.dll' \) -exec cp {} $(NS_INSTALL_ROOT)/lib \;
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

.PHONY: ns_xcframework ns_apple_dirs ns_apple_clean macos_arm64 ios_arm64 xcframework apple-xcframework

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

$(IOS_LIB): ns_apple_dirs $(IOS_OBJS)
	$(APPLE_LIBTOOL) -static -o $@ $(IOS_OBJS)
	codesign --force --sign - --timestamp=none $@
	@echo "📦 iOS arm64 static lib -> $@"

$(IOS_OBJDIR)/%.o: %.c | ns_apple_dirs
	mkdir -p $(dir $@)
	$(APPLE_CC) -c $< -o $@ $(NS_INC) $(IOS_CFLAGS)

# Clean only Apple artifacts
ns_apple_clean:
	rm -rf $(APPLE_OUTDIR) $(NS_XCFRAMEWORK)

endif
# ===== end Apple (Darwin) XCFramework block =====
