.PHONY: all run clean

CC = clang

all: nsc.c ns.c ns_tokenize.c
	mkdir -p out
	$(CC) -O0 -o out/nsc $^ -I.

run: all
	./out/nsc test/simple.ns

clean:
	rm -f out/*
