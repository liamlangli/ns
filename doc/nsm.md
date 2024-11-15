nanoscript module manager
-------------------------

> `nsm.ns` - nanoscript module manager config file
```nanoscript
import nsm

let module = NSModule{
    name: "example",
    version: "0.1.0",
    description: "example module",
    type: "app", // or "lib"
    source: "src", 
    dependencies: [
        {name: "ref_lib", version: "0.1.0"}
    ]
}
```

The `nsm` module is a module manager for nanoscript. It allows you to create, install, and manage modules for your nanoscript projects. The `nsm` module is a core module and is included with the nanoscript runtime.

### Usage

| Command                          | Description                          |
|----------------------------------|--------------------------------------|
| `nsm create example`             | Create a new app module              |
| `nsm create lib_example --lib`   | Create a new lib module              |
| `nsm build`                      | Build the current module             |
| `nsm run`                        | Run the app module                   |
| `nsm lint`                       | Lint the module                      |
| `nsm add [mod_name]`             | Add a module to the current module   |
| `nsm remove [mod_name]`          | Remove a module from the current module |
