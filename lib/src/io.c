#include "ns_type.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

typedef struct io_image {
    i32 width;
    i32 height;
    i32 channels;
    u8 *data;
} io_image;

// `path` arrives as a C string: the ns runtime passes `str` arguments to ref
// functions as a char* (see the FFI string marshalling in ns_vm_lib.c), not as
// an ns_str struct by value.
io_image* io_load_image(const char *path) {
    io_image *img = (io_image*)ns_malloc(sizeof(io_image));
    if (img == NULL) {
        ns_error("io", "Failed to allocate memory for image\n");
        return NULL;
    }

    img->data = stbi_load(path, &img->width, &img->height, &img->channels, 0);
    if (img->data == NULL) {
        free(img);
        ns_error("io", "Failed to load image from file: %s\n", path);
        return NULL;
    }

    return img;
}

i32 io_save_image(const char *path, const io_image *img) {
    if (img == NULL) {
        ns_error("io", "Image is NULL\n");
        return 0;
    }

    i32 result = stbi_write_png(path, img->width, img->height, img->channels, img->data, img->width * img->channels);
    if (result == 0) {
        ns_error("io", "Failed to save image to file: %s\n", path);
    }
    return result;
}
