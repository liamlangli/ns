nanoscript block
------------------

```ns
// define a block or fn type
type op_fn = (i32, i32) -> i32

// define a block
let add: op_fn = { a, b in 
    return a + b
}

fn sub(a: i32, b: i32): i32 {
    return a - b
}

fn do_op(a: i32, b: i32, op: op_fn): i32 {
    return op(a, b)
}

fn main() {
    let ret_add = do_op(1, 2, add)
    let ret_sub = do_op(2, 1, sub)
    print(`1 + 2 = {ret_add}`)
    print(`2 - 1 = {ret_sub}`)
}
```