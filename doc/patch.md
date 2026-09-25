# Over-the-air patches

A target whose code the host runs as data - `target = "eval"` (the AST
interpreter runs linked source) or `target = "emu"` (ns_cpu runs an image) -
can be updated in the field without shipping a new app: new code and new
resources are published to an HTTP server, and the app installs them the next
time it starts. `exec` and `wasm` targets are machine code or a browser bundle
and do not take patches.

```text
 developer                          server                         player's host
 ---------                          ------                         -------------
 ns patch  ──► bin/<name>_patch/ ──► http://ip/<name>.nsapp      1. GET bytes 0-63 of the index
               <name>.nsapp          http://ip/<hash>.nsbundle      (the fixed header): newer?
               <hash>.nsbundle ...   ...                         2. GET the whole index
                                                                  3. GET the bundles it lacks,
                                                                     in parallel, check SHA-256
                                                                  4. install a snapshot, run it
```

## Configure

```toml
name = "Moon"
version = "0.1.0"
target = "eval"          # or "emu"
assets = ["res"]
patch = "http://192.168.1.20:8080/moon/Moon.nsapp"
# patch_version = 12     # optional: pin the version `ns patch` writes
```

`patch` and `patch_version` may also be set on a `[[targets]]` table, and a
target inherits the top-level keys it does not set. The index file is named
after the target (`<name>.nsapp`); its bundles are fetched from the same
directory of the server.

## Make a patch

```sh
ns patch [path | target]
```

`ns patch` links the target exactly as the host runs it - the linked source for
`eval`, the ns_cpu image for `emu` - collects every file the manifest's
`assets` names, and writes `bin/<name>_patch/`:

- `<name>.nsapp`: the index. A fixed 64-byte header (version, run mode, body
  size, SHA-256 of the body) followed by the bundle table and the file table:
  every file's path, size and SHA-256, and every bundle's name, size and
  SHA-256.
- `<hash>.nsbundle`: the files, packed in path order. The program is always
  alone in the first bundle, because it changes with nearly every patch; the
  assets are cut into bundles of at most 4 MiB (a larger file gets a bundle of
  its own). A bundle is named after its content hash, so an unchanged bundle
  keeps its name from patch to patch, a CDN can cache it forever, and a client
  never downloads it twice.

Bundles no longer referenced are removed from the directory. Upload the whole
directory next to the `patch` URL; uploading the index last means a client
never sees an index whose bundles are not there yet.

The version counts on from the index already in `bin/<name>_patch/`: an
identical patch keeps its version (nothing changes for clients), anything else
gets the next one, and the first patch is 1. `ns clean` removes `bin/`, and the
count with it, so a project that publishes patches for real should pin
`patch_version` in `ns.mod` (or keep `bin/<name>_patch/`). A client only ever
moves to a higher version.

## Apply a patch

The host checks the `patch` URL before the program starts:

1. It requests the fixed header only (`Range: bytes=0-63`; a server that
   ignores Range still works, the client stops reading after 64 bytes). If the
   published version is not above the one it runs, or the server cannot be
   reached within the timeout, it goes on with what it has.
2. It downloads the whole index and checks the body against the header digest,
   the program name and the run mode.
3. It assembles the new snapshot in a private staging directory. A file whose
   size and SHA-256 match one it already has - in the snapshot installed last,
   or among the files the app shipped with - is copied (hard-linked where
   possible). Every bundle holding anything else is downloaded, up to four at
   a time, verified against its digest, and unpacked, checking every file
   against its own digest again.
4. The staging directory becomes `<cache>/<name>/<version>/`, a `current` file
   names it, and older snapshots are removed. Nothing the program runs from is
   touched until every byte is verified; a failure at any step keeps the
   previous snapshot (or the shipped files) in charge.

The program then runs with the snapshot as its working directory, so a
relative path such as `res/house.vox` reads the patched file. Its version is
published as `NS_PATCH_VERSION`, and `os_patch_version()` (`use os`) returns it:
0 when the program runs the files it shipped with.

The cache is `NS_PATCH_DIR` when set, else `~/Library/Caches/ns-patch` on
Apple platforms (the app's own Caches directory inside an iOS sandbox),
`%LOCALAPPDATA%\ns-patch` on Windows and `$XDG_CACHE_HOME/ns-patch` or
`~/.cache/ns-patch` elsewhere. `NS_PATCH_URL` overrides the manifest URL.

Where the check happens:

| Host | Behavior |
| --- | --- |
| `ns run --patch [target]` | installs the newest patch, then runs it; the project root is the base whose files are reused. Plain `ns run` always runs the project source. |
| `ns build` launcher of an interpreted target | passes `--patch` when the target sets `patch` |
| `ns project` Apple app (`eval` or `emu`) | checks at launch; the app ships as the patch `ns patch` wrote last, so it only fetches newer ones. An `emu` patch whose image does not load is discarded and the shipped image runs. |

## Formats

All integers are little-endian; a string is a u16 length and its bytes.

```text
.nsapp header (64 bytes)
  0  "NSAPP\0\0\0"
  8  u16 format (1)
 10  u8  mode (1 eval, 2 emu)       11 u8 flags (0)
 12  u32 patch version
 16  u32 body size
 20  u32 bundle count               24 u32 file count
 28  u32 reserved
 32  u8[32] SHA-256 of the body
.nsapp body
  str name, str app version, str program path, u64 created (unix seconds)
  bundle x count: str file, u64 size, u8[32] SHA-256, u32 first file, u32 file count
  file x count:   str path, u64 size, u8[32] SHA-256

.nsbundle
  "NSBUNDLE", u16 format (1), u16 reserved, u32 file count
  file x count: str path, u64 size
  the files' bytes, in the same order
```

The bundles partition the file table in order. Paths are relative and
`/`-separated; the client rejects an index with an absolute path, a `.` or `..`
part, a backslash or a drive, or a bundle name that is not a plain file name.

## Limits

- Plain `http://` only. The digests make a download all-or-nothing and catch
  corruption, but the index itself is not signed: anyone who can alter traffic
  between the server and the player can publish their own code. Serve patches
  from a network you control, and treat an ns_cpu image or linked source as
  code (doc/cpu.md).
- A patch replaces the program and its assets, not the host: a patch that
  calls a native module or runtime function the installed app does not have
  fails when it gets there. Ship a new app for those.
- The check runs before the program starts and blocks it for at most the
  connection timeout (3 s per step) when the server is unreachable, plus the
  download time of a real update.
- On iOS a server on the local network triggers the system's local network
  permission prompt the first time.
