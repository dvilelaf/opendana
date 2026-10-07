/* Hidden levels 100..149: the hidden menu model (SELECT+B x10 on the title,
 * ROUND 11-1..15-10 + SOUND 00, A/B/SELECT navigation, START) and the
 * progression coherence for all 150 levels. Pure C11, no SDL. */
#include "test.h"

#include "opendana/worldmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- title trigger ----------------------------------------------------- */

static void test_hidden_title_trigger(void)
{
    printf("- worldmap: hidden menu title trigger\n");
    od_hidden_menu h;
    od_hidden_reset(&h);
    CHECK_EQ_U32(h.active, 0);

    /* SELECT is held from the first tick; the first tick only arms the
     * counter and returns before counting. */
    CHECK_EQ_U32(od_hidden_title_tick(&h, OD_BTN_SELECT, 0), 0);
    CHECK_EQ_U32(h.counting, 1);
    CHECK_EQ_U32(h.presses, 0);

    /* Eight B edges are not enough. */
    for (int i = 0; i < 8; i++) {
        CHECK_EQ_U32(od_hidden_title_tick(&h, OD_BTN_SELECT,
                                           OD_BTN_B), 0);
    }
    CHECK_EQ_U32(h.presses, 8);
    CHECK_EQ_U32(h.active, 0);

    /* The ninth still does not fire; the tenth does. */
    CHECK_EQ_U32(od_hidden_title_tick(&h, OD_BTN_SELECT, OD_BTN_B), 0);
    CHECK_EQ_U32(od_hidden_title_tick(&h, OD_BTN_SELECT, OD_BTN_B), 1);
    CHECK_EQ_U32(h.active, 1);
    CHECK_EQ_U32(h.level, OD_HIDDEN_FIRST);
    CHECK_EQ_U32(h.sound, 0);

    /* No SELECT: B alone never fires. */
    od_hidden_reset(&h);
    for (int i = 0; i < 20; i++)
        CHECK_EQ_U32(od_hidden_title_tick(&h, OD_BTN_B, OD_BTN_B), 0);
    CHECK_EQ_U32(h.active, 0);

    /* Releasing SELECT disarms; the next hold must count ten fresh presses. */
    od_hidden_reset(&h);
    od_hidden_title_tick(&h, OD_BTN_SELECT, 0);
    for (int i = 0; i < 5; i++)
        od_hidden_title_tick(&h, OD_BTN_SELECT, OD_BTN_B);
    CHECK_EQ_U32(h.presses, 5);
    CHECK_EQ_U32(od_hidden_title_tick(&h, 0, 0), 0); /* SELECT released */
    CHECK_EQ_U32(h.counting, 0);
    CHECK_EQ_U32(h.presses, 0);
    od_hidden_title_tick(&h, OD_BTN_SELECT, 0);
    for (int i = 0; i < 9; i++)
        CHECK_EQ_U32(od_hidden_title_tick(&h, OD_BTN_SELECT,
                                           OD_BTN_B), 0);
    CHECK_EQ_U32(h.active, 0);
    CHECK_EQ_U32(od_hidden_title_tick(&h, OD_BTN_SELECT, OD_BTN_B), 1);
}

/* ---- menu navigation --------------------------------------------------- */

static void test_hidden_menu_navigation(void)
{
    printf("- worldmap: hidden menu A/B/SELECT + START\n");
    od_hidden_menu h;
    od_hidden_reset(&h);

    /* Not active: the menu tick does nothing and never confirms. */
    CHECK_EQ_U32(od_hidden_menu_tick(&h, 0, OD_BTN_START), -1);

    h.active = 1;
    h.level = OD_HIDDEN_FIRST;
    h.sound = 0;

    /* A/B step one level, wrapping across the whole 100..149 range. */
    CHECK_EQ_U32(od_hidden_menu_tick(&h, 0, OD_BTN_A), -1);
    CHECK_EQ_U32(h.level, 101);
    CHECK_EQ_U32(od_hidden_menu_tick(&h, 0, OD_BTN_B), -1);
    CHECK_EQ_U32(h.level, 100);
    CHECK_EQ_U32(od_hidden_menu_tick(&h, 0, OD_BTN_B), -1);
    CHECK_EQ_U32(h.level, OD_HIDDEN_LAST); /* 100 - 1 wraps to 149 */
    CHECK_EQ_U32(od_hidden_menu_tick(&h, 0, OD_BTN_A), -1);
    CHECK_EQ_U32(h.level, OD_HIDDEN_FIRST); /* 149 + 1 wraps to 100 */

    /* A/B are press edges, not auto-repeat (measured: holding A changes the
     * level exactly once). */
    h.level = 100;
    for (int i = 0; i < 100; i++)
        od_hidden_menu_tick(&h, 0, 0); /* held, no new edge */
    CHECK_EQ_U32(h.level, 100);

    /* SELECT+A/B jump a whole world (10), wrapping 100..149. */
    CHECK_EQ_U32(od_hidden_menu_tick(&h, OD_BTN_SELECT, OD_BTN_A),
                 -1);
    CHECK_EQ_U32(h.level, 110);
    CHECK_EQ_U32(od_hidden_menu_tick(&h, OD_BTN_SELECT, OD_BTN_B),
                 -1);
    CHECK_EQ_U32(h.level, 100);
    CHECK_EQ_U32(od_hidden_menu_tick(&h, OD_BTN_SELECT, OD_BTN_B),
                 -1);
    CHECK_EQ_U32(h.level, 140); /* worlds wrap 11 -> 15 */

    /* The SOUND value the original shows stays 00 through the navigation. */
    CHECK_EQ_U32(h.sound, 0);

    /* START confirms the current level. */
    h.level = 105;
    CHECK_EQ_U32(od_hidden_menu_tick(&h, 0, OD_BTN_START), 105);
    /* A later tick without START does not confirm again. */
    CHECK_EQ_U32(od_hidden_menu_tick(&h, 0, 0), -1);
}

/* ---- labels ------------------------------------------------------------ */

static void test_hidden_round_text(void)
{
    printf("- worldmap: hidden ROUND labels\n");
    struct { int level; const char *text; int world, round; } cases[] = {
        { 100, "11-1",  11, 1  },
        { 105, "11-6",  11, 6  },
        { 109, "11-10", 11, 10 },
        { 110, "12-1",  12, 1  },
        { 120, "13-1",  13, 1  },
        { 140, "15-1",  15, 1  },
        { 149, "15-10", 15, 10 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char got[16];
        od_hidden_round_text(got, sizeof(got), cases[i].level);
        CHECK(strcmp(got, cases[i].text) == 0 && cases[i].text[0] != '\0');
        int w = 0, r = 0;
        od_level_world_round(cases[i].level, &w, &r);
        CHECK_EQ_U32(w, cases[i].world);
        CHECK_EQ_U32(r, cases[i].round);
        CHECK_EQ_U32(od_level_hidden(cases[i].level), 1);
    }
    CHECK_EQ_U32(od_level_hidden(99), 0);
    CHECK_EQ_U32(od_level_hidden(150), 0);
}

/* ---- progression coherence for all 150 -------------------------------- */

static void test_progress_covers_hidden(void)
{
    printf("- worldmap: progression supports the 150 levels\n");
    od_progress p;
    od_progress_reset(&p);

    /* Clear every normal round up to 10-9. */
    for (int i = 0; i < OD_HIDDEN_FIRST - 1; i++)
        od_progress_set_cleared(&p, i, 1);
    CHECK_EQ_U32(od_progress_unlocked(&p, 99), 1);
    CHECK_EQ_U32(od_progress_unlocked(&p, 100), 0);

    /* Completing 10-10 unlocks 11-1 but the map cursor stays on 10-10. */
    p.world = 9;
    p.cursor = 9;
    od_progress_complete(&p, 99);
    CHECK_EQ_U32(od_progress_cleared(&p, 99), 1);
    CHECK_EQ_U32(od_progress_unlocked(&p, 100), 1);
    CHECK_EQ_U32(p.world, 9);
    CHECK_EQ_U32(p.cursor, 9);

    /* Clearing a hidden level is recorded and unlocks the next hidden one;
     * the map cursor never moves past 10-10. */
    od_progress_complete(&p, 100);
    CHECK_EQ_U32(od_progress_cleared(&p, 100), 1);
    CHECK_EQ_U32(od_progress_unlocked(&p, 101), 1);
    CHECK_EQ_U32(p.world, 9);
    CHECK_EQ_U32(p.cursor, 9);

    /* Every hidden level can be marked, and they all fit the bitset. */
    for (int i = OD_HIDDEN_FIRST; i <= OD_HIDDEN_LAST; i++)
        od_progress_complete(&p, i);
    CHECK_EQ_U32(od_progress_clear_count(&p), OD_LEVEL_COUNT);
    CHECK_EQ_U32(od_progress_cleared(&p, OD_HIDDEN_LAST), 1);

    /* The map markers still only address the ten normal worlds. */
    CHECK_EQ_U32(od_progress_marker_state(&p, 0), 2);
}

OD_TEST(test_hidden_title_trigger)
OD_TEST(test_hidden_menu_navigation)
OD_TEST(test_hidden_round_text)
OD_TEST(test_progress_covers_hidden)
