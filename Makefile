.PHONY: all run clean

CC = clang
LD = clang++

LLVM_CFLAGS = `llvm-config --cflags`
LLVM_LDFLAGS = `llvm-config --ldflags --libs core --system-libs`
LLVM_TRIPLE = `llvm-config --host-target`

DEBUG = 1

CFLAGS = -Iinclude
ifeq ($(DEBUG), 1)
	CFLAGS += -g -O0 -DDEBUG -Wall -Wextra
else
	CFLAGS += -Os
endif

LDFLAGS = -lc -lm $(LLVM_LDFLAGS)

BINDIR = bin
OBJDIR = $(BINDIR)/obj

NS_SRCS = src/ns.c \
	src/ns_fmt.c \
	src/ns_type.c \
	src/ns_path.c \
	src/ns_tokenize.c \
	src/ns_ast.c \
	src/ns_ast_stmt.c \
	src/ns_ast_expr.c \
	src/ns_ast_dump.c \
	src/ns_vm_parse.c \
	src/ns_vm_eval.c
NS_OBJS = $(NS_SRCS:%.c=$(OBJDIR)/%.o)

LLVM_SRC = src/ns_gen_llvm_bc.c
LLVM_OBJ = $(OBJDIR)/src/ns_gen_llvm_bc.o

TARGET = $(BINDIR)/ns

all: $(TARGET)

$(LLVM_OBJ): $(LLVM_SRC) | $(OBJDIR)
	$(CC) -c $< -o $@ $(CFLAGS) $(LLVM_CFLAGS)

$(TARGET): $(NS_OBJS) $(LLVM_OBJ) | $(BINDIR)
	$(CC) $(NS_OBJS) $(LLVM_OBJ) $ -o $(TARGET) $(LDFLAGS)

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) -c $< -o $@ $(CFLAGS)

$(OBJDIR):
	mkdir -p $(OBJDIR)/src

token: all
	$(TARGET) -t sample/add.ns

ast: all
	$(TARGET) -p sample/add.ns

bc: all
	$(TARGET) -o bin/add.bc -bc sample/add.ns
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
	rm -f $(TARGET) $(NS_OBJS)
	rm -rf $(OBJDIR)

install: $(NS_SRCS)
	$(CC) -O3 -o bin/$(TARGET) $^ -Iinclude
	cp bin/$(TARGET) /usr/local/bin