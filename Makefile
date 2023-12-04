.PHONY: all run clean

CC = gcc

all: ns.c
	$(CC) -o out/ns $^

run: all
	./out/ns

clean:
	rm -f out/*
