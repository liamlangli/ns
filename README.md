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
let a = 1 // default number type is f64
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
    return `{p.x}, {p.y}`  // str format
}

let point_a = Point(0, 0)
let point_b = Point(1, 1)
print(a + b) // use override add fn, expect result [1, 1]

// define a type
type Shape {
    area(): f64
}

type number = f64

struct Circle {
    center: Point,
    radius: f64
}

// define area for circle
fn area(c: Circle): f64 {
    return pi * c.radius * c.radius
}

fn calculate_area(s: Shape): f64 {
    return s.area()
}

let c = Circle(Point(0, 0), 1)
print(c.area())
print(calculate_area(c)) // duck type

// define a asynchronous function
async fn download(url: str): Data {
    return await do_download(url)
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
