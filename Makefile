.PHONY: all run clean

CC = clang

SRCS = src/ns.c src/ns_tokenize.c src/ns_ast.c src/ns_vm.c

all: $(SRCS)
	mkdir -p out
	$(CC) -g -O0 -o out/ns $^ -Isrc

run: all
	./out/ns sample/main.ns

clean:
	rm -f out/*
