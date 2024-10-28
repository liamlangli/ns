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