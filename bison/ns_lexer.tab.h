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
     STRUCT = 261,
     ASYNC = 262,
     AWAIT = 263,
     IF = 264,
     ELSE = 265,
     WHILE = 266,
     FOR = 267,
     IN = 268,
     RETURN = 269,
     BREAK = 270,
     CONTINUE = 271,
     TRUE = 272,
     FALSE = 273,
     NIL = 274,
     SWITCH = 275,
     CASE = 276,
     DEFAULT = 277,
     I8 = 278,
     I16 = 279,
     I32 = 280,
     I64 = 281,
     U8 = 282,
     U16 = 283,
     U32 = 284,
     U64 = 285,
     F32 = 286,
     F64 = 287,
     BOOL = 288,
     BYTE = 289,
     STR = 290,
     ADD = 291,
     SUB = 292,
     MUL = 293,
     DIV = 294,
     MOD = 295,
     BIT_INV = 296,
     NOT = 297,
     AND = 298,
     OR = 299,
     XOR = 300,
     SHL = 301,
     SHR = 302,
     LOGIC_AND = 303,
     LOGIC_OR = 304,
     EQ = 305,
     NE = 306,
     LT = 307,
     GT = 308,
     LE = 309,
     GE = 310,
     ASSIGN = 311,
     ADD_ASSIGN = 312,
     SUB_ASSIGN = 313,
     MUL_ASSIGN = 314,
     DIV_ASSIGN = 315,
     MOD_ASSIGN = 316,
     BIT_AND_ASSIGN = 317,
     BIT_OR_ASSIGN = 318,
     BIT_XOR_ASSIGN = 319,
     BIT_SHL_ASSIGN = 320,
     BIT_SHR_ASSIGN = 321,
     IDENTIFIER = 322,
     STRING = 323,
     INT = 324,
     DOUBLE = 325
   };
#endif
/* Tokens.  */
#define LET 258
#define CONST 259
#define FN 260
#define STRUCT 261
#define ASYNC 262
#define AWAIT 263
#define IF 264
#define ELSE 265
#define WHILE 266
#define FOR 267
#define IN 268
#define RETURN 269
#define BREAK 270
#define CONTINUE 271
#define TRUE 272
#define FALSE 273
#define NIL 274
#define SWITCH 275
#define CASE 276
#define DEFAULT 277
#define I8 278
#define I16 279
#define I32 280
#define I64 281
#define U8 282
#define U16 283
#define U32 284
#define U64 285
#define F32 286
#define F64 287
#define BOOL 288
#define BYTE 289
#define STR 290
#define ADD 291
#define SUB 292
#define MUL 293
#define DIV 294
#define MOD 295
#define BIT_INV 296
#define NOT 297
#define AND 298
#define OR 299
#define XOR 300
#define SHL 301
#define SHR 302
#define LOGIC_AND 303
#define LOGIC_OR 304
#define EQ 305
#define NE 306
#define LT 307
#define GT 308
#define LE 309
#define GE 310
#define ASSIGN 311
#define ADD_ASSIGN 312
#define SUB_ASSIGN 313
#define MUL_ASSIGN 314
#define DIV_ASSIGN 315
#define MOD_ASSIGN 316
#define BIT_AND_ASSIGN 317
#define BIT_OR_ASSIGN 318
#define BIT_XOR_ASSIGN 319
#define BIT_SHL_ASSIGN 320
#define BIT_SHR_ASSIGN 321
#define IDENTIFIER 322
#define STRING 323
#define INT 324
#define DOUBLE 325




#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef union YYSTYPE
#line 8 "ns_lexer.y"
{
    int int_val;
    double double_val;
    char *str_val;
}
/* Line 1529 of yacc.c.  */
#line 195 "ns_lexer.tab.h"
	YYSTYPE;
# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
# define YYSTYPE_IS_TRIVIAL 1
#endif

extern YYSTYPE yylval;

