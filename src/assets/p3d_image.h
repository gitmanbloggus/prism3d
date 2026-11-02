/* p3d_image.h: dev-mode image loading. Release builds will load cooked textures instead.
 * Milestone 1: uncompressed and RLE TGA (24/32-bit), binary PPM. */
#ifndef P3D_IMAGE_H
#define P3D_IMAGE_H

#include <stdint.h>

typedef struct { int width, height; uint8_t *rgba; } p3d_image;   /* top row first */

int  p3d_image_load(const char *path, p3d_image *out);
void p3d_image_free(p3d_image *image);

#endif
