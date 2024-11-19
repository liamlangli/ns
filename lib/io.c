#include "ns_type.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

typedef struct Image {
    i32 width;
    i32 height;
    i32 channels;
    u8 *data;
} Image;

ns_export Image* io_load_image(ns_str path) {
    // Implementation for loading an image from the file at 'path'
    // This is a placeholder implementation
    Image *img = (Image*)malloc(sizeof(Image));
    if (img == NULL) {
        fprintf(stderr, "Failed to allocate memory for image\n");
        return NULL;
    }

    img->data = stbi_load(path.data, &img->width, &img->height, &img->channels, 0);
    if (img->data == NULL) {
        fprintf(stderr, "Failed to load image from file: %s\n", path.data);
        free(img);
        return NULL;
    }

    return img;
}

ns_export void io_save_image(const char *path, const Image *img) {
    // Implementation for saving an image to the file at 'path'
    // This is a placeholder implementation
    if (img == NULL) {
        fprintf(stderr, "Invalid image\n");
        return;
    }

    int result = stbi_write_png(path, img->width, img->height, img->channels, img->data, img->width * img->channels);
    if (result == 0) {
        fprintf(stderr, "Failed to save image to file: %s\n", path);
    }
}