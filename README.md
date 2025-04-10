Nano Script
-----------
> A minimal, data-oriented functional programming language.

## Design Goal
- Minimal syntax and keywords for ease of learning and use.
- Supports both interpretation and compilation.
- Utilizes an explicit type system, avoiding generic types.
- Follows a data-oriented programming approach.

## Syntax Preview
```ns
// this is a comment

// import modules
import math
import std

// define a variable
let a = 1 // default number int type is i32, for float number is f64
let pi: f64 = 3.141592653
let hello: str = "hello world" // string literal

// define a function
fn add(a: f64, b: f64): f64 {
    return a + b
}

// define a struct
struct Point {
   x: f32,
   y: f32
}

// operator override
fn ops(+)(lhs: Point, rhs: Point): Point {
    return Point(lhs.x + rhs.x, lhs.y + rhs.y)
}

// to string fn
fn to_str(p: Point): str {
    return `{p.x}, {p.y}`  // str formatter
}

let point_a = Point(0, 0)
let point_b = Point(1, 1)
print(a + b) // use override add fn, expect result [1, 1]

// typealias
type number = f64

// block[closure]
// define a block or fn type
type op_fn = (i32, i32) -> i32

// define a block
let add_op: op_fn = { a, b in 
    return a + b
}

fn sub(a: i32, b: i32): i32 {
    return a - b
}

fn do_op(a: i32, b: i32, op: op_fn): i32 {
    return op(a, b)
}

fn main() {
    let ret_add = do_op(1, 2, add_op)
    let ret_sub = do_op(2, 1, sub)
    print(`1 + 2 = {ret_add}`)
    print(`2 - 1 = {ret_sub}`)
}

// define a asynchronous function
async fn download(url: str, on_data: (data: Data) to void): Data {
    let d = await do_download(url)
    on_data(d)
    return d
}
```

## Components
- `ns`: The Nano Script compiler and interpreter that can compiles and executes Nano Script source code.
- `ns_lsp`: A language server for Nano Script providing features like code completion, hover, and diagnostics.
- `ns_debug`: A debug adapter for Nano Script offering debugging features such as breakpoints, stepping, and variable inspection.

## Build Options
- `NS_DEBUG`: Debug mode.
- `NS_IR`: Generate llvm ir file.
- `NS_JIT`: Enable Just-In-Time compiler.
