/* stb_image_impl.c - the single translation unit that compiles the vendored
 * stb_image (public domain, v2.30). Only the PNG decoder is needed: the content
 * package stores the CHR as indexed PNG sheets (C2.2). No stdio loader is
 * needed because the VFS hands us an in-memory buffer. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
