.PHONY: all run clean

CC = clang

all: ns.c
	mkdir -p out
	$(CC) -g -O0 -o out/ns $^ -I.

run: all
	./out/ns test/sample.ns

clean:
	rm -f out/*
