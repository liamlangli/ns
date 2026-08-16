# Compress Module

`compress` is Nano Script's native byte-compression module. It is backed by two
pinned third-party downloads:

| Library | Version | Commit | License |
| --- | --- | --- | --- |
| [zlib](https://github.com/madler/zlib) | 1.3.1 | `51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf` | zlib |
| [Zstandard](https://github.com/facebook/zstd) | 1.5.7 | `f8745da6ff1ad1e7bab384bd1f9d742439278e99` | BSD-3-Clause / GPLv2 |

Neither source tree lives in this repository. The build downloads and checksums
each archive into the ignored `third_party/zlib/` and `third_party/zstd/`
directories, exactly as `dynamic` does for Box3D (doc/dynamic.md).

## Surface

The module is deliberately allocation-free. The caller owns both buffers, sizes
the destination with the matching `*_bound` fn, and every call returns either
the number of bytes produced or a negative status code.

```ns
use compress

let source = [u8](4096)
// ... fill source ...

let capacity = compress_gzip_bound(4096)
let packed = [u8](8192)
let packed_size = compress_gzip_deflate(source, 4096, packed, 8192, COMPRESS_LEVEL_BEST)
if packed_size < 0 {
    print(compress_status_str(packed_size))
}

let restored = [u8](8192)
let restored_size = compress_gzip_inflate(packed, packed_size, restored, 8192)
```

## Formats

Four framings are exposed, each with its own bound, encoder, and decoder. They
are not interchangeable: decoding a stream with the wrong framing reports
`COMPRESS_ERROR_DATA` rather than producing bytes.

| Framing | Spec | Encode | Decode |
| --- | --- | --- | --- |
| raw deflate | RFC 1951 | `compress_deflate` | `compress_inflate` |
| zlib | RFC 1950 | `compress_zlib_deflate` | `compress_zlib_inflate` |
| gzip | RFC 1952 | `compress_gzip_deflate` | `compress_gzip_inflate` |
| zstd | RFC 8878 | `compress_zstd_encode` | `compress_zstd_decode` |

Only two of the four record their decoded size. `compress_gzip_decoded_size`
reads the gzip ISIZE trailer, which is stored modulo 2^32 and is therefore exact
only below 4 GiB. `compress_zstd_decoded_size` reads the zstd frame header and
reports `COMPRESS_ERROR_UNSUPPORTED` for a frame written without a content size.
Raw deflate and zlib streams record nothing, so the caller must already know how
large the decoded data is.

## Levels

The three zlib framings share one scale: `COMPRESS_LEVEL_DEFAULT` (-1),
`COMPRESS_LEVEL_STORE` (0), and 1 through 9 from `COMPRESS_LEVEL_FASTEST` to
`COMPRESS_LEVEL_BEST`. Any other value is rejected with
`COMPRESS_ERROR_ARGUMENT`. Level 0 stores the input, so the result is slightly
larger than the input and still within `compress_zlib_bound`.

Zstandard reads level 0 as its own default of 3. The accepted range is reported
at runtime by `compress_zstd_level_min` and `compress_zstd_level_max`; the
pinned build accepts -131072 through 22.

## Status codes

Any result `>= 0` is a byte count. The negative codes are:

| Code | Value | Meaning |
| --- | --- | --- |
| `COMPRESS_ERROR_ARGUMENT` | -1 | null buffer, negative size, or a level out of range |
| `COMPRESS_ERROR_BUFFER` | -2 | the destination is too small |
| `COMPRESS_ERROR_DATA` | -3 | truncated, corrupt, or wrongly framed input |
| `COMPRESS_ERROR_MEMORY` | -4 | the codec could not allocate |
| `COMPRESS_ERROR_UNSUPPORTED` | -5 | the stream needs a feature this build does not have |
| `COMPRESS_ERROR_INTERNAL` | -6 | the codec reported an unexpected failure |

`compress_status_str` turns any of these into a readable string. A destination
that is too small is always reported as `COMPRESS_ERROR_BUFFER`; a partial
result is never written back as a byte count.

## Checksums

`compress_crc32` and `compress_adler32` expose zlib's checksums. Both take the
running value as their first argument, so seed with `COMPRESS_CRC32_INIT` (0) or
`COMPRESS_ADLER32_INIT` (1) and feed the previous result back in to continue
over the next chunk. Each call always starts at index 0 of the array it is
given, so chunked use means refilling a buffer, not offsetting into one.

## Build and platform support

Both libraries are portable C and are compiled straight into the compress
feature library by `lib/Makefile`. Run `make zlib`, `make zstd`, or
`make compress_deps` to download the pinned sources explicitly; the normal
`make`, `make std`, and install flows also download them when absent. They
produce and install `compress.dylib` on macOS or `compress.so` on Linux/Windows,
and need only the C runtime.

Generated Apple IDE apps compile the adapter and pinned Zstandard sources
directly into their macOS, iOS, and visionOS targets. Those targets use the
platform `libz` for raw deflate, zlib, gzip, and checksums, while preserving the
same Nano Script API and status codes.

Two build settings are worth knowing about. zlib is compiled with `Z_PREFIX`, so
its entry points are renamed to `z_*` and the statically linked copy cannot
collide with a system libz that is already in the process — the module is opened
with `RTLD_GLOBAL`. Zstandard is compiled with `XXH_NAMESPACE=ZSTD_` for the same
reason, and with `ZSTD_DISABLE_ASM` so the build stays on the portable C decoder
instead of the x86-64 assembly Huffman loop.

`compress_zlib_version` and `compress_zstd_version` report the linked versions,
which is the cheapest way to confirm the pinned downloads are what a build
actually used.

Run the focused regression coverage with:

```sh
bin/ns run test/compress_test.ns
```
