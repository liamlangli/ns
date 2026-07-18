Nano Script
-----------
[![Build](https://github.com/liamlangli/ns/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/liamlangli/ns/actions/workflows/build.yml)
[![Test](https://github.com/liamlangli/ns/actions/workflows/test.yml/badge.svg?branch=main)](https://github.com/liamlangli/ns/actions/workflows/test.yml)

> A minimal, data-oriented functional programming language.

## Play Nano Script in your browser
- Playground (GitHub Pages): https://liamlangli.github.io/ns/nscode/
- Landing page https://liamlangli.github.io/ns/
- NSCode project page in this repo: https://github.com/liamlangli/ns/tree/main/nscode

If you're new to Nano Script, start with the Playground URL above and run/edit the sample code directly in your browser.

## Run and build from VS Code

Open a `.ns` file and use the **Run Project** (play) or **Build Project** (tools)
button in the editor title bar. The same actions are available from the Command
Palette as **Nano Script: Run Project** and **Nano Script: Build Project**.

The extension finds the nearest parent `ns.mod` and runs `ns run` or `ns build`
from that project directory in VS Code's integrated task terminal. If the file
is not part of a manifest project, it passes the active file directly. Set
`nslang.executablePath` when the `ns` executable is not available on `PATH`.

## Design Goal
- Minimal syntax and keywords for ease of learning and use.
- Supports both interpretation and compilation.
- Utilizes an explicit type system, avoiding generic types.
- Follows a data-oriented programming approach.

## Syntax Preview
```swift
// this is a comment

// use modules
use math
use std

// define a variable
let a = 1 // default int literal type is i32; default float literal type is f32
let pi: f64 = 3.141592653
let hello: str = "hello world" // string literal

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
type number = f32

// union type: a value may be any one of the member types
type num = i32 | f64
let some_num: num = 1       // holds an i32
let other_num: num = 3.14   // holds an f64
let n = some_num as i32     // narrow to a member with `as`

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

## Generate an IDE project

Run `ns project [path]` in or below a directory containing `ns.mod`. On Darwin,
it creates `bin/<safe-name>.xcodeproj` with macOS, iOS, and visionOS app targets.
For app manifests with `icon = "path/to/image.png"`, the generated Xcode project
contains the resized macOS/iOS AppIcon set and a visionOS icon stack.
On Windows, it creates a Visual Studio 2022 x64 NMake solution at
`bin/<safe-name>.sln`. App projects embed the language-only runtime plus `std`
and shader support, with pure-language modules such as `simd`; native UI, GPU,
terminal, network/HTTP, and external FFI modules are not included. Library
manifests generate build/test utility projects, and other hosts report that
project generation is unsupported.

Generated support files are refreshed on later runs, but IDE project files and
local configuration overrides are preserved. See [the module guide](doc/nsm.md)
for output paths, target behavior, and limitations.

## Build Options
- `NS_DEBUG`: Debug mode.
- `NS_JIT`: Enable Just-In-Time compiler.
