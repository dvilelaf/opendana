/* Loader-owned pinned bytes. Capture precedes publication; no directory walk,
 * no second parser inventory, no mutable re-read for state identity. Serial
 * content lifecycle, as with the existing graphics/table transaction. */
#include "content_snapshot.h"
#include "../import/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct snapshot_row {
    char *path, *bytes;
    size_t size;
    int resource;
    struct snapshot_row *next;
} snapshot_row;

struct od_content_snapshot {
    unsigned refs, providers;
    int sealed;
    size_t count;
    snapshot_row *rows;
    od_content_vfs scope;
};

struct od_content_snapshot *od_content_snapshot_create(const od_content_vfs *scope)
{
    struct od_content_snapshot *s = calloc(1, sizeof *s);
    if (s) { s->refs = 1; s->scope = *scope; s->scope.snapshot = NULL; }
    return s;
}
int od_content_snapshot_matches(const od_content_vfs *vfs)
{
    if (!vfs || !vfs->snapshot || vfs->count != vfs->snapshot->scope.count) return 0;
    for (int i = 0; i < vfs->count; ++i)
        if (strcmp(vfs->layers[i].root, vfs->snapshot->scope.layers[i].root)) return 0;
    return 1;
}
void od_content_snapshot_retain(struct od_content_snapshot *s) { if (s) ++s->refs; }
void od_content_snapshot_release(struct od_content_snapshot *s)
{
    if (!s || --s->refs) return;
    snapshot_row *row = s->rows;
    while (row) {
        snapshot_row *next = row->next;
        free(row->path); free(row->bytes); free(row); row = next;
    }
    free(s);
}

void od_content_snapshot_completed(const od_content_vfs *vfs, unsigned provider)
{
    if (vfs && vfs->snapshot) vfs->snapshot->providers |= provider;
}

static int relative_path(const char *path)
{
    if (!path || !*path || *path == '/' || *path == '\\') return 0;
    const char *part = path;
    for (const char *p = path;; ++p) {
        if (*p == ':' || *p == '\\') return 0;
        if (*p == '/' || !*p) {
            size_t n = (size_t)(p - part);
            if (!n || (n == 1 && *part == '.') || (n == 2 && !strncmp(part, "..", 2))) return 0;
            part = p + 1;
        }
        if (!*p) return 1;
    }
}

od_status od_content_snapshot_read(const od_content_vfs *vfs, const char *rel,
    char **data, size_t *size)
{
    if (!od_content_snapshot_matches(vfs) || !data || !size || !relative_path(rel)) return OD_ERR_FORMAT;
    *data = NULL; *size = 0;
    struct od_content_snapshot *s = vfs->snapshot;
    snapshot_row *row = s->rows;
    while (row && strcmp(row->path, rel)) row = row->next;
    if (!row) {
        if (s->sealed) return OD_ERR_IO; /* no late/lazy fallback */
        char *bytes = NULL; size_t n = 0;
        od_status st = od_content_vfs_read_uncached(vfs, rel, &bytes, &n);
        if (st != OD_OK) return st;
        row = malloc(sizeof *row);
        char *path = malloc(strlen(rel) + 1);
        if (!row || !path) { free(row); free(path); free(bytes); return OD_ERR_NOMEM; }
        strcpy(path, rel);
        row->path = path; row->bytes = bytes; row->size = n;
        row->resource = strcmp(rel, "pack.json") && strcmp(rel, "mod.json");
        row->next = s->rows; s->rows = row;
        if (row->resource) ++s->count;
    }
    char *copy = malloc(row->size + 1);
    if (!copy) return OD_ERR_NOMEM;
    memcpy(copy, row->bytes, row->size + 1);
    *data = copy; *size = row->size;
    return OD_OK;
}

od_status od_content_vfs_finish_scenes(const od_content_vfs *vfs)
{
    if (!od_content_snapshot_matches(vfs) || vfs->snapshot->providers != OD_SNAPSHOT_ALL) return OD_ERR_IO;
    vfs->snapshot->sealed = 1;
    return OD_OK;
}

static int order_paths(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

od_status od_content_vfs_resources(const od_content_vfs *vfs, char ***paths, size_t *count)
{
    if (paths) *paths = NULL;
    if (count) *count = 0;
    if (!paths || !count) return OD_ERR_FORMAT;
    if (!od_content_snapshot_matches(vfs) || !vfs->snapshot->sealed ||
        vfs->snapshot->providers != OD_SNAPSHOT_ALL || !vfs->snapshot->count) return OD_ERR_IO;
    size_t n = vfs->snapshot->count, i = 0;
    char **result = malloc(n * sizeof *result);
    if (!result) return OD_ERR_NOMEM;
    for (snapshot_row *row = vfs->snapshot->rows; row; row = row->next) {
        if (!row->resource) continue;
        result[i] = malloc(strlen(row->path) + 1);
        if (!result[i]) {
            while (i) free(result[--i]);
            free(result); return OD_ERR_NOMEM;
        }
        strcpy(result[i++], row->path);
    }
    if (n > 1) qsort(result, n, sizeof *result, order_paths);
    *paths = result; *count = n;
    return OD_OK;
}

static void digest_length(od_sha256_ctx *ctx, size_t n)
{
    uint8_t length[8];
    for (unsigned i = 0; i < 8; ++i) { length[i] = (uint8_t)n; n >>= 8; }
    od_sha256_update(ctx, length, sizeof length);
}

od_status od_content_vfs_snapshot_digest(const od_content_vfs *vfs, uint8_t out[32])
{
    if (!out) return OD_ERR_FORMAT;
    char **paths = NULL; size_t count = 0;
    od_status st = od_content_vfs_resources(vfs, &paths, &count);
    if (st != OD_OK) return st;
    od_sha256_ctx ctx; od_sha256_init(&ctx);
    for (size_t i = 0; i < count; ++i) {
        snapshot_row *row = vfs->snapshot->rows;
        while (row && strcmp(row->path, paths[i])) row = row->next;
        digest_length(&ctx, strlen(row->path));
        od_sha256_update(&ctx, row->path, strlen(row->path));
        digest_length(&ctx, row->size);
        od_sha256_update(&ctx, row->bytes, row->size);
        free(paths[i]);
    }
    free(paths);
    uint8_t digest[32]; od_sha256_final(&ctx, digest);
    memcpy(out, digest, 32);
    return OD_OK;
}
