#include "ns_type.h"

typedef struct Image {
    int width;
    int height;
    int channels;
    unsigned char *data;
} Image;

Image* io_load_image(ns_str path) {
    // Implementation for loading an image from the file at 'path'
    // This is a placeholder implementation
    Image *img = (Image*)malloc(sizeof(Image));
    if (img == NULL) {
        fprintf(stderr, "Failed to allocate memory for image\n");
        return NULL;
    }

    // Load image data into img->data, set img->width, img->height, img->channels
    return img;
}

void io_save_image(const char *path, const Image *img) {
    // Implementation for saving an image to the file at 'path'
    // This is a placeholder implementation
    if (img == NULL) {
        fprintf(stderr, "Invalid image\n");
        return;
    }
    // Save img->data to file at 'path'
}