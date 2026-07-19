# ns-in-ns: a self-hosted Nano Script front-end

This directory contains a **clean-room reimplementation of the Nano Script
front-end, written in Nano Script itself** (self-hosting), plus a unit-test
harness — also written in Nano Script — that exercises it on the `ns`
interpreter built from the repository root.

It is developed in stages:

- [x] **Stage 1 — Lexer** (`lexer.ns`): complete, **94 assertions passing**.
- [x] **Stage 2 — Parser** (`parser.ns`): recursive-descent expression/statement
      parser producing a flat AST, **57 assertions passing**.
- [ ] Stage 3 — Type checker in ns.
- [ ] Stage 4 — Tree-walking interpreter in ns.

## Scope-based project compilation (no Makefile)

Earlier the modules here had to be "linked" by **concatenating** sources in a
Makefile, because Nano Script's `use` only imported modules from the
interpreter's ref path — there was no local module import. That is no longer
needed: `ns` now **compiles a project by scope**.

When you `ns run` or `ns test` a file, the interpreter resolves every
`use <name>` against the project's source tree first. If a local `<name>.ns`
exists it is compiled in; otherwise the `use` is treated as an external library
import (e.g. `use std`). So the modules in this directory simply `use` one
another, and `ns` links them itself.

```sh
# from the repository root, after `make` has produced bin/ns:

bin/ns run  ns/demo_main.ns     # tokenize a sample and print the token stream
bin/ns test ns/                 # discover & run ns/test/*_test.ns
bin/ns test ns/test/lexer_test.ns  # run one test entry (exit code = failures)

# from inside the project directory, `ns run` needs no file argument: it
# uses ns.mod in that directory (or main.ns when there is no manifest).
cd ns && ../bin/ns run          # runs the manifest's entry (demo_main.ns)
../bin/ns test                  # runs every test/*_test.ns; no manifest list
```

`ns test <file>` runs a test entry and uses the i32 returned by its `main`
(the harness's failure count) as the process exit status. `ns test` and
`ns test <project-dir>` discover every `test/*_test.ns` beside `ns.mod`.
An explicit non-project directory is scanned directly. Every suite runs in a
fresh VM, gets a per-suite PASS/FAIL line, and contributes to the final status.
No test list is needed in `ns.mod`.

## Layout

```
ns/
  lexer.ns          the tokenizer        (struct Tok, TK_*/c_* consts; no main)
  parser.ns         the parser           (struct Node, NK_* consts; uses lexer)
  harness.ns        soft-assert unit-test harness (expect / expect_eq / ...)
  demo_main.ns      run entry: tokenize a sample and dump the tokens
  test/
    lexer_test.ns   test entry for the lexer  (use std/lexer/harness)
    parser_test.ns  test entry for the parser (use std/lexer/parser/harness)
```

Every entry file declares its own imports (`use std`, `use lexer`, …); there
are no generated/concatenated artifacts and no Makefile.

## Runtime fixes that made self-hosting possible

Self-hosting means the reimplementation must *run* on the current `ns`
interpreter, which surfaced genuine, general correctness bugs in the C core.
All of the following are fixed in the repo's `src/` (verified, no regressions
to the JSON test or the `add`/`fib` samples):

| Area | Change |
|------|--------|
| `src/ns_vm_eval.c`, `src/ns_vm_parse.c` | **Bitwise operators** `&` `\|` `^` for integers (were unimplemented). |
| `src/ns_vm_eval.c` | **Shift** `<<` `>>` and **modulo** `%` fixed (had been flagged stack-resident instead of immediate). |
| `src/ns_vm_eval.c`, `src/ns_vm_parse.c` | **String concatenation** `+` and **string equality** `==` / `!=`. |
| `src/ns_vm_lib.c`, `lib/std.ns` | New builtins **`substr(s, start, len)`** and **`unescape(s)`**. |
| `src/ns_vm_eval.c`, `src/ns_vm_parse.c`, `include/ns_vm.h` | **`break` / `continue`** for `loop` and `for`. |
| `src/ns_vm_parse.c` | **Array-typed parameters** keep their array flag so they can be indexed. |
| AST files | **Member-access fix**: a field lived in `ns_ast_t.next`, clobbered by arg linking; now `member_expr.right`. |
| `src/ns_type.c` | **`ns_str_to_i32`** now handles a leading sign (`-1`/`+1`) and `0x` hex; before, `"-1"` evaluated to `-29`. |
| `src/ns_vm_parse.c` | **`ns_type_size`** returns pointer size for *any* array value. A scalar array like `[i32]` was sized to its element (4 bytes), so an 8-byte array handle was truncated and clobbered by the next allocation — corrupting arrays written in a callee and read back in the caller. Struct arrays only worked by luck (stride ≥ 8). |
| `src/ns.c` | **`run` / `test` subcommands** and the **scope-based project linker** described above. |

## Language notes / design decisions

The reimplementation preserves Nano Script semantics while working within what
the interpreter currently supports:

- **Tokens and AST nodes are flat records.** A token is a `(kind, start, len,
  line)` span into the source (`struct Tok`); an AST node is a
  `(kind, a, b, c, tok, next)` record (`struct Node`) that refers to its
  children by their index in a flat node array. Both avoid string/heap churn.
- **`&&` / `||` are not short-circuit** in the host, so every source read goes
  through `lx_at` / `p_kind`, which return safe sentinels past end-of-input.
- **Token and node kinds are integer constants exposed as `fn`s** (`TK_LET()`,
  `NK_BINARY()`), keeping this bootstrap front-end independent of the enum
  feature it tokenizes.
- **Characters are byte values**, named via `c_*` helpers (`c_nl()` = 10, …).
- **`main()` is auto-invoked** by the runtime; test entries `return
  test_summary()` so the failure count becomes the exit status.

## What the front-end supports

**Lexer** — keywords (`use let fn struct enum type return if else for in to loop do
break continue assert as ops async await true false nil`), primitive type
keywords, identifiers, integer/hex/float literals, `'…'` / `"…"` strings and
`` `…` `` format strings, line (`//`) and block (`/* */`) comments, newlines as
`EOL` tokens with line tracking, and the full operator/punctuation set.

**Parser** — expressions with C-matching precedence (`||/&&` < `==/!=/===` <
`</>/<=/>=` < `&/|/^` < `<</>>` < `+/-` < `*//%` < unary < call < primary),
parenthesised grouping, unary `!` `~` `-`, function calls with argument lists,
and the `let` / `return` / expression statements, parsed into the flat `Node`
AST.
