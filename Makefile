# FLAGS
MAKEFLAGS += --no-print-directory -j

# PLATFORNM
OS := $(shell uname -s 2>/dev/null || echo Windows)
NS_PLATFORM_DEF =
NS_SUFFIX =
NS_LIB_SUFFIX =
ifeq ($(OS), Linux)
	NS_LIB_SUFFIX = .so
	NS_PLATFORM_DEF = -DNS_LINUX
else ifeq ($(OS), Darwin)
	NS_LIB_SUFFIX = .dylib
	NS_PLATFORM_DEF = -DNS_DARWIN
else ifeq ($(OS), Windows)
	NS_LIB_SUFFIX = .dll
	NS_SUFFIX = .exe
	NS_PLATFORM_DEF = -DNS_WIN32
else
	$(error Unsupported platform: $(OS))
endif

# OPTIONS
LLVM_CONFIG := $(shell command -v llvm-config 2>/dev/null)
ifeq ($(LLVM_CONFIG),)
	NS_BITCODE ?= 0
else
	NS_BITCODE ?= 1
endif

NS_DEBUG ?= 1

# VARIABLES
CC = clang

ifeq ($(NS_BITCODE), 1)
	LLVM_CFLAGS = $(shell llvm-config --cflags 2>/dev/null)
	LLVM_LDFLAGS = $(shell llvm-config --ldflags --libs core --system-libs 2>/dev/null)
	LLVM_TRIPLE = $(shell llvm-config --host-target 2>/dev/null)

	BITCODE_SRC = src/ns_bitcode.c
	BITCODE_OBJ = $(OBJDIR)/src/ns_bitcode.o
	BITCODE_CFLAGS = -DNS_BITCODE $(LLVM_CFLAGS)
	BITCODE_LDFLAGS = $(LLVM_LDFLAGS)
endif

NS_LDFLAGS = -lm -lreadline `pkg-config --libs libffi` -ldl -flto
NS_INC = -Iinclude $(NS_PLATFORM_DEF) `pkg-config --cflags libffi`

NS_DEBUG_CFLAGS = $(NS_INC) -g -O0 -Wall -Wunused-result -Wextra -DNS_DEBUG
NS_RELEASE_CFLAGS = $(NS_INC) -Os

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
	src/ns_path.c \
	src/ns_token.c \
	src/ns_ast.c \
	src/ns_ast_stmt.c \
	src/ns_ast_expr.c \
	src/ns_ast_print.c \
	src/ns_vm_parse.c \
	src/ns_vm_eval.c \
	src/ns_vm_lib.c \
	src/ns_vm_print.c \
	src/ns_json.c \
	src/ns_repl.c
NS_LIB_OBJS = $(NS_LIB_SRCS:%.c=$(OBJDIR)/%.o)

NS_TEST_SRCS = test/ns_json_test.c
NS_TEST_TARGETS = $(NS_TEST_SRCS:test/%.c=$(BINDIR)/%)

NS_ENTRY = src/ns.c 
NS_ENTRY_OBJ = $(OBJDIR)/src/ns.o

TARGET = $(BINDIR)/ns

NS_SRCS = $(NS_LIB_SRCS) $(NS_ENTRY)

all: $(TARGET)
	@echo "building ns at "$(OS)" with options:" \
	"NS_BITCODE=$(NS_BITCODE)" \
	"NS_DEBUG=$(NS_DEBUG)"

$(BITCODE_OBJ): $(BITCODE_SRC) | $(OBJDIR)
	$(CC) -c $< -o $@ $(NS_CFLAGS) $(BITCODE_CFLAGS)

$(NS_ENTRY_OBJ): $(NS_ENTRY) | $(OBJDIR)
	$(CC) -c $< -o $@ $(NS_CFLAGS) $(BITCODE_CFLAGS)

$(TARGET): $(NS_LIB_OBJS) $(NS_ENTRY_OBJ) $(BITCODE_OBJ) | $(BINDIR)
	$(CC) $(NS_LIB_OBJS) $(NS_ENTRY_OBJ) $(BITCODE_OBJ) $ -o $(TARGET)$(NS_SUFFIX) $(NS_LDFLAGS) $(BITCODE_LDFLAGS)

$(NS_LIB_OBJS): $(OBJDIR)/%.o : %.c | $(OBJDIR)
	$(CC) -c $< -o $@ $(NS_CFLAGS)

$(OBJDIR):
	mkdir -p $(OBJDIR)/src

run: all
	$(TARGET)

bc: all
	$(TARGET) -o bin/add.bc -b sample/add.ns
	llvm-dis bin/add.bc
	llc bin/add.bc -filetype=obj -mtriple=$(LLVM_TRIPLE) -relocation-model=pic -o bin/add.o
	clang bin/add.o -o bin/add
	bin/add

clean:
	rm -rf $(OBJDIR)

# utility
count:
	cloc src include sample

# pack source files
pack:
	git ls-files -z | tar --null -T - -czvf bin/ns.tar.gz

debug: $(NS_SRCS) | $(OBJDIR)
	clang -o $(NS_DEBUG_TARGET) $(NS_SRCS) $(NS_DEBUG_CFLAGS) $(NS_LDFLAGS)
	tar -czvf $(NS_DEBUG_TARGET).tar.gz $(NS_DEBUG_TARGET)

release: $(NS_SRCS) | $(OBJDIR)
	clang -o $(NS_RELEASE_TARGET) $(NS_SRCS) $(NS_RELEASE_CFLAGS) $(NS_LDFLAGS)
	tar -czvf $(NS_RELEASE_TARGET).tar.gz $(NS_RELEASE_TARGET)

lib: $(NS_LIB_OBJS) | $(OBJDIR)
	$(CC) -shared $(NS_LIB_OBJS) -o $(BINDIR)/libns.a $(NS_LDFLAGS)

so: $(NS_LIB_OBJS) | $(OBJDIR)
	$(CC) -shared $(NS_LIB_OBJS) -o $(BINDIR)/libns.so $(NS_LDFLAGS)

trace: $(TARGET)
	dtrace -n 'profile-997 /execname == "$(TARGET)"/ { @[ustack()] = count(); }' -c $(TARGET) -o bin/ns.stacks

install: $(TARGET)
	cp $(TARGET) /usr/local/bin

$(NS_TEST_TARGETS): $(BINDIR)/%: test/%.c | $(BINDIR) lib
	$(CC) -o $@ $< $(NS_CFLAGS) $(NS_LDFLAGS) -lns -L$(BINDIR)

test: $(NS_TEST_TARGETS)
	$(BINDIR)/ns_json_test

include lib/Makefile
include lsp/Makefile
include dap/Makefile