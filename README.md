nano script
-----------

> minimal script interpreter.
```ns
// define a variable
let a = 1
let pi: f64 = 3.141592653
let hello = 'hello world'

// define a function
fn add(a, b) {
    return a + b
}

// define a struct
struct Point {
   x: f32
   y: f32
}

// define a anonymous function
let swap = { (a, b) in
    a, b = b, a
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

