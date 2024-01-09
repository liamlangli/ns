/* A Bison parser, made by GNU Bison 2.3.  */

/* Skeleton implementation for Bison's Yacc-like parsers in C

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

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output.  */
#define YYBISON 1

/* Bison version.  */
#define YYBISON_VERSION "2.3"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 0

/* Using locations.  */
#define YYLSP_NEEDED 0



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




/* Copy the first part of user declarations.  */
#line 1 "ns_lexer.y"

#include <stdio.h>
#include <stdlib.h>
void yyerror(const char *s);
int yylex(void);


/* Enabling traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif

/* Enabling verbose error messages.  */
#ifdef YYERROR_VERBOSE
# undef YYERROR_VERBOSE
# define YYERROR_VERBOSE 1
#else
# define YYERROR_VERBOSE 0
#endif

/* Enabling the token table.  */
#ifndef YYTOKEN_TABLE
# define YYTOKEN_TABLE 0
#endif

#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef union YYSTYPE
#line 8 "ns_lexer.y"
{
    int int_val;
    double double_val;
    char *str_val;
}
/* Line 193 of yacc.c.  */
#line 279 "ns_lexer.tab.c"
	YYSTYPE;
# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
# define YYSTYPE_IS_TRIVIAL 1
#endif



/* Copy the second part of user declarations.  */


/* Line 216 of yacc.c.  */
#line 292 "ns_lexer.tab.c"

#ifdef short
# undef short
#endif

#ifdef YYTYPE_UINT8
typedef YYTYPE_UINT8 yytype_uint8;
#else
typedef unsigned char yytype_uint8;
#endif

#ifdef YYTYPE_INT8
typedef YYTYPE_INT8 yytype_int8;
#elif (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
typedef signed char yytype_int8;
#else
typedef short int yytype_int8;
#endif

#ifdef YYTYPE_UINT16
typedef YYTYPE_UINT16 yytype_uint16;
#else
typedef unsigned short int yytype_uint16;
#endif

#ifdef YYTYPE_INT16
typedef YYTYPE_INT16 yytype_int16;
#else
typedef short int yytype_int16;
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif ! defined YYSIZE_T && (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned int
# endif
#endif

#define YYSIZE_MAXIMUM ((YYSIZE_T) -1)

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(msgid) dgettext ("bison-runtime", msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(msgid) msgid
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YYUSE(e) ((void) (e))
#else
# define YYUSE(e) /* empty */
#endif

/* Identity function, used to suppress warnings about constant conditions.  */
#ifndef lint
# define YYID(n) (n)
#else
#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static int
YYID (int i)
#else
static int
YYID (i)
    int i;
#endif
{
  return i;
}
#endif

#if ! defined yyoverflow || YYERROR_VERBOSE

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined _STDLIB_H && (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#     ifndef _STDLIB_H
#      define _STDLIB_H 1
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's `empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (YYID (0))
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined _STDLIB_H \
       && ! ((defined YYMALLOC || defined malloc) \
	     && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef _STDLIB_H
#    define _STDLIB_H 1
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined _STDLIB_H && (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined _STDLIB_H && (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* ! defined yyoverflow || YYERROR_VERBOSE */


#if (! defined yyoverflow \
     && (! defined __cplusplus \
	 || (defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yytype_int16 yyss;
  YYSTYPE yyvs;
  };

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (sizeof (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (sizeof (yytype_int16) + sizeof (YYSTYPE)) \
      + YYSTACK_GAP_MAXIMUM)

/* Copy COUNT objects from FROM to TO.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(To, From, Count) \
      __builtin_memcpy (To, From, (Count) * sizeof (*(From)))
#  else
#   define YYCOPY(To, From, Count)		\
      do					\
	{					\
	  YYSIZE_T yyi;				\
	  for (yyi = 0; yyi < (Count); yyi++)	\
	    (To)[yyi] = (From)[yyi];		\
	}					\
      while (YYID (0))
#  endif
# endif

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack)					\
    do									\
      {									\
	YYSIZE_T yynewbytes;						\
	YYCOPY (&yyptr->Stack, Stack, yysize);				\
	Stack = &yyptr->Stack;						\
	yynewbytes = yystacksize * sizeof (*Stack) + YYSTACK_GAP_MAXIMUM; \
	yyptr += yynewbytes / sizeof (*yyptr);				\
      }									\
    while (YYID (0))

#endif

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  74
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   267

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  86
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  41
/* YYNRULES -- Number of rules.  */
#define YYNRULES  130
/* YYNRULES -- Number of states.  */
#define YYNSTATES  245

/* YYTRANSLATE(YYLEX) -- Bison symbol number corresponding to YYLEX.  */
#define YYUNDEFTOK  2
#define YYMAXUTOK   340

#define YYTRANSLATE(YYX)						\
  ((unsigned int) (YYX) <= YYMAXUTOK ? yytranslate[YYX] : YYUNDEFTOK)

/* YYTRANSLATE[YYLEX] -- Bison symbol number corresponding to YYLEX.  */
static const yytype_uint8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65,    66,    67,    68,    69,    70,    71,    72,    73,    74,
      75,    76,    77,    78,    79,    80,    81,    82,    83,    84,
      85
};

#if YYDEBUG
/* YYPRHS[YYN] -- Index of the first RHS symbol of rule number YYN in
   YYRHS.  */
static const yytype_uint16 yyprhs[] =
{
       0,     0,     3,     5,     7,     9,    11,    13,    15,    17,
      23,    28,    35,    43,    53,    63,    70,    81,    87,    89,
      94,    96,    98,   100,   104,   107,   110,   114,   120,   126,
     128,   130,   132,   134,   136,   138,   140,   142,   144,   146,
     148,   150,   152,   154,   155,   158,   160,   162,   164,   166,
     168,   170,   172,   174,   176,   178,   180,   182,   184,   186,
     192,   194,   198,   200,   204,   206,   210,   212,   216,   218,
     222,   224,   228,   232,   234,   238,   242,   246,   250,   252,
     256,   260,   262,   266,   270,   272,   276,   280,   284,   286,
     289,   292,   294,   296,   298,   300,   302,   304,   306,   310,
     315,   320,   324,   328,   330,   332,   336,   338,   340,   342,
     344,   346,   348,   350,   354,   356,   360,   361,   363,   375,
     384,   388,   390,   391,   393,   394,   398,   405,   409,   411,
     412
};

/* YYRHS -- A `-1'-separated list of the rules' RHS.  */
static const yytype_int8 yyrhs[] =
{
      87,     0,    -1,    88,    -1,    92,    -1,    91,    -1,    89,
      -1,    90,    -1,    93,    -1,    94,    -1,    25,   115,    78,
      88,    81,    -1,    26,    78,    88,    81,    -1,    15,   116,
      73,    88,    74,    81,    -1,    13,    73,    88,    74,    15,
     116,    81,    -1,    16,    82,    83,    14,    83,    73,    88,
      74,    81,    -1,    16,    82,    97,    17,    82,    73,    88,
      74,    81,    -1,    11,   116,    73,    88,    74,    81,    -1,
      11,   116,    73,    88,    74,    12,    73,    88,    74,    81,
      -1,    24,   116,    73,    88,    74,    -1,    81,    -1,    73,
     116,    74,    81,    -1,   119,    -1,   124,    -1,    95,    -1,
      18,   116,    81,    -1,    19,    81,    -1,    20,    81,    -1,
      98,    82,    97,    -1,    98,    82,    97,    60,   116,    -1,
      98,    82,    97,    60,   120,    -1,    27,    -1,    28,    -1,
      29,    -1,    30,    -1,    31,    -1,    32,    -1,    33,    -1,
      34,    -1,    35,    -1,    36,    -1,    37,    -1,    38,    -1,
      39,    -1,    82,    -1,    -1,    78,    96,    -1,     4,    -1,
       3,    -1,    60,    -1,    61,    -1,    62,    -1,    63,    -1,
      64,    -1,    65,    -1,    66,    -1,    67,    -1,    68,    -1,
      69,    -1,    70,    -1,   101,    -1,   101,    79,   116,    78,
     100,    -1,   102,    -1,   101,    53,   102,    -1,   103,    -1,
     102,    52,   103,    -1,   104,    -1,   103,    48,   104,    -1,
     105,    -1,   104,    49,   105,    -1,   106,    -1,   105,    47,
     106,    -1,   107,    -1,   106,    54,   107,    -1,   106,    55,
     107,    -1,   108,    -1,   107,    56,   108,    -1,   107,    57,
     108,    -1,   107,    58,   108,    -1,   107,    59,   108,    -1,
     109,    -1,   108,    50,   109,    -1,   108,    51,   109,    -1,
     110,    -1,   109,    40,   110,    -1,   109,    41,   110,    -1,
     111,    -1,   110,    42,   111,    -1,   110,    43,   111,    -1,
     110,    44,   111,    -1,   113,    -1,   112,   111,    -1,     9,
     111,    -1,    47,    -1,    42,    -1,    40,    -1,    41,    -1,
      45,    -1,    46,    -1,   114,    -1,   113,    71,    72,    -1,
     113,    71,   117,    72,    -1,   113,    75,   116,    76,    -1,
     113,    80,    82,    -1,   113,    10,    96,    -1,   115,    -1,
      82,    -1,    71,   116,    72,    -1,    83,    -1,    84,    -1,
      85,    -1,    21,    -1,    22,    -1,    23,    -1,   117,    -1,
     116,    77,   117,    -1,   100,    -1,   111,    99,   117,    -1,
      -1,     8,    -1,   118,     5,    82,    71,   121,    72,    97,
      73,    88,    74,    81,    -1,    73,    71,   121,    72,    97,
      17,    88,    74,    -1,   121,    77,   123,    -1,   123,    -1,
      -1,     6,    -1,    -1,   122,    82,    97,    -1,     7,    82,
      73,   125,    74,    81,    -1,   125,    81,   126,    -1,   126,
      -1,    -1,    82,    78,    96,    -1
};

/* YYRLINE[YYN] -- source line where rule number YYN was defined.  */
static const yytype_uint8 yyrline[] =
{
       0,    30,    30,    33,    34,    35,    36,    37,    38,    41,
      42,    45,    46,    47,    48,    50,    51,    52,    55,    56,
      59,    60,    61,    64,    65,    66,    69,    70,    71,    74,
      75,    76,    77,    78,    79,    80,    81,    82,    83,    84,
      85,    86,    87,    90,    91,    94,    95,    98,    99,   100,
     101,   102,   103,   104,   105,   106,   107,   108,   111,   112,
     115,   116,   119,   120,   123,   124,   127,   128,   131,   132,
     135,   136,   137,   140,   141,   142,   143,   144,   147,   148,
     149,   152,   153,   154,   157,   158,   159,   160,   163,   164,
     165,   168,   169,   170,   171,   172,   173,   176,   177,   178,
     179,   180,   181,   184,   185,   186,   189,   190,   191,   192,
     193,   194,   197,   198,   201,   202,   205,   206,   209,   212,
     215,   216,   219,   220,   223,   224,   227,   230,   231,   234,
     235
};
#endif

#if YYDEBUG || YYERROR_VERBOSE || YYTOKEN_TABLE
/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "$end", "error", "$undefined", "\"let\"", "CONST", "FN", "REF",
  "STRUCT", "ASYNC", "AWAIT", "AS", "IF", "ELSE", "DO", "TO", "WHILE",
  "FOR", "IN", "RETURN", "BREAK", "CONTINUE", "TRUE", "FALSE", "NIL",
  "SWITCH", "CASE", "DEFAULT", "I8", "I16", "I32", "I64", "U8", "U16",
  "U32", "U64", "F32", "F64", "BOOL", "BYTE", "STR", "ADD", "SUB", "MUL",
  "DIV", "MOD", "BIT_INV", "NOT", "AND", "OR", "XOR", "SHL", "SHR",
  "LOGIC_AND", "LOGIC_OR", "EQ", "NE", "LT", "GT", "LE", "GE", "ASSIGN",
  "ADD_ASSIGN", "SUB_ASSIGN", "MUL_ASSIGN", "DIV_ASSIGN", "MOD_ASSIGN",
  "BIT_AND_ASSIGN", "BIT_OR_ASSIGN", "BIT_XOR_ASSIGN", "BIT_SHL_ASSIGN",
  "BIT_SHR_ASSIGN", "OPEN_PAREN", "CLOSE_PAREN", "OPEN_BRACE",
  "CLOSE_BRACE", "OPEN_BRACKET", "CLOSE_BRACKET", "COMMA", "COLON",
  "QUESTION_MARK", "DOT", "EOL", "IDENTIFIER", "INT", "DOUBLE", "STRING",
  "$accept", "program", "statement", "labeled_statement",
  "iteration_statement", "selection_statement", "expression_statement",
  "declaration_statement", "jump_statement", "variable_declaration",
  "type", "type_declaration", "type_qualifier", "assignment_operator",
  "conditional_expression", "logical_or_expression",
  "logical_and_expression", "inclusive_or_expression",
  "exclusive_or_expression", "and_expression", "equality_expression",
  "relational_expression", "shift_expression", "additive_expression",
  "multiplicative_expression", "unary_expression", "unary_operator",
  "postfix_expression", "primary_expression", "literal", "expression",
  "assignment_expression", "fn_qualifier", "fn_declaration",
  "lambda_declaration", "parameters", "parameter_qualifier", "parameter",
  "struct_declaration", "struct_fields", "struct_field", 0
};
#endif

# ifdef YYPRINT
/* YYTOKNUM[YYLEX-NUM] -- Internal token number corresponding to
   token YYLEX-NUM.  */
static const yytype_uint16 yytoknum[] =
{
       0,   256,   257,   258,   259,   260,   261,   262,   263,   264,
     265,   266,   267,   268,   269,   270,   271,   272,   273,   274,
     275,   276,   277,   278,   279,   280,   281,   282,   283,   284,
     285,   286,   287,   288,   289,   290,   291,   292,   293,   294,
     295,   296,   297,   298,   299,   300,   301,   302,   303,   304,
     305,   306,   307,   308,   309,   310,   311,   312,   313,   314,
     315,   316,   317,   318,   319,   320,   321,   322,   323,   324,
     325,   326,   327,   328,   329,   330,   331,   332,   333,   334,
     335,   336,   337,   338,   339,   340
};
# endif

/* YYR1[YYN] -- Symbol number of symbol that rule YYN derives.  */
static const yytype_uint8 yyr1[] =
{
       0,    86,    87,    88,    88,    88,    88,    88,    88,    89,
      89,    90,    90,    90,    90,    91,    91,    91,    92,    92,
      93,    93,    93,    94,    94,    94,    95,    95,    95,    96,
      96,    96,    96,    96,    96,    96,    96,    96,    96,    96,
      96,    96,    96,    97,    97,    98,    98,    99,    99,    99,
      99,    99,    99,    99,    99,    99,    99,    99,   100,   100,
     101,   101,   102,   102,   103,   103,   104,   104,   105,   105,
     106,   106,   106,   107,   107,   107,   107,   107,   108,   108,
     108,   109,   109,   109,   110,   110,   110,   110,   111,   111,
     111,   112,   112,   112,   112,   112,   112,   113,   113,   113,
     113,   113,   113,   114,   114,   114,   115,   115,   115,   115,
     115,   115,   116,   116,   117,   117,   118,   118,   119,   120,
     121,   121,   122,   122,   123,   123,   124,   125,   125,   126,
     126
};

/* YYR2[YYN] -- Number of symbols composing right hand side of rule YYN.  */
static const yytype_uint8 yyr2[] =
{
       0,     2,     1,     1,     1,     1,     1,     1,     1,     5,
       4,     6,     7,     9,     9,     6,    10,     5,     1,     4,
       1,     1,     1,     3,     2,     2,     3,     5,     5,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     0,     2,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     5,
       1,     3,     1,     3,     1,     3,     1,     3,     1,     3,
       1,     3,     3,     1,     3,     3,     3,     3,     1,     3,
       3,     1,     3,     3,     1,     3,     3,     3,     1,     2,
       2,     1,     1,     1,     1,     1,     1,     1,     3,     4,
       4,     3,     3,     1,     1,     3,     1,     1,     1,     1,
       1,     1,     1,     3,     1,     3,     0,     1,    11,     8,
       3,     1,     0,     1,     0,     3,     6,     3,     1,     0,
       3
};

/* YYDEFACT[STATE-NAME] -- Default rule to reduce with in state
   STATE-NUM when YYTABLE doesn't specify something else to do.  Zero
   means the default is an error.  */
static const yytype_uint8 yydefact[] =
{
     116,    46,    45,     0,   117,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,    18,     0,     2,     5,
       6,     4,     3,     7,     8,    22,     0,     0,    20,    21,
       0,     0,   109,   110,   111,    93,    94,    92,    95,    96,
      91,     0,   104,   106,   107,   108,   114,    58,    60,    62,
      64,    66,    68,    70,    73,    78,    81,    84,     0,    88,
      97,   103,     0,   112,   116,     0,    43,     0,    24,    25,
       0,     0,   116,     0,     1,    43,     0,   129,    90,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    47,
      48,    49,    50,    51,    52,    53,    54,    55,    56,    57,
       0,    89,     0,     0,     0,     0,   116,     0,     0,   116,
       0,     0,     0,    23,   116,   116,     0,     0,    26,     0,
       0,     0,   128,   105,    61,    84,     0,    63,    65,    67,
      69,    71,    72,    74,    75,    76,    77,    79,    80,    82,
      83,    85,    86,    87,   115,    29,    30,    31,    32,    33,
      34,    35,    36,    37,    38,    39,    40,    41,    42,   102,
      98,     0,     0,   101,     0,   113,     0,     0,    44,     0,
       0,     0,     0,    10,    19,     0,   124,     0,     0,   129,
       0,    99,   100,     0,     0,     0,     0,     0,    17,     9,
       0,    27,    28,   123,     0,     0,   121,   130,   126,   127,
      59,     0,    15,     0,    11,   116,   116,   124,    43,   124,
      43,   116,    12,     0,     0,     0,     0,   120,   125,     0,
       0,     0,    43,   116,     0,    13,    14,     0,     0,    16,
     116,     0,     0,   118,   119
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
      -1,    17,    18,    19,    20,    21,    22,    23,    24,    25,
     169,   122,    26,   110,    46,    47,    48,    49,    50,    51,
      52,    53,    54,    55,    56,   135,    58,    59,    60,    61,
      62,    63,    27,    28,   202,   204,   205,   206,    29,   131,
     132
};

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
#define YYPACT_NINF -100
static const yytype_int16 yypact[] =
{
       9,  -100,  -100,   -67,  -100,   155,   -33,   155,   -20,   155,
     -27,   -24,   155,    42,   -19,   155,  -100,    85,  -100,  -100,
    -100,  -100,  -100,  -100,  -100,  -100,     5,    84,  -100,  -100,
      24,   155,  -100,  -100,  -100,  -100,  -100,  -100,  -100,  -100,
    -100,   155,  -100,  -100,  -100,  -100,  -100,   -34,    49,    99,
     104,    60,   -23,    57,    33,    55,    37,   143,   155,    31,
    -100,  -100,   -17,  -100,     9,    -2,   -40,    -9,  -100,  -100,
       1,    78,     9,   -26,  -100,    87,    90,    91,  -100,   -28,
     155,   155,   155,   155,   155,   155,   155,   155,   155,   155,
     155,   155,   155,   155,   155,   155,   155,   155,   155,  -100,
    -100,  -100,  -100,  -100,  -100,  -100,  -100,  -100,  -100,  -100,
     155,  -100,   101,    77,   155,    92,     9,   155,   105,     9,
     101,   170,   168,  -100,     9,     9,   113,   117,   139,   144,
     136,   -51,  -100,  -100,    49,  -100,    26,    99,   104,    60,
     -23,    57,    57,    33,    33,    33,    33,    55,    55,    37,
      37,  -100,  -100,  -100,  -100,  -100,  -100,  -100,  -100,  -100,
    -100,  -100,  -100,  -100,  -100,  -100,  -100,  -100,  -100,  -100,
    -100,   148,    44,  -100,   142,  -100,   203,   147,  -100,   140,
     145,   150,   141,  -100,  -100,   146,    12,   101,   151,    91,
     155,  -100,  -100,   -11,   155,   152,   161,   162,  -100,  -100,
     154,   159,  -100,  -100,   -25,   160,  -100,  -100,  -100,  -100,
    -100,   171,  -100,    -8,  -100,     9,     9,    12,    87,    12,
      87,     9,  -100,   167,   169,   -22,   172,  -100,  -100,   173,
     165,   174,    87,     9,   175,  -100,  -100,   231,   176,  -100,
       9,   177,   178,  -100,  -100
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -100,  -100,   -58,  -100,  -100,  -100,  -100,  -100,  -100,  -100,
     -99,   -66,  -100,  -100,    61,  -100,   179,   180,   166,   181,
     182,    59,    53,    58,    76,    -5,  -100,  -100,  -100,   240,
      -4,   -71,  -100,  -100,  -100,    40,  -100,    35,  -100,  -100,
      71
};

/* YYTABLE[YYPACT[STATE-NUM]].  What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule which
   number is the opposite.  If zero, do what YYDEFACT says.
   If YYTABLE_NINF, syntax error.  */
#define YYTABLE_NINF -123
static const yytype_int16 yytable[] =
{
      57,   211,    57,    65,    57,    67,   118,    57,    70,   128,
      57,    73,     1,     2,   126,    30,     3,     4,   203,    80,
       5,   178,     6,   188,     7,     8,    78,     9,    10,    11,
     189,    86,    87,    12,    13,    14,    57,    79,   120,   154,
      64,   112,   171,   121,   133,    81,   175,   218,   127,   117,
     232,   117,   219,   111,    68,   219,   116,    69,   174,    72,
     117,   177,    66,    32,    33,    34,   181,   182,   117,   117,
     212,   119,   123,   222,   124,   117,    57,   136,   117,    96,
      97,    98,    15,    92,    93,    74,    31,    75,   207,    76,
      16,   151,   152,   153,  -122,    94,    95,    77,    32,    33,
      34,    82,   113,   117,   190,    57,   114,    85,    57,    57,
     172,   115,    57,    88,    89,    90,    91,    35,    36,    37,
     192,   117,    38,    39,    40,    43,    44,    45,   155,   156,
     157,   158,   159,   160,   161,   162,   163,   164,   165,   166,
     167,   143,   144,   145,   146,   141,   142,    83,    41,   170,
     147,   148,   226,    84,   228,    31,   125,   223,   224,    42,
      43,    44,    45,   229,    31,   120,   237,    32,    33,    34,
     149,   150,   129,   130,   173,   238,    32,    33,    34,   176,
      57,   201,   242,   168,   179,   180,    35,    36,    37,    57,
     213,    38,    39,    40,   183,    35,    36,    37,   184,   185,
      38,    39,    40,    99,   100,   101,   102,   103,   104,   105,
     106,   107,   108,   109,   187,   186,   193,    41,   194,   200,
     191,   195,   199,   196,   198,   217,    41,   197,    42,    43,
      44,    45,   208,   214,   215,   216,   117,    42,    43,    44,
      45,   230,   220,   231,   221,   233,   235,   234,   240,   138,
     241,   210,   244,    71,   227,   236,   239,   225,   243,   134,
     209,     0,   137,     0,     0,   139,     0,   140
};

static const yytype_int16 yycheck[] =
{
       5,    12,     7,     7,     9,     9,    64,    12,    12,    75,
      15,    15,     3,     4,    72,    82,     7,     8,     6,    53,
      11,   120,    13,    74,    15,    16,    31,    18,    19,    20,
      81,    54,    55,    24,    25,    26,    41,    41,    78,   110,
      73,    10,   113,    83,    72,    79,   117,    72,    74,    77,
      72,    77,    77,    58,    81,    77,    73,    81,   116,    78,
      77,   119,    82,    21,    22,    23,   124,   125,    77,    77,
      81,    73,    81,    81,    73,    77,    81,    81,    77,    42,
      43,    44,    73,    50,    51,     0,     9,    82,   187,     5,
      81,    96,    97,    98,    82,    40,    41,    73,    21,    22,
      23,    52,    71,    77,    78,   110,    75,    47,   113,   114,
     114,    80,   117,    56,    57,    58,    59,    40,    41,    42,
      76,    77,    45,    46,    47,    83,    84,    85,    27,    28,
      29,    30,    31,    32,    33,    34,    35,    36,    37,    38,
      39,    88,    89,    90,    91,    86,    87,    48,    71,    72,
      92,    93,   218,    49,   220,     9,    78,   215,   216,    82,
      83,    84,    85,   221,     9,    78,   232,    21,    22,    23,
      94,    95,    82,    82,    82,   233,    21,    22,    23,    74,
     185,   185,   240,    82,    14,    17,    40,    41,    42,   194,
     194,    45,    46,    47,    81,    40,    41,    42,    81,    60,
      45,    46,    47,    60,    61,    62,    63,    64,    65,    66,
      67,    68,    69,    70,    78,    71,    74,    71,    15,    73,
      72,    74,    81,    83,    74,    71,    71,    82,    82,    83,
      84,    85,    81,    81,    73,    73,    77,    82,    83,    84,
      85,    74,    82,    74,    73,    73,    81,    74,    17,    83,
      74,   190,    74,    13,   219,    81,    81,   217,    81,    80,
     189,    -1,    82,    -1,    -1,    84,    -1,    85
};

/* YYSTOS[STATE-NUM] -- The (internal number of the) accessing
   symbol of state STATE-NUM.  */
static const yytype_uint8 yystos[] =
{
       0,     3,     4,     7,     8,    11,    13,    15,    16,    18,
      19,    20,    24,    25,    26,    73,    81,    87,    88,    89,
      90,    91,    92,    93,    94,    95,    98,   118,   119,   124,
      82,     9,    21,    22,    23,    40,    41,    42,    45,    46,
      47,    71,    82,    83,    84,    85,   100,   101,   102,   103,
     104,   105,   106,   107,   108,   109,   110,   111,   112,   113,
     114,   115,   116,   117,    73,   116,    82,   116,    81,    81,
     116,   115,    78,   116,     0,    82,     5,    73,   111,   116,
      53,    79,    52,    48,    49,    47,    54,    55,    56,    57,
      58,    59,    50,    51,    40,    41,    42,    43,    44,    60,
      61,    62,    63,    64,    65,    66,    67,    68,    69,    70,
      99,   111,    10,    71,    75,    80,    73,    77,    88,    73,
      78,    83,    97,    81,    73,    78,    88,    74,    97,    82,
      82,   125,   126,    72,   102,   111,   116,   103,   104,   105,
     106,   107,   107,   108,   108,   108,   108,   109,   109,   110,
     110,   111,   111,   111,   117,    27,    28,    29,    30,    31,
      32,    33,    34,    35,    36,    37,    38,    39,    82,    96,
      72,   117,   116,    82,    88,   117,    74,    88,    96,    14,
      17,    88,    88,    81,    81,    60,    71,    78,    74,    81,
      78,    72,    76,    74,    15,    74,    83,    82,    74,    81,
      73,   116,   120,     6,   121,   122,   123,    96,    81,   126,
     100,    12,    81,   116,    81,    73,    73,    71,    72,    77,
      82,    73,    81,    88,    88,   121,    97,   123,    97,    88,
      74,    74,    72,    73,    74,    81,    81,    97,    88,    81,
      17,    74,    88,    81,    74
};

#define yyerrok		(yyerrstatus = 0)
#define yyclearin	(yychar = YYEMPTY)
#define YYEMPTY		(-2)
#define YYEOF		0

#define YYACCEPT	goto yyacceptlab
#define YYABORT		goto yyabortlab
#define YYERROR		goto yyerrorlab


/* Like YYERROR except do call yyerror.  This remains here temporarily
   to ease the transition to the new meaning of YYERROR, for GCC.
   Once GCC version 2 has supplanted version 1, this can go.  */

#define YYFAIL		goto yyerrlab

#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)					\
do								\
  if (yychar == YYEMPTY && yylen == 1)				\
    {								\
      yychar = (Token);						\
      yylval = (Value);						\
      yytoken = YYTRANSLATE (yychar);				\
      YYPOPSTACK (1);						\
      goto yybackup;						\
    }								\
  else								\
    {								\
      yyerror (YY_("syntax error: cannot back up")); \
      YYERROR;							\
    }								\
while (YYID (0))


#define YYTERROR	1
#define YYERRCODE	256


/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

#define YYRHSLOC(Rhs, K) ((Rhs)[K])
#ifndef YYLLOC_DEFAULT
# define YYLLOC_DEFAULT(Current, Rhs, N)				\
    do									\
      if (YYID (N))                                                    \
	{								\
	  (Current).first_line   = YYRHSLOC (Rhs, 1).first_line;	\
	  (Current).first_column = YYRHSLOC (Rhs, 1).first_column;	\
	  (Current).last_line    = YYRHSLOC (Rhs, N).last_line;		\
	  (Current).last_column  = YYRHSLOC (Rhs, N).last_column;	\
	}								\
      else								\
	{								\
	  (Current).first_line   = (Current).last_line   =		\
	    YYRHSLOC (Rhs, 0).last_line;				\
	  (Current).first_column = (Current).last_column =		\
	    YYRHSLOC (Rhs, 0).last_column;				\
	}								\
    while (YYID (0))
#endif


/* YY_LOCATION_PRINT -- Print the location on the stream.
   This macro was not mandated originally: define only if we know
   we won't break user code: when these are the locations we know.  */

#ifndef YY_LOCATION_PRINT
# if defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL
#  define YY_LOCATION_PRINT(File, Loc)			\
     fprintf (File, "%d.%d-%d.%d",			\
	      (Loc).first_line, (Loc).first_column,	\
	      (Loc).last_line,  (Loc).last_column)
# else
#  define YY_LOCATION_PRINT(File, Loc) ((void) 0)
# endif
#endif


/* YYLEX -- calling `yylex' with the right arguments.  */

#ifdef YYLEX_PARAM
# define YYLEX yylex (YYLEX_PARAM)
#else
# define YYLEX yylex ()
#endif

/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)			\
do {						\
  if (yydebug)					\
    YYFPRINTF Args;				\
} while (YYID (0))

# define YY_SYMBOL_PRINT(Title, Type, Value, Location)			  \
do {									  \
  if (yydebug)								  \
    {									  \
      YYFPRINTF (stderr, "%s ", Title);					  \
      yy_symbol_print (stderr,						  \
		  Type, Value); \
      YYFPRINTF (stderr, "\n");						  \
    }									  \
} while (YYID (0))


/*--------------------------------.
| Print this symbol on YYOUTPUT.  |
`--------------------------------*/

/*ARGSUSED*/
#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static void
yy_symbol_value_print (FILE *yyoutput, int yytype, YYSTYPE const * const yyvaluep)
#else
static void
yy_symbol_value_print (yyoutput, yytype, yyvaluep)
    FILE *yyoutput;
    int yytype;
    YYSTYPE const * const yyvaluep;
#endif
{
  if (!yyvaluep)
    return;
# ifdef YYPRINT
  if (yytype < YYNTOKENS)
    YYPRINT (yyoutput, yytoknum[yytype], *yyvaluep);
# else
  YYUSE (yyoutput);
# endif
  switch (yytype)
    {
      default:
	break;
    }
}


/*--------------------------------.
| Print this symbol on YYOUTPUT.  |
`--------------------------------*/

#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static void
yy_symbol_print (FILE *yyoutput, int yytype, YYSTYPE const * const yyvaluep)
#else
static void
yy_symbol_print (yyoutput, yytype, yyvaluep)
    FILE *yyoutput;
    int yytype;
    YYSTYPE const * const yyvaluep;
#endif
{
  if (yytype < YYNTOKENS)
    YYFPRINTF (yyoutput, "token %s (", yytname[yytype]);
  else
    YYFPRINTF (yyoutput, "nterm %s (", yytname[yytype]);

  yy_symbol_value_print (yyoutput, yytype, yyvaluep);
  YYFPRINTF (yyoutput, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static void
yy_stack_print (yytype_int16 *bottom, yytype_int16 *top)
#else
static void
yy_stack_print (bottom, top)
    yytype_int16 *bottom;
    yytype_int16 *top;
#endif
{
  YYFPRINTF (stderr, "Stack now");
  for (; bottom <= top; ++bottom)
    YYFPRINTF (stderr, " %d", *bottom);
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)				\
do {								\
  if (yydebug)							\
    yy_stack_print ((Bottom), (Top));				\
} while (YYID (0))


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static void
yy_reduce_print (YYSTYPE *yyvsp, int yyrule)
#else
static void
yy_reduce_print (yyvsp, yyrule)
    YYSTYPE *yyvsp;
    int yyrule;
#endif
{
  int yynrhs = yyr2[yyrule];
  int yyi;
  unsigned long int yylno = yyrline[yyrule];
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %lu):\n",
	     yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      fprintf (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr, yyrhs[yyprhs[yyrule] + yyi],
		       &(yyvsp[(yyi + 1) - (yynrhs)])
		       		       );
      fprintf (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)		\
do {					\
  if (yydebug)				\
    yy_reduce_print (yyvsp, Rule); \
} while (YYID (0))

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args)
# define YY_SYMBOL_PRINT(Title, Type, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef	YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif



#if YYERROR_VERBOSE

# ifndef yystrlen
#  if defined __GLIBC__ && defined _STRING_H
#   define yystrlen strlen
#  else
/* Return the length of YYSTR.  */
#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static YYSIZE_T
yystrlen (const char *yystr)
#else
static YYSIZE_T
yystrlen (yystr)
    const char *yystr;
#endif
{
  YYSIZE_T yylen;
  for (yylen = 0; yystr[yylen]; yylen++)
    continue;
  return yylen;
}
#  endif
# endif

# ifndef yystpcpy
#  if defined __GLIBC__ && defined _STRING_H && defined _GNU_SOURCE
#   define yystpcpy stpcpy
#  else
/* Copy YYSRC to YYDEST, returning the address of the terminating '\0' in
   YYDEST.  */
#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static char *
yystpcpy (char *yydest, const char *yysrc)
#else
static char *
yystpcpy (yydest, yysrc)
    char *yydest;
    const char *yysrc;
#endif
{
  char *yyd = yydest;
  const char *yys = yysrc;

  while ((*yyd++ = *yys++) != '\0')
    continue;

  return yyd - 1;
}
#  endif
# endif

# ifndef yytnamerr
/* Copy to YYRES the contents of YYSTR after stripping away unnecessary
   quotes and backslashes, so that it's suitable for yyerror.  The
   heuristic is that double-quoting is unnecessary unless the string
   contains an apostrophe, a comma, or backslash (other than
   backslash-backslash).  YYSTR is taken from yytname.  If YYRES is
   null, do not copy; instead, return the length of what the result
   would have been.  */
static YYSIZE_T
yytnamerr (char *yyres, const char *yystr)
{
  if (*yystr == '"')
    {
      YYSIZE_T yyn = 0;
      char const *yyp = yystr;

      for (;;)
	switch (*++yyp)
	  {
	  case '\'':
	  case ',':
	    goto do_not_strip_quotes;

	  case '\\':
	    if (*++yyp != '\\')
	      goto do_not_strip_quotes;
	    /* Fall through.  */
	  default:
	    if (yyres)
	      yyres[yyn] = *yyp;
	    yyn++;
	    break;

	  case '"':
	    if (yyres)
	      yyres[yyn] = '\0';
	    return yyn;
	  }
    do_not_strip_quotes: ;
    }

  if (! yyres)
    return yystrlen (yystr);

  return yystpcpy (yyres, yystr) - yyres;
}
# endif

/* Copy into YYRESULT an error message about the unexpected token
   YYCHAR while in state YYSTATE.  Return the number of bytes copied,
   including the terminating null byte.  If YYRESULT is null, do not
   copy anything; just return the number of bytes that would be
   copied.  As a special case, return 0 if an ordinary "syntax error"
   message will do.  Return YYSIZE_MAXIMUM if overflow occurs during
   size calculation.  */
static YYSIZE_T
yysyntax_error (char *yyresult, int yystate, int yychar)
{
  int yyn = yypact[yystate];

  if (! (YYPACT_NINF < yyn && yyn <= YYLAST))
    return 0;
  else
    {
      int yytype = YYTRANSLATE (yychar);
      YYSIZE_T yysize0 = yytnamerr (0, yytname[yytype]);
      YYSIZE_T yysize = yysize0;
      YYSIZE_T yysize1;
      int yysize_overflow = 0;
      enum { YYERROR_VERBOSE_ARGS_MAXIMUM = 5 };
      char const *yyarg[YYERROR_VERBOSE_ARGS_MAXIMUM];
      int yyx;

# if 0
      /* This is so xgettext sees the translatable formats that are
	 constructed on the fly.  */
      YY_("syntax error, unexpected %s");
      YY_("syntax error, unexpected %s, expecting %s");
      YY_("syntax error, unexpected %s, expecting %s or %s");
      YY_("syntax error, unexpected %s, expecting %s or %s or %s");
      YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s");
# endif
      char *yyfmt;
      char const *yyf;
      static char const yyunexpected[] = "syntax error, unexpected %s";
      static char const yyexpecting[] = ", expecting %s";
      static char const yyor[] = " or %s";
      char yyformat[sizeof yyunexpected
		    + sizeof yyexpecting - 1
		    + ((YYERROR_VERBOSE_ARGS_MAXIMUM - 2)
		       * (sizeof yyor - 1))];
      char const *yyprefix = yyexpecting;

      /* Start YYX at -YYN if negative to avoid negative indexes in
	 YYCHECK.  */
      int yyxbegin = yyn < 0 ? -yyn : 0;

      /* Stay within bounds of both yycheck and yytname.  */
      int yychecklim = YYLAST - yyn + 1;
      int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
      int yycount = 1;

      yyarg[0] = yytname[yytype];
      yyfmt = yystpcpy (yyformat, yyunexpected);

      for (yyx = yyxbegin; yyx < yyxend; ++yyx)
	if (yycheck[yyx + yyn] == yyx && yyx != YYTERROR)
	  {
	    if (yycount == YYERROR_VERBOSE_ARGS_MAXIMUM)
	      {
		yycount = 1;
		yysize = yysize0;
		yyformat[sizeof yyunexpected - 1] = '\0';
		break;
	      }
	    yyarg[yycount++] = yytname[yyx];
	    yysize1 = yysize + yytnamerr (0, yytname[yyx]);
	    yysize_overflow |= (yysize1 < yysize);
	    yysize = yysize1;
	    yyfmt = yystpcpy (yyfmt, yyprefix);
	    yyprefix = yyor;
	  }

      yyf = YY_(yyformat);
      yysize1 = yysize + yystrlen (yyf);
      yysize_overflow |= (yysize1 < yysize);
      yysize = yysize1;

      if (yysize_overflow)
	return YYSIZE_MAXIMUM;

      if (yyresult)
	{
	  /* Avoid sprintf, as that infringes on the user's name space.
	     Don't have undefined behavior even if the translation
	     produced a string with the wrong number of "%s"s.  */
	  char *yyp = yyresult;
	  int yyi = 0;
	  while ((*yyp = *yyf) != '\0')
	    {
	      if (*yyp == '%' && yyf[1] == 's' && yyi < yycount)
		{
		  yyp += yytnamerr (yyp, yyarg[yyi++]);
		  yyf += 2;
		}
	      else
		{
		  yyp++;
		  yyf++;
		}
	    }
	}
      return yysize;
    }
}
#endif /* YYERROR_VERBOSE */


/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

/*ARGSUSED*/
#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static void
yydestruct (const char *yymsg, int yytype, YYSTYPE *yyvaluep)
#else
static void
yydestruct (yymsg, yytype, yyvaluep)
    const char *yymsg;
    int yytype;
    YYSTYPE *yyvaluep;
#endif
{
  YYUSE (yyvaluep);

  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yytype, yyvaluep, yylocationp);

  switch (yytype)
    {

      default:
	break;
    }
}


/* Prevent warnings from -Wmissing-prototypes.  */

#ifdef YYPARSE_PARAM
#if defined __STDC__ || defined __cplusplus
int yyparse (void *YYPARSE_PARAM);
#else
int yyparse ();
#endif
#else /* ! YYPARSE_PARAM */
#if defined __STDC__ || defined __cplusplus
int yyparse (void);
#else
int yyparse ();
#endif
#endif /* ! YYPARSE_PARAM */



/* The look-ahead symbol.  */
int yychar;

/* The semantic value of the look-ahead symbol.  */
YYSTYPE yylval;

/* Number of syntax errors so far.  */
int yynerrs;



/*----------.
| yyparse.  |
`----------*/

#ifdef YYPARSE_PARAM
#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
int
yyparse (void *YYPARSE_PARAM)
#else
int
yyparse (YYPARSE_PARAM)
    void *YYPARSE_PARAM;
#endif
#else /* ! YYPARSE_PARAM */
#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
int
yyparse (void)
#else
int
yyparse ()

#endif
#endif
{
  
  int yystate;
  int yyn;
  int yyresult;
  /* Number of tokens to shift before error messages enabled.  */
  int yyerrstatus;
  /* Look-ahead token as an internal (translated) token number.  */
  int yytoken = 0;
#if YYERROR_VERBOSE
  /* Buffer for error messages, and its allocated size.  */
  char yymsgbuf[128];
  char *yymsg = yymsgbuf;
  YYSIZE_T yymsg_alloc = sizeof yymsgbuf;
#endif

  /* Three stacks and their tools:
     `yyss': related to states,
     `yyvs': related to semantic values,
     `yyls': related to locations.

     Refer to the stacks thru separate pointers, to allow yyoverflow
     to reallocate them elsewhere.  */

  /* The state stack.  */
  yytype_int16 yyssa[YYINITDEPTH];
  yytype_int16 *yyss = yyssa;
  yytype_int16 *yyssp;

  /* The semantic value stack.  */
  YYSTYPE yyvsa[YYINITDEPTH];
  YYSTYPE *yyvs = yyvsa;
  YYSTYPE *yyvsp;



#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  YYSIZE_T yystacksize = YYINITDEPTH;

  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;


  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yystate = 0;
  yyerrstatus = 0;
  yynerrs = 0;
  yychar = YYEMPTY;		/* Cause a token to be read.  */

  /* Initialize stack pointers.
     Waste one element of value and location stack
     so that they stay on the same level as the state stack.
     The wasted elements are never initialized.  */

  yyssp = yyss;
  yyvsp = yyvs;

  goto yysetstate;

/*------------------------------------------------------------.
| yynewstate -- Push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
 yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;

 yysetstate:
  *yyssp = yystate;

  if (yyss + yystacksize - 1 <= yyssp)
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYSIZE_T yysize = yyssp - yyss + 1;

#ifdef yyoverflow
      {
	/* Give user a chance to reallocate the stack.  Use copies of
	   these so that the &'s don't force the real ones into
	   memory.  */
	YYSTYPE *yyvs1 = yyvs;
	yytype_int16 *yyss1 = yyss;


	/* Each stack pointer address is followed by the size of the
	   data in use in that stack, in bytes.  This used to be a
	   conditional around just the two extra args, but that might
	   be undefined if yyoverflow is a macro.  */
	yyoverflow (YY_("memory exhausted"),
		    &yyss1, yysize * sizeof (*yyssp),
		    &yyvs1, yysize * sizeof (*yyvsp),

		    &yystacksize);

	yyss = yyss1;
	yyvs = yyvs1;
      }
#else /* no yyoverflow */
# ifndef YYSTACK_RELOCATE
      goto yyexhaustedlab;
# else
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
	goto yyexhaustedlab;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
	yystacksize = YYMAXDEPTH;

      {
	yytype_int16 *yyss1 = yyss;
	union yyalloc *yyptr =
	  (union yyalloc *) YYSTACK_ALLOC (YYSTACK_BYTES (yystacksize));
	if (! yyptr)
	  goto yyexhaustedlab;
	YYSTACK_RELOCATE (yyss);
	YYSTACK_RELOCATE (yyvs);

#  undef YYSTACK_RELOCATE
	if (yyss1 != yyssa)
	  YYSTACK_FREE (yyss1);
      }
# endif
#endif /* no yyoverflow */

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;


      YYDPRINTF ((stderr, "Stack size increased to %lu\n",
		  (unsigned long int) yystacksize));

      if (yyss + yystacksize - 1 <= yyssp)
	YYABORT;
    }

  YYDPRINTF ((stderr, "Entering state %d\n", yystate));

  goto yybackup;

/*-----------.
| yybackup.  |
`-----------*/
yybackup:

  /* Do appropriate processing given the current state.  Read a
     look-ahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to look-ahead token.  */
  yyn = yypact[yystate];
  if (yyn == YYPACT_NINF)
    goto yydefault;

  /* Not known => get a look-ahead token if don't already have one.  */

  /* YYCHAR is either YYEMPTY or YYEOF or a valid look-ahead symbol.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token: "));
      yychar = YYLEX;
    }

  if (yychar <= YYEOF)
    {
      yychar = yytoken = YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yyn == 0 || yyn == YYTABLE_NINF)
	goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  if (yyn == YYFINAL)
    YYACCEPT;

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the look-ahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);

  /* Discard the shifted token unless it is eof.  */
  if (yychar != YYEOF)
    yychar = YYEMPTY;

  yystate = yyn;
  *++yyvsp = yylval;

  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- Do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     `$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
      
/* Line 1267 of yacc.c.  */
#line 1736 "ns_lexer.tab.c"
      default: break;
    }
  YY_SYMBOL_PRINT ("-> $$ =", yyr1[yyn], &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);

  *++yyvsp = yyval;


  /* Now `shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */

  yyn = yyr1[yyn];

  yystate = yypgoto[yyn - YYNTOKENS] + *yyssp;
  if (0 <= yystate && yystate <= YYLAST && yycheck[yystate] == *yyssp)
    yystate = yytable[yystate];
  else
    yystate = yydefgoto[yyn - YYNTOKENS];

  goto yynewstate;


/*------------------------------------.
| yyerrlab -- here on detecting error |
`------------------------------------*/
yyerrlab:
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
#if ! YYERROR_VERBOSE
      yyerror (YY_("syntax error"));
#else
      {
	YYSIZE_T yysize = yysyntax_error (0, yystate, yychar);
	if (yymsg_alloc < yysize && yymsg_alloc < YYSTACK_ALLOC_MAXIMUM)
	  {
	    YYSIZE_T yyalloc = 2 * yysize;
	    if (! (yysize <= yyalloc && yyalloc <= YYSTACK_ALLOC_MAXIMUM))
	      yyalloc = YYSTACK_ALLOC_MAXIMUM;
	    if (yymsg != yymsgbuf)
	      YYSTACK_FREE (yymsg);
	    yymsg = (char *) YYSTACK_ALLOC (yyalloc);
	    if (yymsg)
	      yymsg_alloc = yyalloc;
	    else
	      {
		yymsg = yymsgbuf;
		yymsg_alloc = sizeof yymsgbuf;
	      }
	  }

	if (0 < yysize && yysize <= yymsg_alloc)
	  {
	    (void) yysyntax_error (yymsg, yystate, yychar);
	    yyerror (yymsg);
	  }
	else
	  {
	    yyerror (YY_("syntax error"));
	    if (yysize != 0)
	      goto yyexhaustedlab;
	  }
      }
#endif
    }



  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse look-ahead token after an
	 error, discard it.  */

      if (yychar <= YYEOF)
	{
	  /* Return failure if at end of input.  */
	  if (yychar == YYEOF)
	    YYABORT;
	}
      else
	{
	  yydestruct ("Error: discarding",
		      yytoken, &yylval);
	  yychar = YYEMPTY;
	}
    }

  /* Else will try to reuse look-ahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:

  /* Pacify compilers like GCC when the user code never invokes
     YYERROR and the label yyerrorlab therefore never appears in user
     code.  */
  if (/*CONSTCOND*/ 0)
     goto yyerrorlab;

  /* Do not reclaim the symbols of the rule which action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;	/* Each real token shifted decrements this.  */

  for (;;)
    {
      yyn = yypact[yystate];
      if (yyn != YYPACT_NINF)
	{
	  yyn += YYTERROR;
	  if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYTERROR)
	    {
	      yyn = yytable[yyn];
	      if (0 < yyn)
		break;
	    }
	}

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
	YYABORT;


      yydestruct ("Error: popping",
		  yystos[yystate], yyvsp);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  if (yyn == YYFINAL)
    YYACCEPT;

  *++yyvsp = yylval;


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", yystos[yyn], yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturn;

/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturn;

#ifndef yyoverflow
/*-------------------------------------------------.
| yyexhaustedlab -- memory exhaustion comes here.  |
`-------------------------------------------------*/
yyexhaustedlab:
  yyerror (YY_("memory exhausted"));
  yyresult = 2;
  /* Fall through.  */
#endif

yyreturn:
  if (yychar != YYEOF && yychar != YYEMPTY)
     yydestruct ("Cleanup: discarding lookahead",
		 yytoken, &yylval);
  /* Do not reclaim the symbols of the rule which action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
		  yystos[*yyssp], yyvsp);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif
#if YYERROR_VERBOSE
  if (yymsg != yymsgbuf)
    YYSTACK_FREE (yymsg);
#endif
  /* Make sure YYID is used.  */
  return YYID (yyresult);
}


#line 237 "ns_lexer.y"


void yyerror(const char *s) {
    fprintf(stderr, "Error: %s\n", s);
}

int main(void) {
    printf("Enter expressions:\n");
    yyparse();
    return 0;
}
