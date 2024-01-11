/* A Bison parser, made by GNU Bison 2.3.  */

/* Skeleton interface for Bison's Yacc-like parsers in C

   Copyright (C) 1984, 1989, 1990, 2000, 2001, 2002, 2003, 2004, 2005, 2006
   Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.  */

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

/* Tokens.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
   /* Put the tokens into the symbol table, so that GDB and other debuggers
      know about them.  */
   enum yytokentype {
     LET = 258,
     CONST = 259,
     FN = 260,
     REF = 261,
     STRUCT = 262,
     ASYNC = 263,
     AWAIT = 264,
     AS = 265,
     IF = 266,
     ELSE = 267,
     DO = 268,
     TO = 269,
     WHILE = 270,
     FOR = 271,
     IN = 272,
     RETURN = 273,
     BREAK = 274,
     CONTINUE = 275,
     TRUE = 276,
     FALSE = 277,
     NIL = 278,
     SWITCH = 279,
     CASE = 280,
     DEFAULT = 281,
     I8 = 282,
     I16 = 283,
     I32 = 284,
     I64 = 285,
     U8 = 286,
     U16 = 287,
     U32 = 288,
     U64 = 289,
     F32 = 290,
     F64 = 291,
     BOOL = 292,
     BYTE = 293,
     STR = 294,
     ADD = 295,
     SUB = 296,
     MUL = 297,
     DIV = 298,
     MOD = 299,
     BIT_INV = 300,
     NOT = 301,
     AND = 302,
     OR = 303,
     XOR = 304,
     SHL = 305,
     SHR = 306,
     LOGIC_AND = 307,
     LOGIC_OR = 308,
     EQ = 309,
     NE = 310,
     LT = 311,
     GT = 312,
     LE = 313,
     GE = 314,
     ASSIGN = 315,
     ADD_ASSIGN = 316,
     SUB_ASSIGN = 317,
     MUL_ASSIGN = 318,
     DIV_ASSIGN = 319,
     MOD_ASSIGN = 320,
     BIT_AND_ASSIGN = 321,
     BIT_OR_ASSIGN = 322,
     BIT_XOR_ASSIGN = 323,
     BIT_SHL_ASSIGN = 324,
     BIT_SHR_ASSIGN = 325,
     OPEN_PAREN = 326,
     CLOSE_PAREN = 327,
     OPEN_BRACE = 328,
     CLOSE_BRACE = 329,
     OPEN_BRACKET = 330,
     CLOSE_BRACKET = 331,
     COMMA = 332,
     COLON = 333,
     QUESTION_MARK = 334,
     DOT = 335,
     EOL = 336,
     IDENTIFIER = 337,
     INT = 338,
     DOUBLE = 339,
     STRING = 340
   };
#endif
/* Tokens.  */
#define LET 258
#define CONST 259
#define FN 260
#define REF 261
#define STRUCT 262
#define ASYNC 263
#define AWAIT 264
#define AS 265
#define IF 266
#define ELSE 267
#define DO 268
#define TO 269
#define WHILE 270
#define FOR 271
#define IN 272
#define RETURN 273
#define BREAK 274
#define CONTINUE 275
#define TRUE 276
#define FALSE 277
#define NIL 278
#define SWITCH 279
#define CASE 280
#define DEFAULT 281
#define I8 282
#define I16 283
#define I32 284
#define I64 285
#define U8 286
#define U16 287
#define U32 288
#define U64 289
#define F32 290
#define F64 291
#define BOOL 292
#define BYTE 293
#define STR 294
#define ADD 295
#define SUB 296
#define MUL 297
#define DIV 298
#define MOD 299
#define BIT_INV 300
#define NOT 301
#define AND 302
#define OR 303
#define XOR 304
#define SHL 305
#define SHR 306
#define LOGIC_AND 307
#define LOGIC_OR 308
#define EQ 309
#define NE 310
#define LT 311
#define GT 312
#define LE 313
#define GE 314
#define ASSIGN 315
#define ADD_ASSIGN 316
#define SUB_ASSIGN 317
#define MUL_ASSIGN 318
#define DIV_ASSIGN 319
#define MOD_ASSIGN 320
#define BIT_AND_ASSIGN 321
#define BIT_OR_ASSIGN 322
#define BIT_XOR_ASSIGN 323
#define BIT_SHL_ASSIGN 324
#define BIT_SHR_ASSIGN 325
#define OPEN_PAREN 326
#define CLOSE_PAREN 327
#define OPEN_BRACE 328
#define CLOSE_BRACE 329
#define OPEN_BRACKET 330
#define CLOSE_BRACKET 331
#define COMMA 332
#define COLON 333
#define QUESTION_MARK 334
#define DOT 335
#define EOL 336
#define IDENTIFIER 337
#define INT 338
#define DOUBLE 339
#define STRING 340




#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef union YYSTYPE
#line 10 "ns_lexer.y"
{
    int int_val;
    double double_val;
    char *str_val;
}
/* Line 1529 of yacc.c.  */
#line 225 "ns_lexer.tab.h"
	YYSTYPE;
# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
# define YYSTYPE_IS_TRIVIAL 1
#endif

extern YYSTYPE yylval;

