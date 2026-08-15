#include "compress.h"

#include <zlib.h>
#include <zstd.h>
#include <zstd_errors.h>

#include <stddef.h>
#include <string.h>

// zlib window-bit selectors. A negative value asks for a raw deflate stream
// with no wrapper, 15 is the zlib wrapper, and 15 + 16 is the gzip wrapper.
enum {
    COMPRESS_WINDOW_RAW = -15,
    COMPRESS_WINDOW_ZLIB = 15,
    COMPRESS_WINDOW_GZIP = 15 + 16,
    COMPRESS_MEMORY_LEVEL = 8,
    // 10-byte gzip header plus the 8-byte CRC32/ISIZE trailer.
    COMPRESS_GZIP_FRAME_MIN = 18
};

static int32_t compress_zlib_status(int status) {
    switch (status) {
        case Z_BUF_ERROR: return NS_COMPRESS_ERROR_BUFFER;
        case Z_MEM_ERROR: return NS_COMPRESS_ERROR_MEMORY;
        case Z_DATA_ERROR: return NS_COMPRESS_ERROR_DATA;
        case Z_STREAM_ERROR: return NS_COMPRESS_ERROR_ARGUMENT;
        case Z_VERSION_ERROR: return NS_COMPRESS_ERROR_UNSUPPORTED;
        default: return NS_COMPRESS_ERROR_INTERNAL;
    }
}

static int32_t compress_zstd_status(size_t code) {
    switch (ZSTD_getErrorCode(code)) {
        case ZSTD_error_dstSize_tooSmall: return NS_COMPRESS_ERROR_BUFFER;
        case ZSTD_error_memory_allocation: return NS_COMPRESS_ERROR_MEMORY;
        case ZSTD_error_parameter_unsupported:
        case ZSTD_error_frameParameter_unsupported:
        case ZSTD_error_frameParameter_windowTooLarge:
        case ZSTD_error_dictionary_wrong:
        case ZSTD_error_dictionaryCreation_failed: return NS_COMPRESS_ERROR_UNSUPPORTED;
        case ZSTD_error_srcSize_wrong:
        case ZSTD_error_corruption_detected:
        case ZSTD_error_checksum_wrong:
        case ZSTD_error_prefix_unknown: return NS_COMPRESS_ERROR_DATA;
        default: return NS_COMPRESS_ERROR_INTERNAL;
    }
}

static int32_t compress_bound_bits(int32_t size, int window_bits) {
    if (size < 0) return NS_COMPRESS_ERROR_ARGUMENT;

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    int status = deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window_bits,
                              COMPRESS_MEMORY_LEVEL, Z_DEFAULT_STRATEGY);
    if (status != Z_OK) return compress_zlib_status(status);

    uLong bound = deflateBound(&stream, (uLong)size);
    deflateEnd(&stream);
    if (bound > (uLong)INT32_MAX) return NS_COMPRESS_ERROR_BUFFER;
    return (int32_t)bound;
}

static int32_t compress_deflate_bits(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity,
                                     int32_t level, int window_bits) {
    if (src == NULL || dst == NULL || size < 0 || capacity < 0) return NS_COMPRESS_ERROR_ARGUMENT;
    if (level < Z_DEFAULT_COMPRESSION || level > Z_BEST_COMPRESSION) return NS_COMPRESS_ERROR_ARGUMENT;

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    int status = deflateInit2(&stream, level, Z_DEFLATED, window_bits,
                              COMPRESS_MEMORY_LEVEL, Z_DEFAULT_STRATEGY);
    if (status != Z_OK) return compress_zlib_status(status);

    stream.next_in = (Bytef *)src;
    stream.avail_in = (uInt)size;
    stream.next_out = (Bytef *)dst;
    stream.avail_out = (uInt)capacity;

    status = deflate(&stream, Z_FINISH);
    if (status != Z_STREAM_END) {
        // Z_OK or Z_BUF_ERROR after Z_FINISH means the whole stream did not
        // fit; deflate never needs more input once Z_FINISH is requested.
        int32_t result = (status == Z_OK || status == Z_BUF_ERROR)
            ? NS_COMPRESS_ERROR_BUFFER
            : compress_zlib_status(status);
        deflateEnd(&stream);
        return result;
    }

    int32_t written = (int32_t)stream.total_out;
    deflateEnd(&stream);
    return written;
}

static int32_t compress_inflate_bits(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity,
                                     int window_bits) {
    if (src == NULL || dst == NULL || size < 0 || capacity < 0) return NS_COMPRESS_ERROR_ARGUMENT;

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    int status = inflateInit2(&stream, window_bits);
    if (status != Z_OK) return compress_zlib_status(status);

    stream.next_in = (Bytef *)src;
    stream.avail_in = (uInt)size;
    stream.next_out = (Bytef *)dst;
    stream.avail_out = (uInt)capacity;

    status = inflate(&stream, Z_FINISH);
    if (status != Z_STREAM_END) {
        int32_t result;
        if (status == Z_MEM_ERROR) {
            result = NS_COMPRESS_ERROR_MEMORY;
        } else if (stream.avail_out == 0) {
            // The decoder stopped because it ran out of room, not because the
            // input ended.
            result = NS_COMPRESS_ERROR_BUFFER;
        } else {
            result = NS_COMPRESS_ERROR_DATA;
        }
        inflateEnd(&stream);
        return result;
    }

    int32_t written = (int32_t)stream.total_out;
    inflateEnd(&stream);
    return written;
}

int32_t compress_deflate_bound(int32_t size) {
    return compress_bound_bits(size, COMPRESS_WINDOW_RAW);
}

int32_t compress_zlib_bound(int32_t size) {
    return compress_bound_bits(size, COMPRESS_WINDOW_ZLIB);
}

int32_t compress_gzip_bound(int32_t size) {
    return compress_bound_bits(size, COMPRESS_WINDOW_GZIP);
}

int32_t compress_deflate(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity, int32_t level) {
    return compress_deflate_bits(src, size, dst, capacity, level, COMPRESS_WINDOW_RAW);
}

int32_t compress_zlib_deflate(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity, int32_t level) {
    return compress_deflate_bits(src, size, dst, capacity, level, COMPRESS_WINDOW_ZLIB);
}

int32_t compress_gzip_deflate(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity, int32_t level) {
    return compress_deflate_bits(src, size, dst, capacity, level, COMPRESS_WINDOW_GZIP);
}

int32_t compress_inflate(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity) {
    return compress_inflate_bits(src, size, dst, capacity, COMPRESS_WINDOW_RAW);
}

int32_t compress_zlib_inflate(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity) {
    return compress_inflate_bits(src, size, dst, capacity, COMPRESS_WINDOW_ZLIB);
}

int32_t compress_gzip_inflate(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity) {
    return compress_inflate_bits(src, size, dst, capacity, COMPRESS_WINDOW_GZIP);
}

int32_t compress_gzip_decoded_size(const uint8_t *src, int32_t size) {
    if (src == NULL || size < 0) return NS_COMPRESS_ERROR_ARGUMENT;
    if (size < COMPRESS_GZIP_FRAME_MIN || src[0] != 0x1f || src[1] != 0x8b) return NS_COMPRESS_ERROR_DATA;

    uint32_t decoded = (uint32_t)src[size - 4] |
                       ((uint32_t)src[size - 3] << 8) |
                       ((uint32_t)src[size - 2] << 16) |
                       ((uint32_t)src[size - 1] << 24);
    if (decoded > (uint32_t)INT32_MAX) return NS_COMPRESS_ERROR_BUFFER;
    return (int32_t)decoded;
}

int32_t compress_zstd_bound(int32_t size) {
    if (size < 0) return NS_COMPRESS_ERROR_ARGUMENT;
    size_t bound = ZSTD_compressBound((size_t)size);
    if (ZSTD_isError(bound)) return compress_zstd_status(bound);
    if (bound > (size_t)INT32_MAX) return NS_COMPRESS_ERROR_BUFFER;
    return (int32_t)bound;
}

int32_t compress_zstd_encode(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity, int32_t level) {
    if (src == NULL || dst == NULL || size < 0 || capacity < 0) return NS_COMPRESS_ERROR_ARGUMENT;
    // Zstandard reads level 0 as "use the default", so it is passed through.
    if (level != 0 && (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel())) return NS_COMPRESS_ERROR_ARGUMENT;

    size_t written = ZSTD_compress(dst, (size_t)capacity, src, (size_t)size, level);
    if (ZSTD_isError(written)) return compress_zstd_status(written);
    return (int32_t)written;
}

int32_t compress_zstd_decode(const uint8_t *src, int32_t size, uint8_t *dst, int32_t capacity) {
    if (src == NULL || dst == NULL || size < 0 || capacity < 0) return NS_COMPRESS_ERROR_ARGUMENT;

    size_t written = ZSTD_decompress(dst, (size_t)capacity, src, (size_t)size);
    if (ZSTD_isError(written)) return compress_zstd_status(written);
    return (int32_t)written;
}

int32_t compress_zstd_decoded_size(const uint8_t *src, int32_t size) {
    if (src == NULL || size < 0) return NS_COMPRESS_ERROR_ARGUMENT;

    unsigned long long decoded = ZSTD_getFrameContentSize(src, (size_t)size);
    if (decoded == ZSTD_CONTENTSIZE_ERROR) return NS_COMPRESS_ERROR_DATA;
    if (decoded == ZSTD_CONTENTSIZE_UNKNOWN) return NS_COMPRESS_ERROR_UNSUPPORTED;
    if (decoded > (unsigned long long)INT32_MAX) return NS_COMPRESS_ERROR_BUFFER;
    return (int32_t)decoded;
}

int32_t compress_zstd_level_min(void) {
    return ZSTD_minCLevel();
}

int32_t compress_zstd_level_max(void) {
    return ZSTD_maxCLevel();
}

uint32_t compress_crc32(uint32_t crc, const uint8_t *data, int32_t size) {
    if (data == NULL || size <= 0) return crc;
    return (uint32_t)crc32((uLong)crc, (const Bytef *)data, (uInt)size);
}

uint32_t compress_adler32(uint32_t adler, const uint8_t *data, int32_t size) {
    if (data == NULL || size <= 0) return adler;
    return (uint32_t)adler32((uLong)adler, (const Bytef *)data, (uInt)size);
}

const char *compress_status_str(int32_t status) {
    switch (status) {
        case NS_COMPRESS_ERROR_ARGUMENT: return "invalid argument";
        case NS_COMPRESS_ERROR_BUFFER: return "destination buffer too small";
        case NS_COMPRESS_ERROR_DATA: return "corrupt or truncated input";
        case NS_COMPRESS_ERROR_MEMORY: return "out of memory";
        case NS_COMPRESS_ERROR_UNSUPPORTED: return "unsupported stream";
        case NS_COMPRESS_ERROR_INTERNAL: return "internal codec failure";
        default: return status < 0 ? "unknown status" : "ok";
    }
}

const char *compress_zlib_version(void) {
    return zlibVersion();
}

const char *compress_zstd_version(void) {
    return ZSTD_versionString();
}
