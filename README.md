nano script
-----------

> minimal script intepreter.
```typescript
// how to define a variable
let a = 1
let pi: f64 = 3.141592653
let str = 'hello world'

// how to define a function
fn add(a, b) {
    return a + b
}

// how to define a struct
struct Point {
   x: f32
   y: f32
}

// how to define a anonymous function
let swap = { (a, b) in
    a, b = b, a
}

// how to define a asynchronous function 
async fn download(url) {
    return await do_download(url)
}

// how to define a type
type Shape {
    area(): f64
}
```

