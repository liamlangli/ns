nano script language
--------------------

## syntax preview
[main.ns](sample/main.ns)
```ns
// this is a comment

// file end with .ns extension

// define a variable
let a = 1 // as f64
let pi: f64 = 3.141592653
let hello = "hello world" // string literal

// define a function
fn add(a, b) {
    return a + b
}

// define a struct
struct Point {
   x: f32
   y: f32
}

// operator override
fn ops(+)(lhs: Point, rhs: Point): Point {
    return Point(lhs.x + rhs.x, lhs.y + rhs.y)
}

// stringfy fn
fn to_str(p: Point): str {
    return "[{p.x}, {p.y}]"
}

let point_a = Point(0, 0)
let point_b = Point(1, 1)
print(a + b) // expect [1, 1]

// define a type
type Shape {
    area(): f64
}

type number = f64

struct Circle {
    center: Point
    radius: f64
}

// define area for circle
fn area(c: Circle): f64 {
    return math.PI * c.radius * c.radiuse
}

fn calculate_area(s: Shape): f64 {
    return s.area()
}

let c = Circle(Point(0, 0), 1)
print(c.area())
print(calculate_area(c)) // duck type

// define a asynchronous function 
async fn download(url: str) {
    return await do_download(url)
}
```

## design goal
- supports both compilation and interpretation.
- supports type but no generic type.
- data and procedure-oriented.
- [`TBD`] use fixed size stack rather dynamic heap allocation and release