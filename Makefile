.PHONY: all run clean

CC = clang
CC_OPT = -g -O0
SRCS = src/ns.c src/ns_tokenize.c src/ns_parse.c src/ns_parse_stmt.c src/ns_parse_expr.c src/ns_vm.c src/ns_type.c

all: $(SRCS)
	mkdir -p out
	$(CC) $(CC_OPT) -o out/ns $^ -Isrc

token: all
	./out/ns -t sample/rt.ns

parse: all
	./out/ns -p sample/rt.ns

eval: all
	./out/ns sample/rt.ns

clean:
	rm -rf out/*

install: $(SRCS)
	$(CC) -O3 -o out/ns $^ -Isrc
	cp out/ns /usr/local/bin