nano script language
--------------------

## syntax preview
[main.ns](sample/main.ns)
```rust
// this is a comment

// file end with .ns extension

// define a variable
let a = 1
let pi: f64 = 3.141592653
let hello = "hello world"

// define a function
fn add(a, b) {
    return a + b
}

// define a struct
struct Point {
   x: f32
   y: f32
}

// define a asynchronous function 
async fn download(url) {
    return await do_download(url)
}

// define a type
type Shape {
    area(): f64
}
```

## design goal
- supports both compilation and interpretation.
- supports type but no generic type.
- data and procedure-oriented.
- [`TBD`] use fixed size stack rather dynamic heap allocation and release