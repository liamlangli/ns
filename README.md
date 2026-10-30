Nano Script
-----------
[![Build](https://github.com/liamlangli/ns/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/liamlangli/ns/actions/workflows/build.yml)
[![Test](https://github.com/liamlangli/ns/actions/workflows/test.yml/badge.svg?branch=main)](https://github.com/liamlangli/ns/actions/workflows/test.yml)

> A minimal, data-oriented functional programming language.

## Releases

The Build workflow validates optimized release builds for macOS aarch64, Linux
amd64, and Windows amd64. Pushing a tag whose name starts with `v` publishes
those three SDK archives and their `SHA256SUMS` file to a GitHub Release
automatically.

## Syntax
```swift
// this is a comment

// use modules
use std
use simd

// define a variable
let a = 1 // default int literal type is i32; default float literal type is f32
let pi: f64 = 3.141592653
let hello: str = "hello world" // string literal

// define compile-time literal constants (global or local)
lit answer = 6 * 7
lit precise: f64 = answer as f64

// operators (see doc/operators.md for the full set)
let ready = true
let blocked = !ready              // unary logical not
let go = ready && !(a == 0)       // negate a compound expression
let delta = -(a + 1)              // arithmetic negation of a group
let low = a & ~1                  // bitwise not: clear the low bit

// define a function
fn add(a: f64, b: f64): f64 {
    return a + b
}

// define a struct
struct point {
   x: f32,
   y: f32
}

// define a nominal enum; the underlying integer type defaults to i32
enum os_platform: u8 {
    unknown = 0,
    macos,
}

let platform = os_platform.macos
let raw: u8 = platform
let restored = raw as os_platform

// operator override
fn ops(+)(lhs: point, rhs: point): point {
    return point(lhs.x + rhs.x, lhs.y + rhs.y)
}

// to string fn
fn to_str(p: point): str {
    return `{p.x}, {p.y}`  // str formatter
}

// same-name fns: later defs are stored as name_FirstArgType
fn length(v: float3) f32 { return sqrt((v.x * v.x + v.y * v.y + v.z * v.z) as f64) as f32 }
fn length(v: float2) f32 { return sqrt((v.x * v.x + v.y * v.y) as f64) as f32 }

let point_a = point(0, 0)
let point_b = point(1, 1)
print(`{point_a + point_b}\n`) // use the operator override, prints 1.00, 1.00

// typealias
type number = f32

// union type: a value may be any one of the member types
type num = i32 | f64
let some_num: num = 1       // holds an i32
let other_num: num = 3.14d  // holds an f64
let n = some_num as i32     // narrow to a member with `as`

// block[closure]
// define a block or fn type
type op_fn = (i32, i32) -> i32

fn sub(a: i32, b: i32): i32 {
    return a - b
}

fn do_op(a: i32, b: i32, op: op_fn): i32 {
    return op(a, b)
}

// define an asynchronous function
async fn twice(n: i32): i32 {
    return n * 2
}

fn main() {
    // define a block
    let add_op: op_fn = { a, b in
        return a + b
    }
    let ret_add = do_op(1, 2, add_op)
    let ret_sub = do_op(2, 1, sub)
    print(`1 + 2 = {ret_add}\n`)
    print(`2 - 1 = {ret_sub}\n`)

    let pending = twice(21)
    let value = await pending
    print(`answer = {value}\n`)
}
```

## Design Goal
- Minimal syntax and keywords for ease of learning and use.
- Supports both interpretation and compilation, plus a register bytecode
  interpreter (`ns run --cpu`, `.nsc` images) for platforms without JIT and for
  hot updates; see [doc/cpu.md](doc/cpu.md).
- Utilizes an explicit type system, avoiding generic types.
- Follows a data-oriented programming approach.

Platform-specific GPU texture compression and size limits in `ns.mod`: [texture cooking](doc/texture_cook.md).
