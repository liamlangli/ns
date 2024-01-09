all:
	bison -d -t ns_lexer.y
	flex ns_flex.l
	clang -o ns_lexer ns_lexer.tab.c lex.yy.c

run: all
	./ns_lexer < ../test/sample.ns