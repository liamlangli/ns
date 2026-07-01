Nano Script expression operators, from lowest to highest binding. Binary
operators are left-associative; the prefix unary operators bind tighter than
any binary operator and may be stacked (`!!a`, `- -a`).

```ns
// logical (result: bool)
a || b            // or
a && b            // and

// equality / relational (result: bool)
a == b   a != b   // also === / !== spellings
a < b    a <= b   a > b   a >= b   // numbers, and strings (lexicographic)

// bitwise (integers) and shifts
a & b    a | b    a ^ b
a << b   a >> b

// arithmetic
a + b    a - b            // + also concatenates strings
a * b    a / b    a % b   // % is fmod for floats

// prefix unary
-a        // arithmetic negation (numbers)
!a        // logical not (bool -> bool)
~a        // bitwise not / complement (integers -> same integer type)
ref a     // take a reference (see ref.md)
```

Unary operators accept a bare operand, a call/index/member operand, or a
parenthesised sub-expression, so all of these parse:

```ns
let ok = !ready()          // call operand
let hidden = !panel.open   // member operand
let outside = !(x == y)    // parenthesised compound operand
let neg = -(a + b)          // arithmetic negation of a group
let flip = !!enabled       // double negation
```

`!` requires a `bool` operand and yields a `bool`. Booleans are read through
their storage, so negating a variable (`!flag`) and negating a literal
comparison (`!(1 == 2)`) behave identically.

`~` requires an integer operand (`i8`..`u64`) and yields the same type; it is
rejected on `bool` and float operands. It composes with the other bitwise
operators, e.g. `x & ~mask` clears the bits of `mask`.
