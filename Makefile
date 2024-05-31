.PHONY: all run clean

CC = gcc
ifeq ($(UNAME_S),Darwin)
	CC = clang
endif

CC_OPT = -g -O0

LLVM_CONFIG = llvm-config
CFLAGS = `$(LLVM_CONFIG) --cflags`
LDFLAGS = `$(LLVM_CONFIG) --ldflags --libs --system-libs core native`
BINDIR = bin
OBJDIR = $(BINDIR)/obj

NS_SRC = src/ns.c src/ns_type.c
NS_TOKEN = src/ns_tokenize.c
NS_PARSE = src/ns_parse.c src/ns_parse_stmt.c src/ns_parse_expr.c src/ns_parse_dump.c
NS_EVAL = src/ns_vm.c src/ns_type.c
NS_CODE_GEN = src/ns_gen_ir.c src/ns_gen_arm.c src/ns_gen_x86.c
NS_SRCS = $(NS_SRC) $(NS_TOKEN) $(NS_PARSE) $(NS_EVAL) $(NS_CODE_GEN)
NS_OBJS = $(NS_SRCS:%.c=$(OBJDIR)/%.o)

TARGET = $(BINDIR)/ns

all: $(TARGET)

$(TARGET): $(NS_OBJS) | $(BINDIR)
	$(CC) $(NS_OBJS) -o $(TARGET) $(LDFLAGS)

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) -c $< -o $@ $(CFLAGS)

$(OBJDIR):
	mkdir -p $(OBJDIR)

token: all
	./bin/$(TARGET) -t sample/rt.ns

parse: all
	./bin/$(TARGET) -p sample/add.ns

ir: all
	./bin/$(TARGET) -ir sample/add.ns

arm: all
	./bin/$(TARGET) -arm sample/add.ns

eval: all
	./bin/$(TARGET) sample/rt.ns

repl: all
	./bin/$(TARGET) -r

clean:
	rm -f $(TARGET) $(NS_OBJS)
	rm -rf $(OBJDIR)

install: $(NS_SRCS)
	$(CC) -O3 -o bin/$(TARGET) $^ -Isrc
	cp bin/$(TARGET) /usr/local/bin