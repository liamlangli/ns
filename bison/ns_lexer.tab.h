/* A Bison parser, made by GNU Bison 3.7.6.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_YY_NS_LEXER_TAB_H_INCLUDED
# define YY_YY_NS_LEXER_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 1
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    LET = 258,                     /* LET  */
    CONST = 259,                   /* CONST  */
    FN = 260,                      /* FN  */
    REF = 261,                     /* REF  */
    STRUCT = 262,                  /* STRUCT  */
    ASYNC = 263,                   /* ASYNC  */
    AWAIT = 264,                   /* AWAIT  */
    AS = 265,                      /* AS  */
    IF = 266,                      /* IF  */
    ELSE = 267,                    /* ELSE  */
    DO = 268,                      /* DO  */
    TO = 269,                      /* TO  */
    WHILE = 270,                   /* WHILE  */
    FOR = 271,                     /* FOR  */
    IN = 272,                      /* IN  */
    RETURN = 273,                  /* RETURN  */
    BREAK = 274,                   /* BREAK  */
    CONTINUE = 275,                /* CONTINUE  */
    TRUE = 276,                    /* TRUE  */
    FALSE = 277,                   /* FALSE  */
    NIL = 278,                     /* NIL  */
    SWITCH = 279,                  /* SWITCH  */
    CASE = 280,                    /* CASE  */
    DEFAULT = 281,                 /* DEFAULT  */
    I8 = 282,                      /* I8  */
    I16 = 283,                     /* I16  */
    I32 = 284,                     /* I32  */
    I64 = 285,                     /* I64  */
    U8 = 286,                      /* U8  */
    U16 = 287,                     /* U16  */
    U32 = 288,                     /* U32  */
    U64 = 289,                     /* U64  */
    F32 = 290,                     /* F32  */
    F64 = 291,                     /* F64  */
    BOOL = 292,                    /* BOOL  */
    BYTE = 293,                    /* BYTE  */
    STR = 294,                     /* STR  */
    ADD = 295,                     /* ADD  */
    SUB = 296,                     /* SUB  */
    MUL = 297,                     /* MUL  */
    DIV = 298,                     /* DIV  */
    MOD = 299,                     /* MOD  */
    BIT_INV = 300,                 /* BIT_INV  */
    NOT = 301,                     /* NOT  */
    AND = 302,                     /* AND  */
    OR = 303,                      /* OR  */
    XOR = 304,                     /* XOR  */
    SHL = 305,                     /* SHL  */
    SHR = 306,                     /* SHR  */
    LOGIC_AND = 307,               /* LOGIC_AND  */
    LOGIC_OR = 308,                /* LOGIC_OR  */
    EQ = 309,                      /* EQ  */
    NE = 310,                      /* NE  */
    LT = 311,                      /* LT  */
    GT = 312,                      /* GT  */
    LE = 313,                      /* LE  */
    GE = 314,                      /* GE  */
    ASSIGN = 315,                  /* ASSIGN  */
    ADD_ASSIGN = 316,              /* ADD_ASSIGN  */
    SUB_ASSIGN = 317,              /* SUB_ASSIGN  */
    MUL_ASSIGN = 318,              /* MUL_ASSIGN  */
    DIV_ASSIGN = 319,              /* DIV_ASSIGN  */
    MOD_ASSIGN = 320,              /* MOD_ASSIGN  */
    BIT_AND_ASSIGN = 321,          /* BIT_AND_ASSIGN  */
    BIT_OR_ASSIGN = 322,           /* BIT_OR_ASSIGN  */
    BIT_XOR_ASSIGN = 323,          /* BIT_XOR_ASSIGN  */
    BIT_SHL_ASSIGN = 324,          /* BIT_SHL_ASSIGN  */
    BIT_SHR_ASSIGN = 325,          /* BIT_SHR_ASSIGN  */
    OPEN_PAREN = 326,              /* OPEN_PAREN  */
    CLOSE_PAREN = 327,             /* CLOSE_PAREN  */
    OPEN_BRACE = 328,              /* OPEN_BRACE  */
    CLOSE_BRACE = 329,             /* CLOSE_BRACE  */
    OPEN_BRACKET = 330,            /* OPEN_BRACKET  */
    CLOSE_BRACKET = 331,           /* CLOSE_BRACKET  */
    COMMA = 332,                   /* COMMA  */
    COLON = 333,                   /* COLON  */
    QUESTION_MARK = 334,           /* QUESTION_MARK  */
    DOT = 335,                     /* DOT  */
    EOL = 336,                     /* EOL  */
    IDENTIFIER = 337,              /* IDENTIFIER  */
    INT = 338,                     /* INT  */
    DOUBLE = 339,                  /* DOUBLE  */
    STRING = 340                   /* STRING  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef union YYSTYPE
#line 10 "ns_lexer.y"
{
#line 8 "ns_lexer.y"

    int int_val;
    double double_val;
    char *str_val;

#line 155 "ns_lexer.tab.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;

int yyparse (void);

#endif /* !YY_YY_NS_LEXER_TAB_H_INCLUDED  */
