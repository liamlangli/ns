.PHONY: all run clean

CC = clang

SRCS = src/ns.c src/ns_tokenize.c src/ns_parse.c src/ns_parse_stmt.c src/ns_parse_expr.c src/ns_vm.c

all: $(SRCS)
	mkdir -p out
	$(CC) -g -O0 -o out/ns $^ -Isrc

token: all
	./out/ns -t sample/token.ns

parse: all
	./out/ns -p sample/parse.ns

clean:
	rm -f out/*
