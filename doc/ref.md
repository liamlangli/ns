ns use ref and const to mamange the memory and the lifetime of the data.
```ns
let a = 1.0
let b = ref a
let c = a
let d = ref b // same as let d = b
b = 2.0 // b is a ref, so it can be changed
print("{a} {b} {c} {d}") // 2.0 2.0 1.0 2.0
d = 3.0
print("{a} {b} {c} {d}") // 3.0 3.0 1.0 3.0

let hi = ref "hello"    // error: ref can only be used with mutable data & generic type
let hi2 = ref hi        // same as above

// ref can be defined in before a fn, mean is a external defined fn
ref fn add(a: i32, b: i32): i32

// ref can define ref arguments and return ref value
fn upper(s: ref str): ref str {
    for i in 0 to s.len() {
        s[i] = s[i].to_upper()
    }
    return s
}
```

## Assigning to a ref binding

A `ref` binding can be declared empty and given something real later, which is
what a handle a native module hands back looks like before that module has been
created:

```ns
let db: ref storage_db = nil

fn open_world() {
    db = storage_db_open("world")   // rebinds: db now aliases that handle
}
```

What an assignment to a `ref` means depends on what is on the right:

- **Another ref rebinds it.** The binding stops aliasing whatever it aliased
  and aliases the new referent instead. Nothing is written through.
- **A plain value is written through** to the current referent, which is the
  scalar case at the top of this file: `b = 2.0` sets `a`, it does not point
  `b` somewhere else.

The distinction matters most for an opaque handle. A `ref storage_db` returned
by a native module is the pointer itself; there is nothing meaningful to write
through into, and copying its contents over the binding would leave reads
looking plausible while the pointer was gone.
