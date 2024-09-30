.PHONY: all run clean

CC = clang
LD = clang++

CFLAGS = -g -O0 -Iinclude `llvm-config --cflags`
LDFLAGS = `llvm-config --ldflags --libs core executionengine mcjit interpreter analysis native bitwriter --system-libs`

BINDIR = bin
OBJDIR = $(BINDIR)/obj

NS_SRCS = src/ns.c \
	src/ns_type.c \
	src/ns_path.c \
	src/ns_tokenize.c \
	src/ns_parse.c \
	src/ns_parse_stmt.c \
	src/ns_parse_expr.c \
	src/ns_parse_dump.c \
	src/ns_vm.c \
	src/ns_gen_llvm_bc.c \
	src/ns_gen_arm.c \
	src/ns_gen_x86.c
NS_OBJS = $(NS_SRCS:%.c=$(OBJDIR)/%.o)

TARGET = $(BINDIR)/ns

all: $(TARGET)

$(TARGET): $(NS_OBJS) | $(BINDIR)
	$(CC) $(NS_OBJS) -o $(TARGET) $(LDFLAGS)

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) -c $< -o $@ $(CFLAGS)

$(OBJDIR):
	mkdir -p $(OBJDIR)/src

token: all
	$(TARGET) -t sample/rt.ns

parse: all
	$(TARGET) -p sample/add.ns

bc: all
	$(TARGET) -o bin/add.bc -bc sample/add.ns
	llvm-dis bin/add.bc

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