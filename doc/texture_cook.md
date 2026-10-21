# Platform texture cooking

`ns build` app packaging and generated Apple Xcode targets can replace GPU
pictures with platform-specific compressed textures. Configure valid TOML tables
in `ns.mod` (use `[cook.ios]`, rather than `[[cook:ios]]`):

```toml
[cook.ios]
format = "astc_6x6"
max_size = 512
exclude = ["res/*_mesh.png", "res/*_vat.png", "res/fonts/*"]

[cook.macos]
format = "astc_4x4"
max_size = 0
exclude = ["res/*_mesh.png", "res/*_vat.png", "res/fonts/*"]

[cook.windows]
format = "bc3"
max_size = 0
```

Platforms: `ios`, `macos`, `windows`, `linux`, `android`, `visionos`.
Formats: `astc_4x4`, `astc_6x6`, `bc1`, `bc3`. ASTC platforms are Apple and
Android; BC formats are intended for desktop GPUs. macOS ASTC requires Apple
silicon; use BC3 for Intel Mac targets. These rules do not add new native
packaging or GPU backends to platforms the toolchain does not support yet.

`max_size` limits the longest edge while preserving aspect ratio. Zero means
unlimited; omitted means 512 for iOS, Android and visionOS, unlimited elsewhere.
Optional `include` and `exclude` are arrays of case-sensitive project-relative
globs; `*` also matches `/`. All PNG/JPEG/WebP/TGA/BMP files in the manifest's
`assets` are selected by default. `exclude` wins. Optional `quality` is an
astcenc preset (`fastest`, `fast`, `medium`, `thorough`, `exhaustive`; default
`medium`). `srgb = true` selects sRGB encoding and sampling; default false
preserves linear sampling. BC1 sRGB is currently unsupported.

Only ordinary GPU pictures should be selected. Exclude mesh/VAT data images,
normal maps requiring special encoding, CPU-read images and font atlases.
16-bit grayscale data images are rejected. Embedded GLB pictures are not
rewritten. Font loaders and `io_load_image` still require original image files.

## Build dependencies

On macOS, Apple texture cooking delegates to the Nano Script tool
[`ns_tex`](../nscode/ns_tex/README.md): `make` places it beside `bin/ns`,
and `make install` installs its launcher and sources. It uses Xcode's bundled
`textureconverter` for ASTC and does not require Python, Pillow or astcenc:

```sh
ns_tex /absolute/project ios /absolute/project/bin/resources
```

Apple `ns run`, `ns build`, `ns cook --platform ios|macos` and
`ns patch --cook-platform ios|macos` all use this path. The following
dependencies apply to the existing cooker for other platforms.

Cooking requires Python 3.11+ and Pillow 11.2.1+ on the build machine. ASTC
also requires Arm's [astcenc](https://github.com/ARM-software/astc-encoder).
Build a pinned copy with `sh scripts/bootstrap_astcenc.sh`; it writes
`bin/astcenc`, which `make install` also installs. Alternatively put `astcenc`
on PATH or set `NS_ASTCENC` to its executable. `NS_COOK_PYTHON` selects the
Python executable. Without it, macOS takes the first of Homebrew's, then
`/usr/local/bin`'s, then PATH's and the system `python3` that has both tomllib
and Pillow, so an Xcode build with a different PATH finds the same one.

The app ships neither Python nor the encoder. The language-only `ns`
executable does not link image or GPU libraries. Encoded results are cached
under the project's `bin/.ns-cook/`, keyed by source contents, settings,
cooker and encoder identity. Missing dependencies or failed encoding fail the
build. Source assets are never modified.

## Packaging and loading

Native macOS app builds select `cook.macos`, Linux packaging selects
`cook.linux`. Xcode's final `Cook Textures` phase runs after Copy Resources,
selects the actual SDK (iOS simulator and device both select `ios`), and runs
before code signing. Each selected packaged `res/picture.png` becomes
`res/picture.png.nstex`; the packaged original is removed after successful
encoding. There is one base mip; mipmap generation is not yet supported.

`ui_atlas_load` automatically prefers the cooked texture. Original atlas
width and height are retained for pixel-coordinate UVs even after resizing.
For custom GPU loading, try `gpu_texture_load_cooked(source_path)` before the
usual image decode/upload; it returns a texture ID, or 0 if absent, invalid,
or unsupported. It uploads compressed blocks directly and releases CPU staging
memory. For example:

```ns
let texture = gpu_texture_load_cooked("res/cloud.png")
if texture == 0u {
    // Existing source image loader for development runs.
}
```

A picture shrunk to `max_size` is resized one channel at a time, so the colour
under fully transparent texels survives: the GPU filters straight alpha, and
blackened transparent texels would draw a dark fringe round the opaque ones.

On Metal every block-compressed texture is created in private storage and
filled by a blit, on macOS as on iOS, so the path the device uses is the one
the desktop exercises. Metal samples ASTC (Apple2 GPUs and later), ETC2/EAC
(every Apple GPU) and BC (every Mac, and iPads that report it from iOS
16.4). A format the GPU cannot sample, and PVRTC, which Apple deprecated, is
refused when the texture is created, so `gpu_texture_load_cooked` returns 0.
`test/gpu_compressed_test.ns` checks every format texel by texel on the
local GPU.

Raw `ns run` and Wasm packaging retain originals. Browser
`gpu_texture_load_cooked` returns 0 so the same development fallback works.
A platform without a matching table keeps originals.

To cook an already copied resource directory manually:

```sh
ns cook . --platform ios --output bin/ios-resources
```

This command writes selected compressed textures and removes matching copied
originals. It does not copy unrelated assets. Keep its output in `bin/` or an
external generated directory.

OTA patches need an explicit platform when the manifest has cook rules:

```sh
ns patch --cook-platform ios
```

The patch writer cooks a separate staging snapshot before hashing and bundling,
then removes the staging copy. Use separate patch targets/URLs for incompatible
platform formats. The patch's assets retain the same relative paths as the app.

## NSTX v1

The runtime sidecar is a fixed 32-byte little-endian header followed by one
mip's tightly packed GPU blocks. Header: magic `NSTX`, version `u32 = 1`, GPU
pixel-format enum `u32`, cooked width/height `u32`, original width/height `u32`,
payload size `u32`. The format determines the block footprint (4 or 6) and
block size (8 bytes for BC1, otherwise 16). Payload size must exactly match
`ceil(width / block) * ceil(height / block) * block_bytes`. Readers reject
unknown versions/formats, invalid dimensions, truncation and trailing data.
