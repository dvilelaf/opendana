/* content_tables.h - engine tables loaded from the content package.
 *
 * Three sets of tables drive the game:
 *
 *   enemy-tables   the flame animation/pose tables (enemy.c)
 *   hero-programs  the two action-program sets         (game.c)
 *   boss-tables    the Boss combat scripts/poses  (objects.c)
 *
 * They live in the package as JSON (`content/base/engine/enemy-tables.json`
 * and its two siblings) and are parsed once at startup by
 * `od_content_vfs_init` (content.c) into a `od_engine_tables`, which the
 * consumers read through `od_engine_tables_get()`.  A mod can override any of
 * the three files by relative path, like any other package file.
 *
 * The JSON is a lossless view of the exact bytes of the tables: the importer
 * emits it, and `tests/test_engine_tables.c` rebuilds the canonical image from
 * the loaded tables and checks its digests.
 */
#ifndef OD_CONTENT_TABLES_H
#define OD_CONTENT_TABLES_H

#include "opendana/common.h"
#include "opendana/content.h"

/* Sizes of the original tables (EU), part of the format. */
#define OD_FLAME_ANIM_COUNT 48
#define OD_HERO_PROG_BYTES 0x9CC
#define OD_ENEMY_H_ANIMS 34
#define OD_ENEMY_V_ANIMS 68
#define OD_ENEMY_R_ANIMS 68
#define OD_BOSS_ATTACK_DATA 199
#define OD_BOSS_ATTACK_OFFSET 32
#define OD_BOSS_CORRIDOR 32
#define OD_BOSS_POSE_DATA 593
/* The extracted retirement blob is ten bytes: five (cell, width) pairs.  Note
 * this is NOT `OD_BOSS_RETIRE_SPANS` (5, objects.h), the pair count. */
#define OD_BOSS_RETIRE_BYTES 10

/* Base address the boss sequences' pose pointers count from: a value stored
 * in the data, not a position in any file. */
#define OD_BOSS_POSE_BASE 0x9D13u

/* One `flame_animation` record: the compiled C had
 * `struct { uint8_t count, wait; uint16_t pose[4]; }`. */
typedef struct {
    uint8_t count;
    uint8_t wait;
    uint16_t pose[4];
} od_flame_anim;

typedef struct {
    uint8_t horizontal_animations[OD_ENEMY_H_ANIMS];
    uint8_t vertical_animations[OD_ENEMY_V_ANIMS];
    uint8_t resistant_animations[OD_ENEMY_R_ANIMS];
    od_flame_anim flame_animation[OD_FLAME_ANIM_COUNT];

    uint8_t hero_prog_clear[OD_HERO_PROG_BYTES];
    uint8_t hero_prog_covered[OD_HERO_PROG_BYTES];

    uint8_t boss_attack_data[OD_BOSS_ATTACK_DATA];
    uint16_t boss_attack_offset[OD_BOSS_ATTACK_OFFSET];
    uint8_t boss_corridor_right[OD_BOSS_CORRIDOR];
    uint8_t boss_corridor_left[OD_BOSS_CORRIDOR];
    uint8_t boss_pose_data[OD_BOSS_POSE_DATA];
    uint8_t boss_retire_spans[OD_BOSS_RETIRE_BYTES];
} od_engine_tables;

/* Parse the three engine documents through `vfs`, in package-mod order.  Fills
 * `*out` completely or returns an error; `err` (optional) gets a short reason.
 * Does not install. */
od_status od_engine_tables_parse(od_engine_tables *out, const od_content_vfs *vfs,
                                   char *err, size_t errcap);

/* Install a copy as the process-wide engine tables (the struct is copied, so
 * the caller may pass a stack value).  Pass NULL to clear. */
void od_engine_tables_install(const od_engine_tables *tables);

/* The installed tables, or, when none are installed, a lazy load from the
 * default package next to the executable.  NULL only when no package with the
 * engine documents is reachable. */
const od_engine_tables *od_engine_tables_get(void);

#endif /* OD_CONTENT_TABLES_H */
