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
missing key inserts it. Set syntax currently covers type labels and
construction, with membership/update helpers intended to use the same hash-table
backing.
