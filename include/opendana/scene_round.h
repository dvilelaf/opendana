/* scene_round.h - ROUND focus of the WORLD map.
 *
 * Split out of `screens.c` for C3.2. The round is the second focus of the
 * original map: `od_scene_round` owns the selected round and the "first
 * uncleared" rule, and moves on the *press edge* of the remapped direction bits
 * (defect A7: the old code moved the round every tick the direction was held).
 * Pure C11, no SDL. */
#ifndef OD_SCENE_ROUND_H
#define OD_SCENE_ROUND_H

#include <stdint.h>

#define OD_MAP_WORLD_COUNT 10
#define OD_MAP_ROUND_COUNT 10

typedef struct {
    int round;   /* 0-based selected round, 0..9 */
} od_scene_round;

/* Clamp `round` into 0..9 and select it. */
void od_scene_round_begin(od_scene_round *r, int round);

/* Move one round on the press edge. `dir` is the original's remapped direction
 * bits of the freshly pressed buttons (bit0 = RIGHT, bit1 = LEFT). The round
 * does not wrap: it stops at 0 and 9, like the reference. Returns 1 when moved. */
int od_scene_round_move(od_scene_round *r, unsigned dir);

/* The round a world resumes at is its first one whose cleared bit is not set;
 * all ten cleared leaves the castle (index 9). `words[w]` is world `w+1`'s
 * progress word (low ten bits = cleared rounds). */
int od_scene_round_first(const uint16_t words[OD_MAP_WORLD_COUNT], int world);

/* Validated level index 0..99, or -1 when world/round are out of range. The
 * map must never hand the app a level outside the ten regular worlds (A5). */
int od_scene_round_level(int world, int round);

/* The round-focus `A` on round 10 of a world whose bit 15 is clear runs that
 * world's castle cinematic (sequence world*2+8) before the level dispatch.
 * Worlds 1..9 (index 0..8) play their castle here; world 10's sequence is the
 * boss sequence (OD_SCREEN_BOSS, H4.10). */
int od_scene_round_castle(const uint16_t words[OD_MAP_WORLD_COUNT], int world,
                           int round);

#endif /* OD_SCENE_ROUND_H */
