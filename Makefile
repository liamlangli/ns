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

NS_LDFLAGS = -lm -lc -lreadline

NS_DEBUG_CFLAGS = -Iinclude -g -O0 -Wall -Wextra -DNS_DEBUG
NS_RELEASE_CFLAGS = -Iinclude -Os

ifeq ($(NS_DEBUG), 1)
	NS_CFLAGS = $(NS_DEBUG_CFLAGS)
else
	NS_CFLAGS = $(NS_RELEASE_CFLAGS)
endif

ifeq ($(NS_BITCODE), 1)
	LLVM_CFLAGS = `llvm-config --cflags`
	LLVM_LDFLAGS = `llvm-config --ldflags --libs core --system-libs`
	LLVM_TRIPLE = `llvm-config --host-target`

	BITCODE_SRC = src/ns_bitcode.c
	BITCODE_OBJ = $(OBJDIR)/src/ns_bitcode.o
	BITCODE_CFLAGS = -DNS_BITCODE $(LLVM_CFLAGS)
	BITCODE_LDFLAGS = $(LLVM_LDFLAGS)
endif

BINDIR = bin
OBJDIR = $(BINDIR)

NS_LIB_SRCS = src/ns_fmt.c \
	src/ns_type.c \
	src/ns_path.c \
	src/ns_tokenize.c \
	src/ns_ast.c \
	src/ns_ast_stmt.c \
	src/ns_ast_expr.c \
	src/ns_ast_dump.c \
	src/ns_vm_parse.c \
	src/ns_vm_eval.c \
	src/ns_vm_std.c \
	src/ns_repl.c
NS_LIB_OBJS = $(NS_LIB_SRCS:%.c=$(OBJDIR)/%.o)

NS_ENTRY = src/ns.c 
NS_ENTRY_OBJ = $(OBJDIR)/src/ns.o

TARGET = $(BINDIR)/ns

NS_SRCS = $(NS_LIB_SRCS) $(NS_ENTRY)

all: $(TARGET)
	@echo "Building with options:" \
	"NS_BITCODE=$(NS_BITCODE)" \
	"NS_DEBUG=$(NS_DEBUG)"

$(BITCODE_OBJ): $(BITCODE_SRC) | $(OBJDIR)
	$(CC) -c $< -o $@ $(NS_CFLAGS) $(BITCODE_CFLAGS)

$(NS_ENTRY_OBJ): $(NS_ENTRY) | $(OBJDIR)
	$(CC) -c $< -o $@ $(NS_CFLAGS) $(BITCODE_CFLAGS)

$(TARGET): $(NS_LIB_OBJS) $(NS_ENTRY_OBJ) $(BITCODE_OBJ) | $(BINDIR)
	$(CC) $(NS_LIB_OBJS) $(NS_ENTRY_OBJ) $(BITCODE_OBJ) $ -o $(TARGET) $(NS_LDFLAGS) $(BITCODE_LDFLAGS)

$(NS_LIB_OBJS): $(OBJDIR)/%.o : %.c | $(OBJDIR)
	$(CC) -c $< -o $@ $(NS_CFLAGS)

$(OBJDIR):
	mkdir -p $(OBJDIR)/src

run: all
	$(TARGET)

token: all
	$(TARGET) -t sample/add.ns

ast: all
	$(TARGET) -p sample/add.ns

bc: all
	$(TARGET) -o bin/add.bc -b sample/add.ns
	llvm-dis bin/add.bc
	llc bin/add.bc -filetype=obj -mtriple=$(LLVM_TRIPLE) -relocation-model=pic -o bin/add.o
	clang bin/add.o -o bin/add
	bin/add

eval: all
	$(TARGET) sample/add.ns

clean:
	rm -rf $(OBJDIR)

# utility
count:
	cloc src include sample

# pack source files
pack:
	git ls-files -z | tar --null -T - -czvf bin/ns.tar.gz

debug: clean $(NS_SRCS) | $(OBJDIR)
	clang -o $(TARGET)_debug $(NS_SRCS) $(NS_DEBUG_CFLAGS) $(NS_LDFLAGS)

release: clean $(NS_SRCS) | $(OBJDIR)
	clang -o $(TARGET)_release $(NS_SRCS) $(NS_RELEASE_CFLAGS) $(NS_LDFLAGS)

install: $(TARGET)
	cp bin/$(TARGET) /usr/local/bin