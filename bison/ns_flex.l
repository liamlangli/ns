%{
#include "ns_lexer.tab.h"
%}

%%

"async" { return ASYNC; }
"await" { return AWAIT; }
"as" { return AS; }
"let" { return LET; }
"const" { return CONST; }
"fn" { return FN; }
"struct" { return STRUCT; }
"nil" { return NIL; }
"ref" { return REF; }

"for" { return FOR; }
"do" { return DO; }
"to" { return TO; }
"while" { return WHILE; }
"return" { return RETURN; }
"break" { return BREAK; }
"continue" { return CONTINUE; }
"if" { return IF; }
"else" { return ELSE; }

"=" { return ASSIGN; }
"+" { return ADD; }
"-" { return SUB; }
"*" { return MUL; }
"/" { return DIV; }
"%" { return MOD; }

"&" { return AND; }
"|" { return OR; }
"!" { return NOT; }
"~" { return BIT_INV; }
"<<" { return SHL; }
">>" { return SHR; }

"+=" { return ADD_ASSIGN; }
"-=" { return SUB_ASSIGN; }
"*=" { return MUL_ASSIGN; }
"/=" { return DIV_ASSIGN; }
"%=" { return MOD_ASSIGN; }
"&=" { return BIT_AND_ASSIGN; }
"|=" { return BIT_OR_ASSIGN; }
"^=" { return BIT_XOR_ASSIGN; }
"<<=" { return BIT_SHL_ASSIGN; }
">>=" { return BIT_SHR_ASSIGN; }

"==" { return EQ; }
"!=" { return NE; }
"<" { return LT; }
">" { return GT; }
"<=" { return LE; }
">=" { return GE; }

"&&" { return LOGIC_AND; }
"||" { return LOGIC_OR; }

"(" { return OPEN_PAREN; }
")" { return CLOSE_PAREN; }
"{" { return OPEN_BRACE; }
"}" { return CLOSE_BRACE; }
"[" { return OPEN_BRACKET; }
"]" { return CLOSE_BRACKET; }
"," { return COMMA; }
":" { return COLON; }
"?" { return QUESTION_MARK; }
"." { return DOT; }
";" { return EOL; }
"\n" { return EOL; }
"\r\n" { return EOL; }

%%

int yywrap() {
    return 1;
}