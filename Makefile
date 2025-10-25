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

ifeq ($(OS), Linux)
	NS_DYLIB_SUFFIX = .so
	NS_LIB_SUFFIX = .a
	NS_PLATFORM_DEF = -DNS_LINUX
	NS_OS =	$(NS_LINUX)
	NS_CC = gcc
	NS_LD = ld
else ifeq ($(OS), Darwin)
	NS_DYLIB_SUFFIX = .dylib
	NS_LIB_SUFFIX = .a
	NS_PLATFORM_DEF = -DNS_DARWIN
	NS_OS = $(NS_DARWIN)
else
	NS_DYLIB_SUFFIX = .so
	NS_LIB_SUFFIX = .a
	NS_SUFFIX = .exe
	NS_PLATFORM_DEF = -DNS_WIN
	NS_OS = $(NS_WIN)
	NS_HOME = $(USERPROFILE)
endif
NS_INC += $(NS_PLATFORM_DEF)

# OPTIONS
NS_DEBUG ?= 1
NS_DEBUG_HOOK ?= 1

ifeq ($(NS_OS), $(NS_WIN))
NS_LDFLAGS = -LD:/msys64/ucrt64/lib -lmsvcrt -lm -lreadline -lffi -ldl -lws2_32
else
NS_LDFLAGS = -L/usr/lib -lm -lreadline -lffi -ldl
endif

NS_DEBUG_CFLAGS = -g -O0 -Wall -Wunused-result -Wextra -DNS_DEBUG
NS_RELEASE_CFLAGS = -Os

ifeq ($(NS_DEBUG), 1)
	NS_CFLAGS = $(NS_DEBUG_CFLAGS)
else
	NS_CFLAGS = $(NS_RELEASE_CFLAGS)
endif

NS_DEBUG_TARGET = $(TARGET)_debug_$(LLVM_TRIPLE)$(NS_SUFFIX)
NS_RELEASE_TARGET = $(TARGET)_release_$(LLVM_TRIPLE)$(NS_SUFFIX)

NS_BINDIR = bin

NS_LIB_SRCS = src/ns_fmt.c \
	src/ns_type.c \
	src/ns_os.c \
	src/ns_token.c \
	src/ns_ast.c \
	src/ns_ast_stmt.c \
	src/ns_ast_expr.c \
	src/ns_ast_print.c \
	src/ns_vm_parse.c \
	src/ns_vm_eval.c \
	src/ns_vm_lib.c \
	src/ns_vm_print.c \
	src/ns_net.c \
	src/ns_json.c \
	src/ns_repl.c \
	src/ns_def.c \
	src/ns_asm.c

# iOS subset: exclude repl and vm_lib (depend on readline/libffi not available on iOS)
NS_IOS_LIB_SRCS = src/ns_fmt.c \
	src/ns_type.c \
	src/ns_os.c \
	src/ns_token.c \
	src/ns_ast.c \
	src/ns_ast_stmt.c \
	src/ns_ast_expr.c \
	src/ns_ast_print.c \
	src/ns_vm_parse.c \
	src/ns_vm_eval.c \
	src/ns_vm_print.c \
	src/ns_net.c \
	src/ns_json.c \
	src/ns_def.c \
	src/ns_asm.c

NS_LIB_OBJS = $(NS_LIB_SRCS:%.c=$(NS_BINDIR)/%.o)

NS_TEST_SRCS = test/ns_json_test.c
NS_TEST_TARGETS = $(NS_TEST_SRCS:test/%.c=$(NS_BINDIR)/%)

NS_ENTRY = src/ns.c 
NS_ENTRY_OBJ = $(NS_BINDIR)/src/ns.o

TARGET = $(NS_BINDIR)/ns

NS_SRCS = $(NS_LIB_SRCS) $(NS_ENTRY)
NS_DIRS = bin/src bin/lsp bin/debug bin/lib

all: $(NS_DIRS) $(TARGET) $(NS_LIB) $(NS_LSP_TARGET) $(NS_DAP_TARGET) std

$(NS_DIRS):
	$(NS_MKDIR) $(NS_DIRS)

$(BITCODE_OBJ): $(BITCODE_SRC)
	$(NS_CC) -c $< -o $@ $(NS_INC) $(NS_CFLAGS) $(BITCODE_CFLAGS)

$(JIT_OBJ): $(JIT_SRC)
	$(NS_CC) -c $< -o $@ $(NS_INC) $(NS_CFLAGS) $(JIT_CFLAGS)

$(NS_ENTRY_OBJ): $(NS_ENTRY)
	$(NS_CC) -c $< -o $@ $(NS_INC) $(NS_CFLAGS) $(BITCODE_CFLAGS)

$(TARGET): $(NS_LIB_OBJS) $(NS_ENTRY_OBJ) $(BITCODE_OBJ) $(JIT_OBJ) | $(NS_BINDIR)
	$(NS_LD) $(NS_LIB_OBJS) $(NS_ENTRY_OBJ) $(BITCODE_OBJ) $(JIT_OBJ) $ -o $(TARGET)$(NS_SUFFIX) $(NS_LDFLAGS) $(LLVM_LDFLAGS)

$(NS_LIB_OBJS): $(NS_BINDIR)/%.o : %.c
	$(NS_CC) -c $< -o $@ $(NS_INC) $(NS_CFLAGS)

run: all
	$(TARGET)

bc: all
	$(TARGET) -o bin/add.bc -b sample/add.ns
	llvm-dis bin/add.bc
	llc bin/add.bc -filetype=obj -mtriple=$(LLVM_TRIPLE) -relocation-model=pic -o bin/add.o
	clang bin/add.o -o bin/add
	bin/add

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

$(NS_TEST_TARGETS): $(NS_BINDIR)/%: test/%.c | $(NS_LIB)
	$(NS_CC) -o $@ $< $(NS_INC) $(NS_CFLAGS) $(NS_LDFLAGS) -L$(NS_BINDIR) -lns -Itest

test: $(NS_TEST_TARGETS)
	$(NS_BINDIR)/ns_json_test

include lib/Makefile
include lsp/Makefile
include debug/Makefile
include sample/c/Makefile

install: all ${NS_DAP_TARGET} ${NS_LSP_TARGET}
	$(NS_MKDIR) ~/.cache/ns/bin
	$(NS_CP) bin/ns$(NS_SUFFIX) ~/.cache/ns/bin
	$(NS_CP) bin/ns_debug$(NS_SUFFIX) ~/.cache/ns/bin
	$(NS_CP) bin/ns_lsp$(NS_SUFFIX) ~/.cache/ns/bin
	$(NS_MKDIR) ~/.cache/ns/ref && $(NS_CP) lib/*.ns ~/.cache/ns/ref
	$(NS_MKDIR) ~/.cache/ns/lib && $(NS_CP) bin/*$(NS_DYLIB_SUFFIX) ~/.cache/ns/lib

# ===== Apple (Darwin) XCFramework packing (macOS arm64 + iOS arm64) =====
# Unique target names to avoid clashes with other included makefiles.

ifeq ($(NS_OS), $(NS_DARWIN))

APPLE_CC        := $(shell xcrun -find clang)
APPLE_LIBTOOL   := $(shell xcrun -find libtool)
MACOS_SDK       := $(shell xcrun --sdk macosx --show-sdk-path)
IOS_SDK         := $(shell xcrun --sdk iphoneos --show-sdk-path)

MACOS_MIN_VER   ?= 12.0
IOS_MIN_VER     ?= 13.0

APPLE_OUTDIR    := $(NS_BINDIR)/apple
MACOS_OBJDIR    := $(APPLE_OUTDIR)/macos-arm64/obj
IOS_OBJDIR      := $(APPLE_OUTDIR)/ios-arm64/obj
MACOS_LIB       := $(APPLE_OUTDIR)/macos-arm64/libns.a
IOS_LIB         := $(APPLE_OUTDIR)/ios-arm64/libns.a

NS_XCFRAMEWORK  := $(NS_BINDIR)/ns.xcframework
NS_HEADERS_DIR  := include

MACOS_CFLAGS := -target arm64-apple-macos$(MACOS_MIN_VER) -isysroot $(MACOS_SDK) -fPIC -g -O0 -DNS_DEBUG -DNS_XCLIB
IOS_CFLAGS   := -target arm64-apple-ios$(IOS_MIN_VER)   -isysroot $(IOS_SDK)   -fembed-bitcode -fPIC -g -O0 -DNS_DEBUG -DNS_XCLIB

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
