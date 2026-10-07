/* test_ui_strings.c - the English string table of the modern interface (D6). */
#include "test.h"
#include "opendana/ui_strings.h"

#include <string.h>

static void test_ui_strings_case(void)
{
    /* Every id resolves to a non-NULL English string. */
    for (int i = 0; i < OD_STR_COUNT; i++)
        CHECK(od_str((od_string)i) != NULL);

    CHECK(strcmp(od_str(OD_STR_QUIT), "Quit") == 0);
    CHECK(strcmp(od_str(OD_STR_MENU_TITLE), "MENU") == 0);
    CHECK(strcmp(od_str(OD_STR_CONTINUE), "Continue") == 0);
    CHECK(strcmp(od_str(OD_STR_SETTINGS), "Settings") == 0);
    CHECK(strcmp(od_str(OD_STR_TOAST_MUTED), "Muted") == 0);
    CHECK(strcmp(od_str(OD_STR_ON), "On") == 0);
    CHECK(strcmp(od_str_bool(0), "Off") == 0);
    CHECK(strcmp(od_str_bool(1), "On") == 0);

    /* Out-of-range ids return the empty string, never NULL. */
    CHECK(od_str((od_string)-1) != NULL);
    CHECK(od_str((od_string)OD_STR_COUNT) != NULL);
}

OD_TEST(test_ui_strings_case)
