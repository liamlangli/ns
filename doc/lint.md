# Lint

`ns lint` reports style findings for Nano Script sources and `ns lint_fix`
(also spelled `ns lint --fix`) rewrites the ones that have a mechanical fix.

```bash
ns lint                 # the project below the current directory
ns lint main.ns         # one file
ns lint src             # one directory, recursively
ns lint_fix             # rewrite the fixable findings in place
```

Without a path the current directory is linted. A directory that is a project
root follows the manifest's `source` and `exclude`; any other directory is
scanned as given. Test sources are ordinary sources here: they are linted too.
`bin/` is never linted.

Each finding prints as `file:line:column: severity rule: message`, `(fixable)`
marks what `ns lint_fix` would rewrite, and `(fixed)` what it did. `ns lint`
exits non-zero when any `error` severity finding remains, so it can gate CI;
warnings do not fail the run.

The linter works on the token stream rather than the AST, so a file that does
not parse is still checked and fixed. Comments and string literals are payload:
nothing inside them is reported or rewritten, including the expressions inside
a backtick interpolation.

## Rules

| Rule | Default | Fixable | Checks |
| --- | --- | --- | --- |
| `binary_op_space` | error | yes | A binary operator takes exactly one space on both sides: `a+b` becomes `a + b`. A prefix `-`/`+`/`!`/`~` is not a binary operator, and an operator that ends a line continues the expression, so only its left side is checked. |
| `comma_space` | error | yes | `,` binds to the token on its left and is followed by one space. |
| `colon_space` | error | yes | `:` binds to the token on its left and is followed by one space, in type labels, struct fields and named literals alike. |
| `trailing_space` | error | yes | No whitespace at end of line, and the file ends with exactly one newline. The file's own line ending (LF or CRLF) is kept. |
| `tab_indent` | error | yes | Indentation is spaces, never tabs. A tab is expanded to the next `indent` stop. |
| `struct_label` | warning | yes | A struct literal that labels every field in declaration order carries no information in its labels: `point { x: 0, y: 0 }` becomes `point { 0, 0 }`. A literal that reorders or omits fields keeps its labels. |
| `nested_name` | warning | no | A nested fn - a `{ arg, ... in ... }` block, and anything nested inside one - keeps its own names shorter than `nested_name_max`. Its arguments and its `let`/`lit`/`for` bindings are checked. Renaming is a semantic edit, so this one is reported and never rewritten. |

## Configuration

Rules are customized by the `[lint]` table of the project's `ns.mod`. Every
rule takes a severity of `"error"`, `"warn"` or `"off"`; the two scalar options
tune the rules that need a number. The nearest manifest above the linted path
supplies the settings, so a project owns its own style.

```toml
schema = "ns.mod/v1"
name = "example"
version = "0.1.0"
type = "app"
source = "."
entry = "main.ns"

[lint]
indent = 4              # spaces per indentation level
nested_name_max = 8     # longest name allowed inside a nested fn
binary_op_space = "error"
comma_space = "error"
colon_space = "error"
trailing_space = "error"
tab_indent = "error"
struct_label = "warn"
nested_name = "warn"
```

Omitted keys keep the defaults in the table above, so a manifest only needs the
lines it changes. An unknown key or an unknown severity warns and is otherwise
ignored, which keeps a newer manifest usable with an older `ns`.

A rule set to `"off"` is neither reported nor rewritten. That is the way to opt
out of a fix: `struct_label = "off"` keeps every label in place, and raising
`struct_label = "error"` instead makes ordered labels fail `ns lint`.
