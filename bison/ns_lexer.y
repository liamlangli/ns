%{
#include <stdio.h>
#include <stdlib.h>
void yyerror(const char *s);
int yylex(void);
%}

%union {
    int int_val;
    double double_val;
    char *str_val;
}

%token LET CONST FN REF STRUCT ASYNC AWAIT AS
%token IF ELSE DO TO WHILE FOR IN RETURN BREAK CONTINUE TRUE FALSE NIL SWITCH CASE DEFAULT
%token I8 I16 I32 I64 U8 U16 U32 U64 F32 F64 BOOL BYTE STR
%token ADD SUB MUL DIV MOD BIT_INV NOT
%token AND OR XOR SHL SHR
%token LOGIC_AND LOGIC_OR EQ NE LT GT LE GE
%token ASSIGN ADD_ASSIGN SUB_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN
%token BIT_AND_ASSIGN BIT_OR_ASSIGN BIT_XOR_ASSIGN BIT_SHL_ASSIGN BIT_SHR_ASSIGN
%token OPEN_PAREN CLOSE_PAREN OPEN_BRACE CLOSE_BRACE OPEN_BRACKET CLOSE_BRACKET
%token COMMA COLON QUESTION_MARK DOT
%token EOL
%token IDENTIFIER INT DOUBLE STRING

%%

program: statement
    ;

statement: expression_statement 
    | selection_statement
    | labeled_statement
    | iteration_statement
    | declaration_statement
    | jump_statement
    ;

labeled_statement: CASE literal COLON statement EOL
    | DEFAULT COLON statement EOL
    ;

iteration_statement: WHILE expression OPEN_BRACE statement CLOSE_BRACE  EOL
    | DO OPEN_BRACE statement CLOSE_BRACE WHILE expression EOL
    | FOR IDENTIFIER INT TO INT OPEN_BRACE statement CLOSE_BRACE EOL
    | FOR IDENTIFIER type_declaration IN IDENTIFIER OPEN_BRACE statement CLOSE_BRACE EOL
    
selection_statement: IF expression OPEN_BRACE statement CLOSE_BRACE EOL
    | IF expression OPEN_BRACE statement CLOSE_BRACE ELSE OPEN_BRACE statement CLOSE_BRACE EOL
    | SWITCH expression OPEN_BRACE statement CLOSE_BRACE
    ;

expression_statement: EOL
    | OPEN_BRACE expression CLOSE_BRACE EOL
    ;

declaration_statement: fn_declaration
    | struct_declaration
    | variable_declaration
    ;

jump_statement: RETURN expression EOL
    | BREAK EOL
    | CONTINUE EOL
    ;

variable_declaration: type_qualifier IDENTIFIER type_declaration
    | type_qualifier IDENTIFIER type_declaration ASSIGN expression
    | type_qualifier IDENTIFIER type_declaration ASSIGN lambda_declaration
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
    | COLON type
    ;

type_qualifier: CONST
    | LET
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

conditional_expression: logical_or_expression
    | logical_or_expression QUESTION_MARK expression COLON conditional_expression
    ;

logical_or_expression: logical_and_expression
    | logical_or_expression LOGIC_OR logical_and_expression
    ;

logical_and_expression: inclusive_or_expression
    | logical_and_expression LOGIC_AND inclusive_or_expression
    ;

inclusive_or_expression: exclusive_or_expression
    | inclusive_or_expression OR exclusive_or_expression
    ;

exclusive_or_expression: and_expression
    | exclusive_or_expression XOR and_expression
    ;

and_expression: equality_expression
    | and_expression AND equality_expression
    ;

equality_expression: relational_expression
    | equality_expression EQ relational_expression
    | equality_expression NE relational_expression
    ;

relational_expression: shift_expression
    | relational_expression LT shift_expression
    | relational_expression GT shift_expression
    | relational_expression LE shift_expression
    | relational_expression GE shift_expression
    ;

shift_expression: additive_expression
    | shift_expression SHL additive_expression
    | shift_expression SHR additive_expression
    ;

additive_expression: multiplicative_expression
    | additive_expression ADD multiplicative_expression
    | additive_expression SUB multiplicative_expression
    ;

multiplicative_expression: unary_expression
    | multiplicative_expression MUL unary_expression
    | multiplicative_expression DIV unary_expression
    | multiplicative_expression MOD unary_expression
    ;

unary_expression: postfix_expression
    | unary_operator unary_expression
    | AWAIT unary_expression
    ;

unary_operator: AND
    | MUL
    | ADD
    | SUB
    | BIT_INV
    | NOT
    ;

postfix_expression: primary_expression
    | postfix_expression OPEN_PAREN CLOSE_PAREN
    | postfix_expression OPEN_PAREN assignment_expression CLOSE_PAREN
    | postfix_expression OPEN_BRACKET expression CLOSE_BRACKET
    | postfix_expression DOT IDENTIFIER
    | postfix_expression AS type
    ;

primary_expression: literal
    | IDENTIFIER
    | OPEN_PAREN expression CLOSE_PAREN
    ;

literal: INT
    | DOUBLE
    | STRING
    | TRUE
    | FALSE
    | NIL
    ;

expression: assignment_expression
    | expression COMMA assignment_expression
    ;

assignment_expression: conditional_expression
    | unary_expression assignment_operator assignment_expression
    ;

fn_qualifier: 
    | ASYNC
    ;

fn_declaration: fn_qualifier FN IDENTIFIER OPEN_PAREN parameters CLOSE_PAREN type_declaration OPEN_BRACE statement CLOSE_BRACE EOL
    ;

lambda_declaration: OPEN_BRACE OPEN_PAREN parameters CLOSE_PAREN type_declaration IN statement CLOSE_BRACE
    ;

parameters: parameters COMMA parameter
    | parameter
    ;

parameter_qualifier:
    | REF
    ;

parameter:
    | parameter_qualifier IDENTIFIER type_declaration
    ;

struct_declaration: STRUCT IDENTIFIER OPEN_BRACE struct_fields CLOSE_BRACE EOL
    ;

struct_fields: struct_fields EOL struct_field
    | struct_field
    ;

struct_field:
    | IDENTIFIER COLON type
    ;
%%

void yyerror(const char *s) {
    fprintf(stderr, "Error: %s\n", s);
}

int main(void) {
    yydebug = 1;
    printf("Enter expressions:\n");
    yyparse();
    return 0;
}