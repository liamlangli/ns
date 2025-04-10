nanoscript block
------------------
# Block Expression Grammer
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

# Block Expression Parse
- encounter block expr.
    - create a block scope.
    - before block parsing, align actual block arg type.
        - for fn call with, align block type with fn arg type definition.
        - for var def statement, require explicit type label and align type.
    - check each variable usage.
        - arg, local var, global var usage skip.
        - outter scope var usage capture it, and save to block symbol refs array.

# Block Expression Eval
- encounter block expr.
    - for fn call, alloc stack mem for block ctx, then call block fn and parse ctx as first argument
    - for any other kind of assign op, if not target type marked as a ref fn type, then do heap alloc
        and return a heap block ref.