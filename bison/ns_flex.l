%{
#include <ns_lexer.tab.h>
%}

%%

'let' { return LET; }
'const' { return CONST; }
'fn' { return FN; }
'struct' { return STRUCT; }
'async' { return ASYNC; }
'await' { return AWAIT; }
'for' { return FOR; }
'while' { return WHILE; }
'return' { return RETURN; }
'break' { return BREAK; }
'continue' { return CONTINUE; }
'if' { return IF; }
'else' { return ELSE; }

'=' { return EQUAL; }
'+' { return PLUS; }
'-' { return MINUS; }
'*' { return TIMES; }

'&' { return AND; }
'|' { return OR; }
'!' { return NOT; }
'~' { return BIT_INV; }
'<<' { return LSHIFT; }
'>>' { return RSHIFT; }

'+=' { return ADD_ASSIGN; }
'-=' { return SUB_ASSIGN; }
'*=' { return MUL_ASSIGN; }
'/=' { return DIV_ASSIGN; }
'%=' { return MOD_ASSIGN; }
'&=' { return AND_ASSIGN; }
'|=' { return OR_ASSIGN; }
'^=' { return XOR_ASSIGN; }
'<<=' { return LSHIFT_ASSIGN; }
'>>=' { return RSHIFT_ASSIGN; }

'==' { return EQ; }
'!=' { return NEQ; }
'<' { return LT; }
'>' { return GT; }
'<=' { return LEQ; }
'>=' { return GEQ; }

'&&' { return LOGIC_AND; }
'||' { return LOGIC_OR; }


%%