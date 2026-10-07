/* png.h - tiny dependency-free PNG writer (truecolour, uncompressed). */
#ifndef OD_PNG_H
#define OD_PNG_H

#include <stddef.h>
#include <stdint.h>

/* Write a `width` x `height` RGB24 image (row-major, 3 bytes per pixel). */
int od_png_write_rgb(const char *path, int width, int height, const uint8_t *rgb);

#endif /* OD_PNG_H */
