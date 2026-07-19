# Enum types

`enum` defines a nominal type backed by a builtin integer. Definitions are
module-level, contain at least one member, and allow a trailing comma.

```ns
enum os_platform: u8 {
    unknown = 0,
    macos,
    linux = macos + 4,
    other,
}
```

The underlying type defaults to `i32`. It must be one of `i8`, `u8`, `i16`,
`u16`, `i32`, `u32`, `i64`, or `u64`; aliases and other types are not accepted.
The first automatic value is zero. Each later automatic value is one greater
than the preceding member, including after an explicit value.

Members are scoped to their enum and are accessed with a qualified name:

```ns
let platform = os_platform.macos
let raw: u8 = platform
let restored = raw as os_platform
```

Enums are nominal: values of different enum types are not interchangeable, and
integers do not implicitly convert to enums. An enum does implicitly lower to
its underlying integer. `integer as Enum` accepts any value in the underlying
type's range, even when no member has that value. Arithmetic, bitwise, and
relational operations lower enum operands to integers and return the normal
integer or `bool` result rather than an enum.

Explicit member values are compile-time integer expressions. They may use
integer literals, already declared members of the same enum, parentheses,
unary `-` and `~`, and binary `+`, `-`, `*`, `/`, `%`, `<<`, `>>`, `&`, `|`,
and `^`. Forward references, runtime values, calls, duplicate names, duplicate
values, division by zero, and values outside the underlying range are errors.

Formatting a declared value uses its qualified name, such as
`os_platform.macos`. A valid but unnamed value is formatted as
`os_platform(3)`.
