# FLAGS
MAKEFLAGS += --no-print-directory -j

# PLATFORNM
OS := $(shell uname -s 2>/dev/null || echo Windows)
NS_PLATFORM_DEF =
NS_SUFFIX =
NS_DYLIB_SUFFIX =
NS_LIB_SUFFIX = 
NS_OS = 

NS_DARWIN = darwin
NS_LINUX = linux
NS_WIN32 = windows

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
	NS_PLATFORM_DEF = -DNS_WIN32
	NS_OS = $(NS_WIN32)
	NS_HOME = $(USERPROFILE)
endif

# OPTIONS
NS_DEBUG ?= 1
NS_DEBUG_HOOK ?= 1
NS_JIT ?= 1

LLVM_CONFIG := $(shell command -v llvm-config 2>/dev/null)
ifeq ($(LLVM_CONFIG),)
	NS_BITCODE ?= 0
else
	NS_BITCODE ?= 1
endif

ifeq ($(NS_BITCODE), 1)
	LLVM_CFLAGS = $(shell llvm-config --cflags 2>/dev/null)
	LLVM_LDFLAGS = $(shell llvm-config --ldflags --libs core --system-libs 2>/dev/null)
	LLVM_TRIPLE = $(shell llvm-config --host-target 2>/dev/null)

	BITCODE_SRC = src/ns_bitcode.c
	BITCODE_OBJ = $(OBJDIR)/src/ns_bitcode.o
	BITCODE_CFLAGS = -DNS_BITCODE $(LLVM_CFLAGS)
	BITCODE_LDFLAGS = $(LLVM_LDFLAGS)

	JIT_SRC = src/ns_jit.c
	JIT_OBJ = $(OBJDIR)/src/ns_jit.o
	JIT_CFLAGS = -DNS_JIT $(LLVM_CFLAGS)
	JIT_LDFLAGS = $(LLVM_LDFLAGS)
endif

ifeq ($(NS_OS), $(NS_WIN32))
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

BINDIR = bin
OBJDIR = $(BINDIR)

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
	src/ns_repl.c
NS_LIB_OBJS = $(NS_LIB_SRCS:%.c=$(OBJDIR)/%.o)

NS_TEST_SRCS = test/ns_json_test.c
NS_TEST_TARGETS = $(NS_TEST_SRCS:test/%.c=$(BINDIR)/%)

NS_ENTRY = src/ns.c 
NS_ENTRY_OBJ = $(OBJDIR)/src/ns.o

TARGET = $(BINDIR)/ns

NS_SRCS = $(NS_LIB_SRCS) $(NS_ENTRY)

all: ns_dirs $(TARGET) std lib ns_lsp ns_debug
	@echo "building ns at "$(OS)" with options:" \
	"NS_BITCODE=$(NS_BITCODE)" \
	"NS_DEBUG=$(NS_DEBUG)"

ns_dirs:
	$(NS_MKDIR) bin
	$(NS_MKDIR) bin/src bin/lsp bin/debug bin/lib

$(BITCODE_OBJ): $(BITCODE_SRC) | $(OBJDIR)
	$(NS_CC) -c $< -o $@ $(NS_INC) $(NS_CFLAGS) $(BITCODE_CFLAGS)

$(JIT_OBJ): $(JIT_SRC) | $(OBJDIR)
	$(NS_CC) -c $< -o $@ $(NS_INC) $(NS_CFLAGS) $(JIT_CFLAGS)

$(NS_ENTRY_OBJ): $(NS_ENTRY) | $(OBJDIR)
	$(NS_CC) -c $< -o $@ $(NS_INC) $(NS_CFLAGS) $(BITCODE_CFLAGS)

$(TARGET): $(NS_LIB_OBJS) $(NS_ENTRY_OBJ) $(BITCODE_OBJ) $(JIT_OBJ) | $(BINDIR)
	$(NS_LD) $(NS_LIB_OBJS) $(NS_ENTRY_OBJ) $(BITCODE_OBJ) $(JIT_OBJ) $ -o $(TARGET)$(NS_SUFFIX) $(NS_LDFLAGS) $(LLVM_LDFLAGS)

$(NS_LIB_OBJS): $(OBJDIR)/%.o : %.c | $(OBJDIR)
	$(NS_CC) -c $< -o $@ $(NS_INC) $(NS_CFLAGS)

$(OBJDIR):
	$(NS_MKDIR) $(OBJDIR)/src

run: all
	$(TARGET)

bc: all
	$(TARGET) -o bin/add.bc -b sample/add.ns
	llvm-dis bin/add.bc
	llc bin/add.bc -filetype=obj -mtriple=$(LLVM_TRIPLE) -relocation-model=pic -o bin/add.o
	clang bin/add.o -o bin/add
	bin/add

clean:
	$(NS_RMDIR) $(OBJDIR)

# utility
count:
	cloc src include sample

# pack source files
pack:
	git ls-files -z | tar --null -T - -czvf bin/ns.tar.gz

lib: $(NS_LIB_OBJS) | $(OBJDIR)
	ar rcs $(BINDIR)/libns$(NS_LIB_SUFFIX) $(NS_LIB_OBJS)

so: $(NS_LIB_OBJS) | $(OBJDIR)
	$(NS_CC) -shared $(NS_LIB_OBJS) -o $(BINDIR)/ns$(NS_DYLIB_SUFFIX) $(NS_LDFLAGS)

$(NS_TEST_TARGETS): $(BINDIR)/%: test/%.c | $(BINDIR) lib
	$(NS_CC) -o $@ $< $(NS_INC) $(NS_CFLAGS) $(NS_LDFLAGS) -lns -L$(BINDIR) -Itest

test: $(NS_TEST_TARGETS)
	$(BINDIR)/ns_json_test

include lib/Makefile
include lsp/Makefile
include debug/Makefile

install: all ns_debug ns_lsp
	$(NS_MKDIR) ~/.cache/ns/bin
	$(NS_CP) bin/ns$(NS_SUFFIX) ~/.cache/ns/bin
	$(NS_CP) bin/ns_debug$(NS_SUFFIX) ~/.cache/ns/bin
	$(NS_CP) bin/ns_lsp$(NS_SUFFIX) ~/.cache/ns/bin
	$(NS_MKDIR) ~/.cache/ns/ref && $(NS_CP) lib/*.ns ~/.cache/ns/ref
	$(NS_MKDIR) ~/.cache/ns/lib && $(NS_CP) bin/*$(NS_DYLIB_SUFFIX) ~/.cache/ns/lib