/* stb_image_write_impl.c - compile the vendored PNG writer once.
 *
 * STBIW_WINDOWS_UTF8: on Windows stbi_write_png takes a UTF-8 path and opens it
 * with _wfopen, so a data directory with non-ASCII characters works (C6.29). */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#include "stb_image_write.h"
