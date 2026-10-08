/* common.h - shared constants and small helpers for the native opendana core.
 *
 * The core is pure C11 and deliberately has no SDL dependency so that it can be
 * built, unit-tested and reasoned about on its own.
 */
#ifndef OD_COMMON_H
#define OD_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define OD_GRID_W 16
#define OD_GRID_H 15
#define OD_LEVEL_COUNT 150
#define OD_GRID_CELLS (OD_GRID_W * OD_GRID_H)

#define OD_TILE_COUNT 8192
#define OD_TILE_BYTES 16
#define OD_CHR_BYTES (OD_TILE_COUNT * OD_TILE_BYTES)

#define OD_SCREEN_W 256
#define OD_SCREEN_H 240

typedef enum {
    OD_OK = 0,
    OD_ERR_IO,
    OD_ERR_FORMAT,
    OD_ERR_RANGE,
    OD_ERR_NOMEM
} od_status;

const char *od_status_str(od_status status);

#define OD_FNV1A32_INIT 0x811C9DC5u
#define OD_FNV1A64_INIT 0xCBF29CE484222325ull

uint32_t od_fnv1a32(const void *data, size_t size, uint32_t state);
uint64_t od_fnv1a64(const void *data, size_t size, uint64_t state);

/* 16-bit little-endian reader used by the asset parsers. */
static inline uint16_t od_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t od_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* C6.81: path separators. '/' everywhere; on Windows also '\\', because the
 * per-user folders come from %LOCALAPPDATA% (backslashes) and the paths a
 * player types or the chooser returns use either. On POSIX a backslash is an
 * ordinary file-name character, so it is not a separator there. */
static inline int od_path_is_sep(char c)
{
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

/* The last separator of `p`, or NULL. */
static inline char *od_path_last_sep(const char *p)
{
    char *last = NULL;
    for (; *p != '\0'; p++)
        if (od_path_is_sep(*p))
            last = (char *)p;
    return last;
}

#endif /* OD_COMMON_H */
