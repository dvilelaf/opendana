/* test_input_map.c - the pure controller/keyboard mapping.
 *
 * Covers the by-position default, the full button mask, the analogue deadzone
 * with hysteresis, reassignment, and persistence in a settings.ini that keeps
 * every other line intact. */
#include "test.h"

#include "opendana/ext/input_map.h"
#include "opendana/game.h"
#include "opendana/replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static od_pad_state pressed(od_pad_button b)
{
    od_pad_state s;
    memset(&s, 0, sizeof s);
    s.buttons = OD_PAD_BIT(b);
    return s;
}

/* The right button is game A and the bottom button is game B. */
static void test_pad_default_by_position(void)
{
    printf("- the controller maps by position (right = A, bottom = B)\n");
    od_pad_map m;
    od_pad_map_default(&m);
    od_pad_state east = pressed(OD_PAD_EAST);
    od_pad_state south = pressed(OD_PAD_SOUTH);
    CHECK_EQ_U32(od_pad_to_nes(&east, &m), OD_BTN_A);
    CHECK_EQ_U32(od_pad_to_nes(&south, &m), OD_BTN_B);
    /* The labels do not decide: WEST/NORTH are not game A/B by default. */
    od_pad_state west = pressed(OD_PAD_WEST);
    od_pad_state north = pressed(OD_PAD_NORTH);
    CHECK_EQ_U32(od_pad_to_nes(&west, &m), 0u);
    CHECK_EQ_U32(od_pad_to_nes(&north, &m), 0u);
    /* GUIDE is a menu shortcut, not a game button. */
    od_pad_state guide = pressed(OD_PAD_GUIDE);
    CHECK_EQ_U32(od_pad_to_nes(&guide, &m), 0u);
    CHECK_EQ_U32(od_pad_actions(&guide, &m), OD_ACT_BIT(OD_ACT_MENU));
}

/* Every game button must be reachable and the union must be exactly the mask. */
static void test_pad_all_nes_buttons(void)
{
    printf("- the whole 8-button game mask is reachable, nothing else\n");
    od_pad_map m;
    od_pad_map_default(&m);
    unsigned all = OD_BTN_A | OD_BTN_B | OD_BTN_SELECT | OD_BTN_START |
                   OD_BTN_UP | OD_BTN_DOWN | OD_BTN_LEFT | OD_BTN_RIGHT;
    od_pad_state s;
    memset(&s, 0, sizeof s);
    for (int b = 0; b < OD_PAD_COUNT; b++)
        s.buttons |= OD_PAD_BIT((od_pad_button)b);
    CHECK_EQ_U32(od_pad_to_nes(&s, &m), all);
    /* A controller can never produce a bit outside the game mask. */
    CHECK_EQ_U32(od_pad_to_nes(&s, &m) & ~all, 0u);
}

/* Left stick -> d-pad with deadzone 0.5 and hysteresis 0.1. */
static void test_pad_deadzone_hysteresis(void)
{
    printf("- left stick deadzone 0.5 and hysteresis 0.1\n");
    od_pad_map m;
    od_pad_map_default(&m);
    od_pad_state s;
    memset(&s, 0, sizeof s);
    /* 0.80 to the right: engaged. */
    s.axis_left_x = 26000;      /* ~0.79 */
    od_pad_resolve_axes(&s, &m);
    CHECK_EQ_U32(s.axis_dpad, OD_BTN_RIGHT);
    /* 0.55 crosses the 0.5 engage threshold. */
    s.axis_left_x = 18000;      /* ~0.55 */
    od_pad_resolve_axes(&s, &m);
    CHECK_EQ_U32(s.axis_dpad, OD_BTN_RIGHT);
    /* 0.45 is below deadzone but above release (0.4): kept by hysteresis. */
    s.axis_left_x = 15000;      /* ~0.458 */
    od_pad_resolve_axes(&s, &m);
    CHECK_EQ_U32(s.axis_dpad, OD_BTN_RIGHT);
    /* 0.35 is below the release threshold: off. */
    s.axis_left_x = 11000;      /* ~0.336 */
    od_pad_resolve_axes(&s, &m);
    CHECK_EQ_U32(s.axis_dpad, 0u);
    /* And it must not re-engage until 0.5 again. */
    s.axis_left_x = 15000;
    od_pad_resolve_axes(&s, &m);
    CHECK_EQ_U32(s.axis_dpad, 0u);
    s.axis_left_x = 18000;
    od_pad_resolve_axes(&s, &m);
    CHECK_EQ_U32(s.axis_dpad, OD_BTN_RIGHT);
    /* Up is negative Y. */
    s.axis_left_x = 0;
    s.axis_left_y = -18000;
    od_pad_resolve_axes(&s, &m);
    CHECK_EQ_U32(s.axis_dpad, OD_BTN_UP);
    /* Disabled axis_dpad ignores the stick entirely. */
    m.axis_dpad = 0;
    od_pad_resolve_axes(&s, &m);
    CHECK_EQ_U32(s.axis_dpad, 0u);
    CHECK_EQ_U32(od_pad_to_nes(&s, &m), 0u);
}

/* Reassignment, and the map stays a function (one control per destination). */
static void test_pad_reassign(void)
{
    printf("- reassignment swaps and never duplicates a destination\n");
    od_pad_map m;
    od_pad_map_default(&m);
    od_binding b;
    od_binding_set_nes(&b, OD_BTN_A);
    od_pad_rebind(&m, OD_PAD_SOUTH, &b);       /* south becomes A */
    CHECK_EQ_U32(m.bind[OD_PAD_SOUTH].value, OD_BTN_A);
    CHECK_EQ_U32(m.bind[OD_PAD_EAST].kind, OD_BIND_NONE);   /* old A cleared */
    od_binding_set_nes(&b, OD_BTN_B);
    od_pad_rebind(&m, OD_PAD_EAST, &b);        /* east becomes B -> swap */
    CHECK_EQ_U32(m.bind[OD_PAD_EAST].value, OD_BTN_B);
    od_pad_state south = pressed(OD_PAD_SOUTH);
    od_pad_state east = pressed(OD_PAD_EAST);
    CHECK_EQ_U32(od_pad_to_nes(&south, &m), OD_BTN_A);
    CHECK_EQ_U32(od_pad_to_nes(&east, &m), OD_BTN_B);
    /* A third button taking the same function clears the previous owner. */
    od_binding_set_nes(&b, OD_BTN_A);
    od_pad_rebind(&m, OD_PAD_START, &b);
    CHECK_EQ_U32(m.bind[OD_PAD_SOUTH].kind, OD_BIND_NONE);
    CHECK_EQ_U32(m.bind[OD_PAD_START].value, OD_BTN_A);
    /* An action binding clears a previous action owner but no game-button binding. */
    od_binding_set_action(&b, OD_ACT_MENU);
    od_pad_rebind(&m, OD_PAD_NORTH, &b);
    CHECK_EQ_U32(m.bind[OD_PAD_GUIDE].kind, OD_BIND_NONE);
    CHECK_EQ_U32(m.bind[OD_PAD_START].kind, OD_BIND_NES);
    /* Unbinding. */
    od_binding_clear(&b);
    od_pad_rebind(&m, OD_PAD_START, &b);
    CHECK_EQ_U32(m.bind[OD_PAD_START].kind, OD_BIND_NONE);
}

/* settings.ini: load, save, preserve the rest of the file, reload. */
static void test_persistence(void)
{
    printf("- bindings persist in settings.ini and other lines survive\n");
    char path[] = "/tmp/opendana-input-map-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    close(fd);
    const char *seed =
        "[video]\nfullscreen = 0\nscale = 3\n\n"
        "[input]\n"
        "keyboard_layout = es\n"          /* an unknown key of the same section */
        "pad_south = none\n";             /* override one default */
    FILE *fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (fp != NULL) {
        fwrite(seed, 1, strlen(seed), fp);
        fclose(fp);
    }

    od_input_config c;
    od_input_config_load(&c, path);
    /* The file's override wins... */
    CHECK_EQ_U32(c.pad.bind[OD_PAD_SOUTH].kind, OD_BIND_NONE);
    /* ...and the other defaults still stand. */
    CHECK_EQ_U32(c.pad.bind[OD_PAD_EAST].value, OD_BTN_A);
    CHECK_EQ_U32(c.pad.deadzone, 500);

    /* Swap the two face buttons, like the Settings screen would. */
    od_binding b;
    od_binding_set_nes(&b, OD_BTN_A);
    od_pad_rebind(&c.pad, OD_PAD_SOUTH, &b);
    od_binding_set_nes(&b, OD_BTN_B);
    od_pad_rebind(&c.pad, OD_PAD_EAST, &b);
    c.pad.deadzone = 420;
    CHECK_EQ_U32(od_input_config_save(&c, path), OD_OK);

    /* The unknown lines are still there. */
    size_t len = 0;
    char *raw = NULL;
    fp = fopen(path, "rb");
    CHECK(fp != NULL);
    if (fp != NULL) {
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        rewind(fp);
        raw = malloc((size_t)n + 1);
        if (raw != NULL) {
            len = fread(raw, 1, (size_t)n, fp);
            raw[len] = '\0';
        }
        fclose(fp);
    }
    CHECK(raw != NULL);
    if (raw != NULL) {
        CHECK(strstr(raw, "[video]") != NULL);
        CHECK(strstr(raw, "fullscreen = 0") != NULL);
        CHECK(strstr(raw, "scale = 3") != NULL);
        CHECK(strstr(raw, "keyboard_layout = es") != NULL);
        CHECK(strstr(raw, "pad_south = nes_a") != NULL);
        CHECK(strstr(raw, "pad_east = nes_b") != NULL);
        CHECK(strstr(raw, "pad_deadzone = 420") != NULL);
        free(raw);
    }

    /* A fresh process loading that file reproduces the reassignment. */
    od_input_config c2;
    od_input_config_load(&c2, path);
    CHECK_EQ_U32(c2.pad.bind[OD_PAD_SOUTH].value, OD_BTN_A);
    CHECK_EQ_U32(c2.pad.bind[OD_PAD_EAST].value, OD_BTN_B);
    CHECK_EQ_U32(c2.pad.deadzone, 420);
    od_pad_state south = pressed(OD_PAD_SOUTH);
    CHECK_EQ_U32(od_pad_to_nes(&south, &c2.pad), OD_BTN_A);

    /* A missing file just leaves the defaults. */
    od_input_config c3;
    CHECK_EQ_U32(od_input_config_load(&c3, "/tmp/opendana-input-map-does-not-exist"),
                 OD_OK);
    CHECK_EQ_U32(c3.pad.bind[OD_PAD_EAST].value, OD_BTN_A);
    unlink(path);
}

/* The keyboard defaults match the game's historic keys. */
static void test_keyboard_defaults(void)
{
    printf("- keyboard defaults and action rising edges\n");
    od_key_map m;
    od_key_map_default(&m);
    CHECK_EQ_U32(od_keys_to_nes(OD_KEY_BIT(OD_KEY_A), &m), OD_BTN_A);
    CHECK_EQ_U32(od_keys_to_nes(OD_KEY_BIT(OD_KEY_B), &m), OD_BTN_B);
    CHECK_EQ_U32(od_keys_to_nes(OD_KEY_BIT(OD_KEY_START) |
                                 OD_KEY_BIT(OD_KEY_SELECT), &m),
                 OD_BTN_START | OD_BTN_SELECT);
    CHECK_EQ_U32(od_keys_actions(OD_KEY_BIT(OD_KEY_MUTE), &m),
                 OD_ACT_BIT(OD_ACT_MUTE));
    /* Rising edge only: a held key is not an action twice. */
    unsigned cur = OD_ACT_BIT(OD_ACT_MENU);
    od_action out[8];
    CHECK_EQ_U32(od_actions_flank(cur, 0, out, 8), 1);
    CHECK_EQ_U32(out[0], OD_ACT_MENU);
    CHECK_EQ_U32(od_actions_flank(cur, cur, out, 8), 0);
    CHECK_EQ_U32(od_actions_flank(cur | OD_ACT_BIT(OD_ACT_MUTE), cur, out, 8), 1);
    CHECK_EQ_U32(out[0], OD_ACT_MUTE);
    /* Keyboard reassignment uses the same pure model. */
    od_key_map km;
    od_key_map_default(&km);
    od_binding b;
    od_binding_set_nes(&b, OD_BTN_A);
    od_key_rebind(&km, OD_KEY_B, &b);
    CHECK_EQ_U32(od_keys_to_nes(OD_KEY_BIT(OD_KEY_B), &km), OD_BTN_A);
    /* The previous A owner (key A) was cleared. */
    CHECK_EQ_U32(km.bind[OD_KEY_A].kind, OD_BIND_NONE);
}

OD_TEST(test_pad_default_by_position)
OD_TEST(test_pad_all_nes_buttons)
OD_TEST(test_pad_deadzone_hysteresis)
OD_TEST(test_pad_reassign)
OD_TEST(test_persistence)
OD_TEST(test_keyboard_defaults)
