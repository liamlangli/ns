.PHONY: all run clean

CC = gcc

all: ns.c
	mkdir -p out
	$(CC) -o out/ns $^

run: all
	./out/ns

clean:
	rm -f out/*
