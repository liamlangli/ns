#include "gpu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static u32 expected_format, expected_size;
static i32 expected_width, expected_height;
static i32 uploads;
static u32 create_texture(i32 width, i32 height, i32 layers, i32 format, u32 usage, i32 mips, i32 kind) {
    assert(width == expected_width && height == expected_height);
    assert(format == (i32)expected_format && layers == 1 && mips == 1);
    assert(usage == TEXTURE_USAGE_READ && kind == TEXTURE_2D);
    return 17;
}
static void upload_texture(u32 texture, i32 mip, i32 layer, const void *data, u64 size) {
    assert(texture == 17 && mip == 0 && layer == 0 && data && size == expected_size);
    uploads++;
}
static void word(u8 *p, u32 value) {
    for (int i = 0; i < 4; i++) p[i] = (u8)(value >> (i * 8));
}
static void write_image(const char *path, u8 *header, int extra) {
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(header, 1, 32, file) == 32);
    for (u32 i = 0; i < expected_size; i++) fputc(0, file);
    if (extra) fputc(0, file);
    fclose(file);
}
int main(int argc, char **argv) {
    if (argc == 2) {
        assert(gpu_request_device(NULL));
        i32 width = 0, height = 0;
        u32 texture = gpu_texture_load_cooked_info(argv[1], &width, &height);
        assert(texture && width > 0 && height > 0);
        gpu_texture_destroy(texture);
        gpu_destroy_device();
        puts("PASS: real GPU compressed texture allocation and upload");
        return 0;
    }
    gpu_v2_ops ops = {.texture_create = create_texture, .texture_upload = upload_texture};
    gpu_v2_set_backend(&ops, 0, 0);
    char path[] = "/tmp/ns-cooked-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    unlink(path);
    char filename[128];
    snprintf(filename, sizeof(filename), "%s.nstex", path);
    const u32 formats[] = {PIXELFORMAT_ASTC_4x4_RGBA, PIXELFORMAT_ASTC_4x4_SRGBA,
                          PIXELFORMAT_ASTC_6x6_RGBA, PIXELFORMAT_ASTC_6x6_SRGBA,
                          PIXELFORMAT_BC1_RGBA, PIXELFORMAT_BC3_RGBA, PIXELFORMAT_BC3_SRGBA};
    for (u32 i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        expected_format = formats[i];
        expected_width = 13;
        expected_height = 7;
        u32 block = (formats[i] == PIXELFORMAT_ASTC_6x6_RGBA || formats[i] == PIXELFORMAT_ASTC_6x6_SRGBA) ? 6 : 4;
        expected_size = ((13 + block - 1) / block) * ((7 + block - 1) / block) * (formats[i] == PIXELFORMAT_BC1_RGBA ? 8 : 16);
        assert((u32)gpu_pixel_format_surface_pitch((gpu_pixel_format)formats[i], 13, 7, 1) == expected_size);
        u8 header[32] = {'N', 'S', 'T', 'X'};
        word(header + 4, 1);
        word(header + 8, formats[i]);
        word(header + 12, 13);
        word(header + 16, 7);
        word(header + 20, 26);
        word(header + 24, 14);
        word(header + 28, expected_size);
        write_image(filename, header, 0);
        i32 width = 0, height = 0;
        assert(gpu_texture_load_cooked_info(path, &width, &height) == 17);
        assert(width == 26 && height == 14);
        assert(uploads == (i32)i + 1);
        write_image(filename, header, 1);
        assert(gpu_texture_load_cooked(path) == 0);
        word(header + 28, expected_size - 1);
        write_image(filename, header, 0);
        assert(gpu_texture_load_cooked(path) == 0);
        word(header + 12, 0xffffffffu);
        write_image(filename, header, 0);
        assert(gpu_texture_load_cooked(path) == 0);
    }
    unlink(filename);
    assert(gpu_texture_load_cooked(path) == 0);
    gpu_v2_set_backend(NULL, 0, 0);
    puts("PASS: cooked texture formats, atlas metadata, malformed input and upload sizes");
    return 0;
}
