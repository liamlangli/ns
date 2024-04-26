.PHONY: all run clean

CC = clang
CC_OPT = -g -O0

NS_SRC = src/ns.c src/ns_type.c
NS_TOKEN = src/ns_tokenize.c
NS_PARSE = src/ns_parse.c src/ns_parse_stmt.c src/ns_parse_expr.c src/ns_parse_dump.c
NS_EVAL = src/ns_vm.c src/ns_type.c
NS_SRCS = $(NS_SRC) $(NS_TOKEN) $(NS_PARSE) $(NS_EVAL)

all: $(NS_SRCS)
	mkdir -p bin
	$(CC) $(CC_OPT) -o bin/ns $^ -Isrc

token: all
	./bin/ns -t sample/rt.ns

parse: all
	./bin/ns -p sample/rt.ns

eval: all
	./bin/ns sample/rt.ns

clean:
	rm -rf bin/*

install: $(NS_SRCS)
	$(CC) -O3 -o bin/ns $^ -Isrc
	cp bin/ns /usr/local/bin