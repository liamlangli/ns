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
