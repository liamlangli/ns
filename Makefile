# OPTIONS
NS_BITCODE ?= 1
NS_DEBUG ?= 1

# VARIABLES
CC = clang

LLVM_CFLAGS = `llvm-config --cflags`
LLVM_LDFLAGS = `llvm-config --ldflags --libs core --system-libs`
LLVM_TRIPLE = `llvm-config --host-target`

CFLAGS = -Iinclude
ifeq ($(NS_DEBUG), 1)
	CFLAGS += -g -O0 -DNS_DEBUG -Wall -Wextra
else
	CFLAGS += -Os
endif

LDFLAGS = -lc -lm $(LLVM_LDFLAGS)

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
	src/ns_vm_eval.c
NS_LIB_OBJS = $(NS_LIB_SRCS:%.c=$(OBJDIR)/%.o)

NS_ENTRY = src/ns.c 
NS_ENTRY_OBJ = $(OBJDIR)/src/ns.o

ifeq ($(NS_BITCODE), 1)
	BITCODE_SRC = src/ns_bitcode.c
	BITCODE_OBJ = $(OBJDIR)/src/ns_bitcode.o
	BITCODE_FLAGS = -DNS_BITCODE
endif

TARGET = $(BINDIR)/ns

all: $(TARGET)
	@echo "Building with options:" \
	"NS_BITCODE=$(NS_BITCODE)" \
	"NS_DEBUG=$(NS_DEBUG)"

$(BITCODE_OBJ): $(BITCODE_SRC) | $(OBJDIR)
	$(CC) -c $< -o $@ $(CFLAGS) $(LLVM_CFLAGS) $(BITCODE_FLAGS)

$(NS_ENTRY_OBJ): $(NS_ENTRY) | $(OBJDIR)
	$(CC) -c $< -o $@ $(CFLAGS) $(BITCODE_FLAGS)

$(TARGET): $(NS_LIB_OBJS) $(NS_ENTRY_OBJ) $(BITCODE_OBJ) | $(BINDIR)
	$(CC) $(NS_LIB_OBJS) $(NS_ENTRY_OBJ) $(BITCODE_OBJ) $ -o $(TARGET) $(LDFLAGS)

$(NS_LIB_OBJS): $(OBJDIR)/%.o : %.c | $(OBJDIR)
	$(CC) -c $< -o $@ $(CFLAGS)

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

arm: all
	$(TARGET) -arm sample/add.ns

eval: all
	$(TARGET) sample/rt.ns

repl: all
	$(TARGET) -r

clean:
	rm -rf $(OBJDIR)

count:
	cloc src include sample

install: $(TARGET)
	cp bin/$(TARGET) /usr/local/bin