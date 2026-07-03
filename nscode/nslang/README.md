# nslang — Nano Script for VS Code

Syntax highlighting for the **Nano Script** (`.ns`) language, powered by a
TextMate grammar that mirrors the reference lexer (`src/ns_token.c`).

## Features

- Highlighting for all ns keywords (`fn`, `let`, `struct`, `type`, `use`,
  `ref`, `ops`, `kernel`, `async`, `await`, `match`, `loop`, …) and primitive
  types (`i8`–`i64`, `u8`–`u64`, `f32`, `f64`, `bool`, `str`, `void`, `any`).
- String literals (`'…'`, `"…"`) and backtick format strings with `{expr}`
  interpolation highlighted as embedded expressions.
- Number literals including hex (`0x1f`), binary (`0b1010`), octal (`0o755`),
  floats, and the one-letter type suffixes (`1.5f`, `42i`, `7u`, `200b`).
- Function definition/call and struct/type name scopes.
- `//` line comments, bracket matching, auto-closing pairs, and indentation
  rules via the language configuration.
- Associates `ns.mod` module manifests with TOML highlighting.
- Ships the ns logo (`icons/ns.png` / `icons/ns.svg`, copied from
  `sample/ns.svg` and `sample/ns.png`) as the extension icon and the `.ns`
  file icon shown in editor tabs.

## Install

### From source (symlink)

```bash
ln -s "$(pwd)/nscode/nslang" ~/.vscode/extensions/nslang
```

Restart VS Code (or run the **Developer: Reload Window** command) and open any
`.ns` file.

### Packaged (`.vsix`)

```bash
npm install -g @vscode/vsce
cd nscode/nslang
vsce package
code --install-extension nslang-0.1.0.vsix
```

## Development

The grammar lives in `syntaxes/ns.tmLanguage.json`. Use the
**Developer: Inspect Editor Tokens and Scopes** command in VS Code to check
which scope a token resolves to. Keep the keyword and type lists in sync with
`src/ns_token.c` and `doc/token.md` when the language grows.
