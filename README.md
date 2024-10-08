nano script language
--------------------

## syntax preview
[main.ns](sample/main.ns)
```ns
// file end with .ns extension

// this is a comment

// import a module
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
    return "[{p.x}, {p.y}]"
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

## design goal
- minimal syntax and keywords, easy to learn and use.
- interpretable and compilable.
- use explicit type system, not generic type.
- data oriented programming style.