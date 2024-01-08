%{
#include <stdio.h>
#include <stdlib.h>
void yyerror(char *s);
int yylex(void);
%}

%union {
    int int_val;
    double double_val;
    char *str_val;
}

%token LET CONST FN STRUCT ASYNC AWAIT
%token IF ELSE WHILE FOR IN RETURN BREAK CONTINUE TRUE FALSE NIL SWITCH CASE DEFAULT
%token I8 I16 I32 I64 U8 U16 U32 U64 F32 F64 BOOL BYTE STR
%token ADD SUB MUL DIV MOD BIT_INV NOT
%token AND OR XOR SHL SHR
%token LOGIC_AND LOGIC_OR EQ NE LT GT LE GE
%token ASSIGN ADD_ASSIGN SUB_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN
%token BIT_AND_ASSIGN BIT_OR_ASSIGN BIT_XOR_ASSIGN BIT_SHL_ASSIGN BIT_SHR_ASSIGN
%token <str_val> IDENTIFIER
%token <str_val> STRING
%token <int_val> INT
%token <double_val> DOUBLE

%%

program:
    | statements
    ;

statements: statements statement
    | statement

statement: expression
    | declaration
    | fn_declaration
    | lambda_declaration
    | struct_declaration
    | if_statement
    | while_statement
    | for_statement
    | switch_statement
    | return_statement
    | break_statement
    | continue_statement
    ;

type: I8
    | I16
    | I32
    | I64
    | U8
    | U16
    | U32
    | U64
    | F32
    | F64
    | BOOL
    | BYTE
    | STR
    | IDENTIFIER
    ;

type_declaration:
    | ':' type
    ;

declaration: LET IDENTIFIER type_declaration
    | LET IDENTIFIER type_declaration ASSIGN expression
    | CONST IDENTIFIER type_declaration
    | CONST IDENTIFIER type_declaration ASSIGN expression
    ;

assignment_operator: ASSIGN
    | ADD_ASSIGN
    | SUB_ASSIGN
    | MUL_ASSIGN
    | DIV_ASSIGN
    | MOD_ASSIGN
    | BIT_AND_ASSIGN
    | BIT_OR_ASSIGN
    | BIT_XOR_ASSIGN
    | BIT_SHL_ASSIGN
    | BIT_SHR_ASSIGN
    ;

athmetic_operator: ADD
    | SUB
    | MUL
    | DIV
    | MOD
    ;

bitwise_operator: AND
    | OR
    | XOR
    | SHL
    | SHR
    ;

logical_operator: LOGIC_AND
    | LOGIC_OR
    ;

comparison_operator: EQ
    | NE
    | LT
    | GT
    | LE
    | GE
    ;

if_statement: IF '(' expression ')' '{' program '}'
    | IF '(' expression ')' '{' program '}' ELSE '{' program '}'
    ;

while_statement: WHILE '(' expression ')' '{' program '}'
    ;

for_statement: FOR '(' expression ';' expression ';' expression ')' '{' program '}'
    ;

switch_statement: SWITCH '(' expression ')' '{' cases '}'
    ;

cases: cases case
    | case
    ;

case: CASE expression ':' program
    | DEFAULT ':' program
    ;

return_statement: RETURN expression
    ;

break_statement: BREAK
    ;

continue_statement:  CONTINUE
    ;

expression: expression '[' expression ']'
    | expression '(' ')'
    | expression '(' expression ')'
    | expression '?' expression ':' expression
    | '(' expression ')'
    | INT
    | DOUBLE
    | STRING
    | IDENTIFIER
    | TRUE
    | FALSE
    | NIL
    | ASYNC expression
    | AWAIT expression
    | expression '.' IDENTIFIER
    | expression '(' arguments ')'
    | expression '(' parameters ')' '{' program '}'
    | expression '(' parameters ')' '{' '}'
    | expression '(' ')' '{' program '}'
    | expression '(' ')' '{' '}'
    | NOT expression
    | BIT_INV expression
    | expression athmetic_operator expression
    | expression bitwise_operator expression
    | expression logical_operator expression
    | expression comparison_operator expression
    | expression assignment_operator expression
    ;

arguments: arguments ',' argument
    | argument
    ;

argument: IDENTIFIER ':' type
    | expression
    ;

fn_declaration: FN IDENTIFIER '(' parameters ')' type_declaration '{' program '}'
    | FN IDENTIFIER '(' parameters ')' type_declaration '{' '}'
    | ASYNC FN IDENTIFIER '(' parameters ')' type_declaration '{' program '}'
    | ASYNC FN IDENTIFIER '(' parameters ')' type_declaration '{' '}'
    ;

lambda_declaration: '{' '(' parameters ')' IN '{' program '}' '}'
    | '{' '(' parameters ')' ':' type IN '{' program '}' '}'
    ;

parameters:
    | parameters ',' parameter
    | parameter
    ;

parameter: IDENTIFIER
    | IDENTIFIER ':' type
    ;

struct_declaration: STRUCT IDENTIFIER '{' '}'
    | STRUCT IDENTIFIER '{' struct_fields '}'
    ;

struct_fields: struct_fields struct_field
    | struct_field
    ;

struct_field: IDENTIFIER ':' type
    ;
%%