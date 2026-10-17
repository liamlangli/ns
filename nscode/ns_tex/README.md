# ns_tex

Apple texture cooking written in Nano Script. `ns_tex` reads the project's
`ns.mod`, selects its `[cook.ios]` or `[cook.macos]` rule, invokes Xcode's
`textureconverter`, validates the KTX1 output and packs NSTX v1 files for
`gpu_texture_load_cooked` and UI atlases. It needs ns, its `os`/`io` modules,
and Xcode; Python, Pillow and a separate astcenc installation are unnecessary.

```sh
make ns_tex                  # bin/ns_tex, alongside bin/ns
make install-ns_tex          # install launcher and .ns sources into the SDK
bin/ns_tex /absolute/project ios /absolute/project/bin/resources
bin/ns_tex /absolute/project macosx /absolute/project/bin/mac-resources
```

`make` on macOS and `make install` also include the tool. Installed sources
live in `share/nscode/ns_tex`; projects call the `ns_tex` executable beside
their `ns` executable. `NS_EXECUTABLE` may select the ns runtime explicitly.
The source launcher `nscode/ns_tex/ns_tex` also works without installation.

The third argument is an already copied resource directory. The tool replaces
selected packaged images with `<original path>.nstex` only after successful
encoding, and never rewrites source assets. Outputs must be separate from
source directories; output symlink directories are rejected. Cached results
live in the input project's `bin/.ns-tex/`, keyed by source content, manifest,
tool sources and encoder identity.

Supported platforms: `ios`, `iphoneos`, `iphonesimulator`, `macos`, `macosx`.
Supported formats: `astc_4x4` and `astc_6x6`, optionally `srgb = true`.
The other rule keys are `max_size`, `include`, `exclude` and `quality`.
`max_size` defaults to 512 for iOS and 0 (unlimited) for macOS; the encoder
may choose a smaller size below this limit. Alpha stays straight and original
atlas dimensions are preserved in the NSTX header.

The manifest reader supports one-line quoted string values and arrays with
`*`/`?` globs and trailing comments. It reads top-level `assets` and `source`;
without explicit assets it scans `assets` and `<source>/assets`. Missing
asset roots are skipped. Unknown cook options, unsupported formats, malformed
arrays, 16-bit data PNGs and invalid encoder output fail the build. Mesh/VAT
data and CPU-read images should be excluded from compression.

Tests:

```sh
bin/ns test nscode/ns_tex
bin/ns test --cpu nscode/ns_tex
sh nscode/ns_tex/test/integration.sh
```

The integration test uses Xcode's real encoder and skips when it is unavailable.
On macOS, `ns cook --platform ios|macos` delegates to this tool, as do Apple
asset packaging during `ns run`, `ns build`, and `ns patch --cook-platform`.
Other platforms retain the existing cooker.
