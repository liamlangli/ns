.PHONY: all

all: add.ll
	mkdir -p bytecode bin
	llvm-as add.ll -o bytecode/add.bc
	lli bytecode/add.bc

hello: hello.ll
	clang hello.ll -o bin/hello
	./bin/hello