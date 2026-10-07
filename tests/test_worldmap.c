/* World map progression: the unlock and marker model. Pure C11, no SDL. */
#include "test.h"

#include "opendana/worldmap.h"

#include <stdio.h>

static void test_progress_model(void)
{
    printf("- worldmap: progression model\n");
    od_progress p;
    od_progress_reset(&p);
    CHECK_EQ_U32(od_progress_clear_count(&p), 0);
    CHECK_EQ_U32(p.world, 0);
    CHECK_EQ_U32(p.cursor, 0);
    CHECK_EQ_U32(od_progress_unlocked(&p, 0), 1);
    CHECK_EQ_U32(od_progress_unlocked(&p, 1), 0);
    CHECK_EQ_U32(od_progress_marker_state(&p, 0), 1); /* round 1 open */
    CHECK_EQ_U32(od_progress_marker_state(&p, 1), 0); /* round 2 locked */

    int lv = -1;
    CHECK_EQ_U32(od_progress_enter(&p, &lv), 0);
    CHECK_EQ_U32(lv, 0);

    /* Entering a locked round is refused. */
    p.cursor = 5;
    CHECK_EQ_U32(od_progress_enter(&p, &lv), -1);
    p.cursor = 0;

    /* Completing a level marks it and unlocks the next round. */
    od_progress_complete(&p, 0);
    CHECK_EQ_U32(od_progress_cleared(&p, 0), 1);
    CHECK_EQ_U32(od_progress_clear_count(&p), 1);
    CHECK_EQ_U32(p.cursor, 1);
    CHECK_EQ_U32(od_progress_unlocked(&p, 1), 1);
    CHECK_EQ_U32(od_progress_marker_state(&p, 0), 2); /* cleared */
    CHECK_EQ_U32(od_progress_marker_state(&p, 1), 1); /* open    */
    CHECK_EQ_U32(od_progress_marker_state(&p, 2), 0); /* locked  */

    /* The cursor is clamped, not wrapped, at both ends. */
    p.cursor = OD_MAP_SLOTS - 1;
    CHECK_EQ_U32(od_progress_move(&p, +1, 0), 0);
    CHECK_EQ_U32(p.cursor, OD_MAP_SLOTS - 1);
    p.cursor = 0;
    CHECK_EQ_U32(od_progress_move(&p, -1, 0), 0);
    CHECK_EQ_U32(p.cursor, 0);
    CHECK_EQ_U32(od_progress_move(&p, +1, 0), 1);
    CHECK_EQ_U32(p.cursor, 1);

    /* World navigation only opens a world whose first round is unlocked. */
    od_progress_reset(&p);
    p.cursor = 0;
    CHECK_EQ_U32(od_progress_move(&p, 0, +1), 0);
    CHECK_EQ_U32(p.world, 0);
    for (int i = 0; i < OD_WORLD_ROUNDS; i++)
        od_progress_set_cleared(&p, i, 1);
    CHECK_EQ_U32(od_progress_unlocked(&p, OD_WORLD_ROUNDS), 1);
    CHECK_EQ_U32(od_progress_move(&p, 0, +1), 1);
    CHECK_EQ_U32(p.world, 1);
    CHECK_EQ_U32(od_progress_move(&p, 0, +1), 0); /* world 3 still locked */

    /* Completing the last round of a world jumps to the next world. */
    od_progress q;
    od_progress_reset(&q);
    for (int i = 0; i < OD_WORLD_ROUNDS - 1; i++)
        od_progress_set_cleared(&q, i, 1);
    od_progress_complete(&q, OD_WORLD_ROUNDS - 1);
    CHECK_EQ_U32(q.world, 1);
    CHECK_EQ_U32(q.cursor, 0);

    /* Hidden levels unlock after the 100 normal rounds. */
    od_progress_reset(&q);
    for (int i = 0; i < OD_HIDDEN_FIRST - 1; i++)
        od_progress_set_cleared(&q, i, 1);
    CHECK_EQ_U32(od_progress_unlocked(&q, 99), 1);
    CHECK_EQ_U32(od_progress_unlocked(&q, 100), 0);
    od_progress_set_cleared(&q, 99, 1);
    CHECK_EQ_U32(od_progress_unlocked(&q, 100), 1);
    /* Completing level 99 does not move the map cursor onto a hidden level. */
    q.world = 9;
    q.cursor = 9;
    od_progress_complete(&q, 99);
    CHECK_EQ_U32(q.world, 9);
    CHECK_EQ_U32(q.cursor, 9);
}

OD_TEST(test_progress_model)
