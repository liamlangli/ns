.PHONY: all run clean

CC = gcc
ifeq ($(UNAME_S),Darwin)
	CC = clang
endif

CC_OPT = -g -O0

NS_SRC = src/ns.c src/ns_type.c
NS_TOKEN = src/ns_tokenize.c
NS_PARSE = src/ns_parse.c src/ns_parse_stmt.c src/ns_parse_expr.c src/ns_parse_dump.c
NS_EVAL = src/ns_vm.c src/ns_type.c
NS_CODE_GEN = src/ns_gen_ir.c src/ns_gen_arm.c src/ns_gen_x86.c
NS_SRCS = $(NS_SRC) $(NS_TOKEN) $(NS_PARSE) $(NS_EVAL) $(NS_CODE_GEN)

all: $(NS_SRCS)
	mkdir -p bin
	$(CC) $(CC_OPT) -o bin/ns $^ -Isrc

token: all
	./bin/ns -t sample/rt.ns

parse: all
	./bin/ns -p sample/rt.ns

ir: all
	./bin/ns -ir sample/add.ns

arm: all
	./bin/ns -arm sample/add.ns

eval: all
	./bin/ns sample/rt.ns

repl: all
	./bin/ns -r

clean:
	rm -rf bin/*

install: $(NS_SRCS)
	$(CC) -O3 -o bin/ns $^ -Isrc
	cp bin/ns /usr/local/bin