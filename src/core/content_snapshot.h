/* Internal lifecycle/coverage hooks. No independently maintained path list. */
#ifndef OD_CONTENT_SNAPSHOT_INTERNAL_H
#define OD_CONTENT_SNAPSHOT_INTERNAL_H
#include "opendana/content.h"
enum { OD_SNAPSHOT_TABLES = 1, OD_SNAPSHOT_LEVELS = 2,
    OD_SNAPSHOT_GFX = 4, OD_SNAPSHOT_CINEMATICS = 8, OD_SNAPSHOT_ALL = 15 };
struct od_content_snapshot *od_content_snapshot_create(const od_content_vfs *scope);
int od_content_snapshot_matches(const od_content_vfs *vfs);
od_status od_content_vfs_capture(const od_content_vfs *src, od_content_vfs *out);
void od_content_snapshot_retain(struct od_content_snapshot *s);
void od_content_snapshot_release(struct od_content_snapshot *s);
void od_content_snapshot_completed(const od_content_vfs *vfs, unsigned provider);
od_status od_content_snapshot_read(const od_content_vfs *vfs, const char *rel,
    char **data, size_t *size);
od_status od_content_vfs_read_uncached(const od_content_vfs *vfs, const char *rel,
    char **data, size_t *size);
#endif
