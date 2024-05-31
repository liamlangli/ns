.PHONY: all run clean

CC = clang

CFLAGS = -g -O0
LDFLAGS =

BINDIR = bin
OBJDIR = $(BINDIR)/obj

NS_SRCS = src/ns.c \
	src/ns_type.c \
	src/ns_tokenize.c \
	src/ns_parse.c \
	src/ns_parse_stmt.c \
	src/ns_parse_expr.c \
	src/ns_parse_dump.c \
	src/ns_vm.c \
	src/ns_gen_ir.c \
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

ir: all
	$(TARGET) -ir sample/add.ns

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
	$(CC) -O3 -o bin/$(TARGET) $^ -Isrc
	cp bin/$(TARGET) /usr/local/bin