# Union Types

A **union type** is a named type that admits a value of any one of several
member types. It is declared with `type` and `|`:

```swift
type num = i32 | f64
```

A value of any member type is assignable to the union:

```swift
let a: num = 7      // ok, holds an i32
let b: num = 3.5    // ok, holds an f64
let c: num = true   // error: bool is not a member of `num`
```

Unions are also valid as function parameter and return types:

```swift
fn identity(x: num) num {
    return x
}
```

## Narrowing with `as`

Nano Script is explicitly typed, so a union value cannot be used directly where
a specific member type is required. Use the `as` cast to narrow a union to one
of its members:

```swift
let a: num = 7
let n = a as i32        // n : i32
assert (a as i32) == 7
```

The interpreter tags every value with its concrete type, so narrowing a union to
the member type it actually holds returns that value unchanged. Narrowing
between two numeric members (e.g. an `i32`-holding `num` cast to `f64`) performs
a numeric conversion, mirroring ordinary numeric casts.

A member value can also be widened to a union explicitly with `as`:

```swift
let v = 1 as num        // v : num
```

## Members

Members may be primitive types, named types/aliases, or structs:

```swift
struct vec2 { x: f64, y: f64 }
type shape = i32 | vec2

let id: shape = 5
let pt: shape = vec2 { x: 1.0, y: 2.0 }
assert (pt as vec2).x == 1.0
```

A union slot reserves enough storage for its largest member.

## Implementation notes

- The parser builds a union from a chain of type labels separated by `|`
  (`src/ns_ast_stmt.c`, `ns_parse_typedef_stmt`).
- A union is represented by the value type `NS_TYPE_UNION` and an
  `NS_SYMBOL_UNION` symbol holding the list of member types
  (`include/ns_type.h`, `include/ns_vm.h`).
- `ns_type_match` treats a value as assignable to a union when it matches any
  member; `ns_type_size` sizes a union to its largest member
  (`src/ns_vm_parse.c`).
- At runtime, union-typed variables and return values keep the concrete
  member's type tag so values round-trip (`src/ns_vm_eval.c`).

## Current limitations

- Member type labels are simple type names, arrays, or structs. Function-type
  members (`(i32) -> i32`) inside a union are not supported.
- Narrowing is performed with an explicit `as` cast; there is no `match`-based
  destructuring yet.
