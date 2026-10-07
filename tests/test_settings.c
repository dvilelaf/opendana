/* test_settings.c - unit tests for the settings layer (src/ext). */
#include "test.h"
#include "opendana/ext/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_settings_defaults_case(void)
{
    od_settings s;
    od_settings_defaults(&s);
    CHECK_EQ_U32(s.fullscreen, 0);
    CHECK_EQ_U32(s.integer_scale, 1);
    CHECK_EQ_U32(s.aspect_87, 0);
    CHECK_EQ_U32(s.pause_on_focus_loss, 1);
    CHECK_EQ_U32(s.muted, 0);
    CHECK_EQ_U32(s.volume, 100);
    CHECK_EQ_U32(s.states_slots, 8);
    CHECK_EQ_U32(s.states_current, 0);
    CHECK(strcmp(s.language, "en") == 0);
    CHECK_EQ_U32(s.warnings, 0);
}

static void test_settings_parse_case(void)
{
    const char *ini =
        "[audio]\n"
        "muted = true\n"
        "volume = 40\n"
        "[video]\n"
        "integer_scale = off\n"
        "aspect_87 = 1\n"
        "pause_on_focus_loss = 0\n"
        "[states]\n"
        "slots = 4\n"
        "current = 3\n";
    od_settings s;
    od_settings_defaults(&s);
    CHECK(od_settings_parse(&s, ini, 0) == OD_OK);
    CHECK_EQ_U32(s.muted, 1);
    CHECK_EQ_U32(s.volume, 40);
    CHECK_EQ_U32(s.integer_scale, 0);
    CHECK_EQ_U32(s.aspect_87, 1);
    CHECK_EQ_U32(s.pause_on_focus_loss, 0);
    CHECK_EQ_U32(s.states_slots, 4);
    CHECK_EQ_U32(s.states_current, 3);
    CHECK_EQ_U32(s.warnings, 0);
}

static void test_settings_invalid_case(void)
{
    const char *ini =
        "# comment\n"
        "[audio]\n"
        "volume = 250\n"       /* out of range: ignored */
        "muted = maybe\n"      /* bad bool: ignored */
        "[nope]\n"
        "whatever = 1\n"       /* unknown section */
        "malformed line\n"     /* no '=' */
        "[video]\n"
        "integer_scale = 7\n"  /* bad bool */
        "[states]\n"
        "slots = 0\n"          /* out of range */
        "current = 99\n";      /* out of range */
    od_settings s;
    od_settings_defaults(&s);
    CHECK(od_settings_parse(&s, ini, 0) == OD_OK);
    CHECK_EQ_U32(s.volume, 100);          /* kept its default */
    CHECK_EQ_U32(s.muted, 0);
    CHECK_EQ_U32(s.integer_scale, 1);
    CHECK_EQ_U32(s.states_slots, 8);
    CHECK_EQ_U32(s.states_current, 0);
    CHECK(s.warnings >= 7);               /* every bad entry counted */
}

static void test_settings_roundtrip_case(void)
{
    const char *path = OD_REPO_ROOT "/build/c51-settings-rt.ini";
    od_settings a, b;
    od_settings_defaults(&a);
    a.muted = 1;
    a.volume = 30;
    a.integer_scale = 0;
    a.states_current = 2;
    strcpy(a.language, "en");

    remove(path);
    CHECK(od_settings_save(&a, path) == OD_OK);
    od_settings_defaults(&b);
    CHECK(od_settings_load(&b, path) == OD_OK);
    CHECK_EQ_U32(b.muted, a.muted);
    CHECK_EQ_U32(b.volume, a.volume);
    CHECK_EQ_U32(b.integer_scale, a.integer_scale);
    CHECK_EQ_U32(b.states_current, a.states_current);
    CHECK(strcmp(b.language, a.language) == 0);
    remove(path);

    /* A missing file is not an error and yields the defaults. */
    CHECK(od_settings_load(&b, OD_REPO_ROOT "/build/c51-settings-missing.ini")
          == OD_OK);
    CHECK_EQ_U32(b.volume, 100);
}

static int16_t i16(int v) { return (int16_t)v; }

static void test_settings_gain_case(void)
{
    od_settings s;
    od_settings_defaults(&s);
    CHECK_EQ_U32(od_settings_gain_permille(&s), 1000);

    s.volume = 50;
    CHECK_EQ_U32(od_settings_gain_permille(&s), 500);

    s.muted = 1;
    CHECK_EQ_U32(od_settings_gain_permille(&s), 0);   /* mute wins */

    s.muted = 0;
    s.volume = 0;
    CHECK_EQ_U32(od_settings_gain_permille(&s), 0);

    int16_t buf[4] = { i16(1000), i16(-1000), i16(32767), i16(-32768) };
    od_audio_apply_gain(buf, 4, 0);
    CHECK_EQ_U32(buf[0], 0);
    CHECK_EQ_U32(buf[1], 0);
    CHECK_EQ_U32(buf[2], 0);
    CHECK_EQ_U32(buf[3], 0);

    int16_t half[4] = { i16(1000), i16(-1000), i16(100), i16(-100) };
    od_audio_apply_gain(half, 4, 500);
    CHECK_EQ_U32(half[0], 500);
    CHECK(half[1] == -500);
    CHECK_EQ_U32(half[2], 50);
    CHECK(half[3] == -50);

    int16_t one[1] = { i16(1234) };
    od_audio_apply_gain(one, 1, 1000);
    CHECK_EQ_U32(one[0], 1234);
}

static void test_settings_xdg_path_case(void)
{
    static char env[] = "XDG_CONFIG_HOME=/tmp/opendana-c51-xdg";
    putenv(env);
    char path[512];
    od_settings_default_path(path, sizeof path);
    CHECK(strcmp(path, "/tmp/opendana-c51-xdg/" OD_SETTINGS_APP_DIR "/settings.ini") == 0);

    /* Without XDG_CONFIG_HOME the path falls back to $HOME/.config. */
    static char home_env[] = "HOME=/tmp/opendana-c51-home";
    static char empty_xdg[] = "XDG_CONFIG_HOME=";
    putenv(empty_xdg);
    putenv(home_env);
    od_settings_default_path(path, sizeof path);
    CHECK(strcmp(path, "/tmp/opendana-c51-home/.config/" OD_SETTINGS_APP_DIR
                      "/settings.ini") == 0);

    /* Restore a harmless XDG setting for any later case. */
    putenv((char *)"XDG_CONFIG_HOME=/tmp/opendana-c51-xdg");
}

OD_TEST(test_settings_defaults_case)
OD_TEST(test_settings_parse_case)
OD_TEST(test_settings_invalid_case)
OD_TEST(test_settings_roundtrip_case)
OD_TEST(test_settings_gain_case)
OD_TEST(test_settings_xdg_path_case)
