- as
- async
- await
- break
- const
- continue
- default
- do
- else
- enum
- f32
- f64
- fn
- for
- i16
- i32
- i64
- i8
- if
- in
- use
- kernel
- let
- nil
- ref
- str
- struct
- type
- u16
- u32
- u64
- u8
- loop

## utf8 source and strings

Source text — and the `str` type it produces — is utf8 by default; utf8 is
the global string encoding of ns. `ns_str` stores bytes (`len` is a byte
count) and the utf8 helpers in `ns_type.h` (`ns_utf8_decode`,
`ns_utf8_encode`, `ns_str_utf8_len`, `ns_str_utf8_valid`) work in codepoints
on top of that.

- Identifiers may contain any valid non-ascii codepoint alongside ascii
  letters, digits, `_`: `let 名字 = 1` binds the identifier `名字`.
- String literals (`"…"`, `'…'`) and format strings (`` `…` ``) carry their
  utf8 payload byte-for-byte. A backslash escapes the following byte, so
  `\"` (or the matching quote kind) and `\\` stay inside the literal;
  escapes are resolved later by `unescape`/formatting via `ns_str_unescape`.
- `\u{XXXX}` (hex codepoint, e.g. `\u{4F60}`) unescapes to the codepoint's
  utf8 bytes; a malformed `\u` escape keeps its spelling.
- A utf8 BOM is treated as whitespace; bytes that do not form well-formed
  utf8 (bad lead, truncated or overlong sequences, surrogates) tokenize as
  invalid tokens instead of being absorbed.
- `std.utf8_len(s)` counts codepoints, while `substr` and `s.len` stay
  byte-based.

## number literal suffix

A decimal number literal defaults to GPU-friendly 32-bit types: int literals
are `i32`, float literals are `f32`. A suffix picks another width:

- `12b` -> `i8`
- `12ub` -> `u8`
- `12s` -> `i16`
- `12us` -> `u16`
- `12u` -> `u32`
- `12l` -> `i64`
- `12ul` -> `u64`
- `1d`, `1.0d` -> `f64`
- `1h`, `1.0h` -> half float intent; CPU currently falls back to `f32`
- `1hb`, `1.0hb` -> brain float intent; CPU currently falls back to `f32`

The suffix only counts when it ends the number (`32b` is an `i8`, `32bit` is the
int `32` followed by the identifier `bit`). An unsuffixed literal also adopts
the number type expected by its context (a typed `let`, fn arg, struct field,
return slot, or the other side of a binary expr), so `let x: f32 = 1.5` needs
no suffix or cast.

When shader source is emitted, `h` literals are emitted as half literals/casts.
`hb` literals fall back to half when bfloat16 is not available in the shader
target and emit a warning.
