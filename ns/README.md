# ns-in-ns: a self-hosted Nano Script front-end

This directory contains a **clean-room reimplementation of the Nano Script
front-end, written in Nano Script itself** (self-hosting), plus a unit-test
harness — also written in Nano Script — that exercises it on the existing `ns`
interpreter built from the repository root.

It is developed in stages. **Stage 1 — the tokenizer/lexer — is complete and
fully tested (78 assertions passing).** Later stages (parser → type checker →
tree-walking interpreter) build on the same approach.

## Why a runtime had to be extended first

Self-hosting means the reimplementation must *run* on the current `ns`
interpreter. A tokenizer is almost entirely string scanning and looping, and
the interpreter was missing or mis-implementing several primitives needed for
that. So the first task was to make the C interpreter capable of hosting its
own front-end. The following changes were made in the repo's `src/` (all
verified, with no regressions to the existing JSON tests or samples):

| Area | Change |
|------|--------|
| `src/ns_vm_eval.c`, `src/ns_vm_parse.c` | **Bitwise operators** `&` `\|` `^` added for integers (were unimplemented). |
| `src/ns_vm_eval.c` | **Shift** `<<` `>>` and **modulo** `%` fixed — they produced garbage because results were flagged stack-resident instead of immediate. |
| `src/ns_vm_eval.c`, `src/ns_vm_parse.c` | **String concatenation** `+` and **string equality** `==` / `!=`. |
| `src/ns_vm_lib.c` | `print()` now resolves stack-resident strings (e.g. concatenation results) correctly. |
| `src/ns_vm_lib.c`, `lib/std.ns` | New builtins **`substr(s, start, len)`** and **`unescape(s)`**. |
| `src/ns_vm_eval.c`, `src/ns_vm_parse.c`, `include/ns_vm.h` | **`break` / `continue`** statements implemented for `loop` and `for`. |
| `src/ns_vm_parse.c` | **Array-typed parameters** (`a: [i32]`) keep their array flag, so they can be indexed. |
| `src/ns_fmt.c` | `print` tolerates literal `{`/`}` in already-formatted text instead of aborting. |
| AST (`include/ns_ast.h`, `ns_ast_expr.c`, `ns_vm_parse.c`, `ns_vm_eval.c`, `ns_ast_print.c`) | **Member-access fix**: a field was stored in `ns_ast_t.next`, which argument-list linking clobbered, so `f(p.a, p.b)` aliased both args to `p.b`. The field now lives in a dedicated `member_expr.right`. |

These are genuine, general correctness fixes — they benefit any Nano Script
program, not just this project.

## Language notes / design decisions

The reimplementation is a **clean redesign** that preserves Nano Script
semantics while working within what the interpreter currently supports:

- **Tokens are spans, not copied text.** Each token is a `(kind, start, len,
  line)` record into the source string (`struct Tok`), mirroring the C
  tokenizer and avoiding string allocation. `substr` materializes text only
  when needed.
- **`&&` / `||` are not short-circuit** in the host, so every source read goes
  through `lx_at`, which returns `0` past end-of-input. This makes all loop
  conditions safe regardless of evaluation order.
- **Token kinds are integer constants exposed as `fn`s** (e.g. `TK_LET()`),
  since the language has no enums.
- **Characters are byte values.** ASCII codes are named via `c_*` helpers for
  readability (`c_nl()` = 10, etc.).
- **No local module imports.** The interpreter's `use` only loads modules from
  its lib path, so multi-file programs are "linked" by **concatenation** (see
  the Makefile): `use std` + `lexer.ns` + driver/tests. This is the one
  structural concession to the host; it keeps the lexer source DRY (shared by
  both the demo and the tests).
- **`main()` is auto-invoked** by the runtime, so drivers define `main` and do
  not call it.

## Layout

```
ns/
  lexer.ns            the tokenizer (functions + Tok struct; no main)
  demo_main.ns        a demo driver: tokenize a sample and print the tokens
  test/
    harness.ns        soft-assert unit-test harness (expect / expect_eq / ...)
    test_lexer.ns     the lexer test suite (main + summary)
  Makefile            concatenates sources and runs them on ../bin/ns
  bin/                generated, concatenated programs (gitignored)
```

## Building and running

First build the interpreter from the repository root:

```sh
make          # produces bin/ns (the io.so std lib may fail to build; unrelated)
```

Then, from this directory:

```sh
make test     # run the self-hosted lexer unit tests
make demo     # tokenize a sample function and print the token stream
```

`make test` prints one line per assertion and a final summary; it exits after
`ALL TESTS PASSED` / `SOME TESTS FAILED`.

## What the lexer supports

Keywords (`use let fn struct type return if else for in to loop do break
continue assert as ops async await true false nil`), primitive type keywords
(`i8`..`f64`, `bool`, `str`, `void`, `any`), identifiers, integer/hex/float
literals, `'...'` / `"..."` strings and `` `...` `` format strings, line (`//`)
and block (`/* */`) comments, newlines as `EOL` tokens with line tracking, and
the full operator/punctuation set (`+ - * / % << >> < > <= >= == != === && ||
& | ^ ! ~ = += -> , . : ( ) { } [ ]`).

## Status / next stages

- [x] **Stage 1 — Lexer**: complete, 78 assertions passing.
- [ ] Stage 2 — Parser (AST) in ns.
- [ ] Stage 3 — Type checker in ns.
- [ ] Stage 4 — Tree-walking interpreter in ns.
