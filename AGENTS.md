# AGENTS.md

- For runtime appearance changes, ask the user to check the running app visually.
- Keep the `ns` interpreter language-only. Do not link non-language runtime
  features such as UI, terminal, view, GPU, network, HTTP, or image helpers into
  `bin/ns`; expose them through `lib/*.ns` declarations backed by FFI-loaded
  dynamic libraries instead.
