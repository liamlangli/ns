.PHONY: all run clean

CC = clang

all: ns.c ns_tokenize.c ns_ast.c ns_vm.c
	mkdir -p out
	$(CC) -g -O0 -o out/ns $^ -Isrc

run: all
	./out/ns test/sample.ns

clean:
	rm -f out/*
