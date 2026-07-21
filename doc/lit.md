# Literal constants

`lit` declares a compile-time constant at module or local scope. It uses the
same optional type label and inference rules as `let`, but always requires an
initializer.

```ns
lit base = 40
lit answer: i64 = (base + 2) as i64

fn main() bool {
    lit expected = answer
    return expected == 42l
}
```

A `lit` initializer may contain literal values, preceding `lit` values, enum
members, casts, parentheses, and pure unary or binary operators. Calls,
ordinary `let`/`ref` bindings, arrays, dictionaries, sets, structs, and blocks
are runtime values and cannot initialize a `lit`.

Literal constants support numbers, `bool`, `str`, and enum values. They cannot
be reassigned or mutated through an index/member expression. Scalar literals
remain immediate values in the interpreter instead of occupying writable stack
storage. Native compilation evaluates global numeric, boolean, and enum `lit`
expressions once and seeds each function with the resulting SSA constant.
