#ifndef NS_COMPRESS_H
#define NS_COMPRESS_H

#include <stdint.h>

// Native compression adapter used by lib/compress.ns. zlib supplies raw
// deflate (RFC 1951), zlib-framed deflate (RFC 1950), and gzip (RFC 1952);
// Zstandard supplies the zstd frame format (RFC 8878).
//
// The interface is intentionally scalar/array-only so it stays inside Nano
// Script's FFI ABI: every call takes caller-owned byte buffers and returns
// either the number of bytes produced or one of the negative status codes
// below. Nothing is allocated on the caller's behalf.

enum {
    NS_COMPRESS_ERROR_ARGUMENT = -1,    // null buffer, negative size, bad level
    NS_COMPRESS_ERROR_BUFFER = -2,      // the destination buffer is too small
    NS_COMPRESS_ERROR_DATA = -3,        // truncated or corrupt input
    NS_COMPRESS_ERROR_MEMORY = -4,      // the codec could not allocate
    NS_COMPRESS_ERROR_UNSUPPORTED = -5, // the stream needs an unsupported feature
    NS_COMPRESS_ERROR_INTERNAL = -6     // the codec reported an unexpected failure
};

// Worst-case encoded size for `size` input bytes, or a negative status.
int32_t compress_deflate_bound(int32_t size);
int32_t compress_zlib_bound(int32_t size);
int32_t compress_gzip_bound(int32_t size);
int32_t compress_zstd_bound(int32_t size);

// `level` is -1 for the codec default, 0 for stored, and 1..9 from fastest to
// smallest. Returns the bytes written to `dst`, or a negative status.
int32_t compress_deflate(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity, int32_t level);
int32_t compress_zlib_deflate(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity, int32_t level);
int32_t compress_gzip_deflate(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity, int32_t level);

// Decode one complete stream. Returns the bytes written to `dst`, or a
// negative status; a `dst` that is too small reports NS_COMPRESS_ERROR_BUFFER
// rather than a partial result.
int32_t compress_inflate(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity);
int32_t compress_zlib_inflate(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity);
int32_t compress_gzip_inflate(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity);

// Decoded size taken from the gzip ISIZE trailer. It is stored modulo 2^32, so
// the value is exact only for members that decode to less than 4 GiB.
int32_t compress_gzip_decoded_size(const uint8_t *src, int32_t size);

// Zstandard. `level` is 0 for the default (3), otherwise a value between
// compress_zstd_level_min() and compress_zstd_level_max().
int32_t compress_zstd_encode(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity, int32_t level);
int32_t compress_zstd_decode(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity);
// Decoded size from the frame header. Frames written without a content size
// report NS_COMPRESS_ERROR_UNSUPPORTED.
int32_t compress_zstd_decoded_size(const uint8_t *src, int32_t size);
int32_t compress_zstd_level_min(void);
int32_t compress_zstd_level_max(void);

// Rolling checksums. Seed crc32 with 0 and adler32 with 1, then feed the
// previous result back in to continue over another chunk.
uint32_t compress_crc32(uint32_t crc, const uint8_t *data, int32_t size);
uint32_t compress_adler32(uint32_t adler, const uint8_t *data, int32_t size);

// Human-readable name for a negative status code, or "ok" for zero and above.
const char *compress_status_str(int32_t status);

// Pinned upstream versions, e.g. "1.3.1" and "1.5.7".
const char *compress_zlib_version(void);
const char *compress_zstd_version(void);

#endif // NS_COMPRESS_H
