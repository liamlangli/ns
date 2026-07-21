# Container Types

Nano Script supports explicit container type labels for arrays, dictionaries,
and sets:

```ns
[T]          // array
[K: V]       // dict
set[T]       // set
```

Constructors take a capacity/count hint:

```ns
let bytes = [u8](1024)
let ages = [str: i32](64)
let names = set[str](64)
```

Dictionary values can be written and read with index syntax:

```ns
ages["amy"] = 32
assert ages["amy"] == 32
```

The current dictionary runtime is fixed-capacity and open-addressed. It accepts
numeric and string keys. Reading a missing key is an error; assigning to a
missing key inserts it.

Membership operations are the builtins `has`, `insert`, and `remove`, shared
by dicts and sets over the same hash-table backing:

```ns
let ages = [str: i32](8)
ages["amy"] = 32
assert has(ages, "amy")
assert remove(ages, "amy")   // true when the key was present
assert !has(ages, "amy")

let tags = set[str](64)
assert insert(tags, "red")   // true when newly inserted
assert !insert(tags, "red")  // already a member
assert has(tags, "red")
assert remove(tags, "red")
```

`insert` applies to sets (dicts insert by index assignment); `has` and
`remove` accept both. Removal tombstones the slot so later entries in the
probe chain stay reachable, and insertion reuses tombstones, so a
fixed-capacity table supports remove/insert cycles. `.len` reports the live
entry count. A user fn named `has`, `insert`, or `remove` shadows the
builtin.
