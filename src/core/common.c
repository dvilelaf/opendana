#include "opendana/common.h"

const char *od_status_str(od_status status)
{
    switch (status) {
    case OD_OK: return "ok";
    case OD_ERR_IO: return "I/O error";
    case OD_ERR_FORMAT: return "malformed asset";
    case OD_ERR_RANGE: return "value out of range";
    case OD_ERR_NOMEM: return "out of memory";
    default: return "unknown error";
    }
}

uint32_t od_fnv1a32(const void *data, size_t size, uint32_t seed)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = seed;
    for (size_t i = 0; i < size; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

uint64_t od_fnv1a64(const void *data, size_t size, uint64_t seed)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = seed;
    for (size_t i = 0; i < size; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}
