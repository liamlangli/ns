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

# OPTIONS
NS_DEBUG ?= 1
NS_DEBUG_HOOK ?= 1
NS_JIT ?= 1

LLVM_CONFIG := $(shell command -v llvm-config 2>/dev/null)
ifeq ($(LLVM_CONFIG),)
	NS_IR ?= 0
else
	NS_IR ?= 1
endif

ifeq ($(NS_IR), 1)
	LLVM_CFLAGS = $(shell llvm-config --cflags 2>/dev/null)
	LLVM_LDFLAGS = $(shell llvm-config --ldflags --libs core --system-libs 2>/dev/null)
	LLVM_TRIPLE = $(shell llvm-config --host-target 2>/dev/null)

	BITCODE_SRC = src/ns_ir.c
	BITCODE_OBJ = $(NS_BINDIR)/src/ns_ir.o
	BITCODE_CFLAGS = -DNS_IR $(LLVM_CFLAGS)
	BITCODE_LDFLAGS = $(LLVM_LDFLAGS)

	JIT_SRC = src/ns_jit.c
	JIT_OBJ = $(NS_BINDIR)/src/ns_jit.o
	JIT_CFLAGS = -DNS_JIT $(LLVM_CFLAGS)
	JIT_LDFLAGS = $(LLVM_LDFLAGS)
endif

ifeq ($(NS_OS), $(NS_WIN))
NS_LDFLAGS = -LD:/msys64/ucrt64/lib -lmsvcrt -lm -lreadline -lffi -ldl -lws2_32
NS_INC = -I/usr/include -Iinclude $(NS_PLATFORM_DEF)
else
NS_LDFLAGS = -L/usr/lib -lm -lreadline -lffi -ldl
NS_INC = -I/usr/include -Iinclude $(NS_PLATFORM_DEF)
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
	src/ns_def.c
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
	cloc --force-lang=c=.h src include sample

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

install: all ns_debug ns_lsp
	$(NS_MKDIR) ~/.cache/ns/bin
	$(NS_CP) bin/ns$(NS_SUFFIX) ~/.cache/ns/bin
	$(NS_CP) bin/ns_debug$(NS_SUFFIX) ~/.cache/ns/bin
	$(NS_CP) bin/ns_lsp$(NS_SUFFIX) ~/.cache/ns/bin
	$(NS_MKDIR) ~/.cache/ns/ref && $(NS_CP) lib/*.ns ~/.cache/ns/ref
	$(NS_MKDIR) ~/.cache/ns/lib && $(NS_CP) bin/*$(NS_DYLIB_SUFFIX) ~/.cache/ns/lib