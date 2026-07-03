- as
- async
- await
- break
- const
- continue
- default
- do
- else
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

A decimal number literal defaults to a 64-bit type: int literals are `i64`,
float literals are `f64`. A one-letter tail picks a 32-bit (or byte) type
instead:

- `1.5f`, `3f` -> `f32`
- `42i` -> `i32`
- `7u` -> `u32`
- `200b` -> byte (`u8`)

The tail only counts when it ends the number (`32b` is a byte, `32bit` is the
int `32` followed by the identifier `bit`). An unsuffixed literal also adopts
the number type expected by its context (a typed `let`, fn arg, struct field,
return slot, or the other side of a binary expr), so `let x: f32 = 1.5` needs
no suffix or cast.
