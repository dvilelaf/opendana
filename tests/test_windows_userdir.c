/* Windows branch of the per-user directories: the save, the settings and the
 * generic userdir API resolve under %LOCALAPPDATA% (data) and %APPDATA%
 * (config); HOME and XDG_* are not consulted. The POSIX counterparts live in
 * test_save.c, test_settings.c and test_session.c and are not built on
 * Windows. Nothing is registered on other platforms. */
#include "test.h"

#ifdef _WIN32
#include "opendana/save.h"
#include "opendana/userdir.h"
#include "opendana/ext/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_env(const char *name, const char *value)
{
    char buf[1200];
    snprintf(buf, sizeof buf, "%s=%s", name, value);
    _putenv(buf);
}

/* Remember a variable; restore (or clear) it when done. */
typedef struct { const char *name; char value[600]; int had; } saved_env;

static void env_save(saved_env *s, const char *name)
{
    const char *v = getenv(name);
    s->name = name;
    s->had = v != NULL && v[0] != '\0';
    snprintf(s->value, sizeof s->value, "%s", s->had ? v : "");
}

static void env_restore(const saved_env *s)
{
    put_env(s->name, s->had ? s->value : "");
}

static void test_windows_save_default_path(void)
{
    printf("- windows: save.dat is under %%LOCALAPPDATA%%\n");
    saved_env local;
    env_save(&local, "LOCALAPPDATA");
    char path[1024];
    put_env("LOCALAPPDATA", "C:/opendana-local");
    od_save_default_path(path, sizeof path);
    CHECK(strcmp(path, "C:/opendana-local/opendana/save.dat") == 0);
    env_restore(&local);
}

static void test_windows_userdir(void)
{
    printf("- windows: data under %%LOCALAPPDATA%%, config under %%APPDATA%%\n");
    saved_env local, roaming;
    env_save(&local, "LOCALAPPDATA");
    env_save(&roaming, "APPDATA");
    char out[1024];
    put_env("LOCALAPPDATA", "C:/c631-local");
    put_env("APPDATA", "C:/c631-roaming");
    CHECK_EQ_U32(od_userdir(OD_USERDIR_DATA, out, sizeof out), OD_OK);
    CHECK(strcmp(out, "C:/c631-local/" OD_USERDIR_APP) == 0);
    CHECK_EQ_U32(od_userdir_path(OD_USERDIR_CONFIG, "settings.ini", out, sizeof out), OD_OK);
    CHECK(strcmp(out, "C:/c631-roaming/" OD_USERDIR_APP "/settings.ini") == 0);
    CHECK_EQ_U32(od_userdir_path(OD_USERDIR_DATA, "states/slot-1.odstate", out, sizeof out), OD_OK);
    CHECK(strcmp(out, "C:/c631-local/" OD_USERDIR_APP "/states/slot-1.odstate") == 0);
    char tiny[8];
    CHECK_EQ_U32(od_userdir(OD_USERDIR_DATA, tiny, sizeof tiny), OD_ERR_RANGE);
    env_restore(&local);
    env_restore(&roaming);
}

static void test_windows_settings_path(void)
{
    printf("- windows: settings.ini under %%APPDATA%%, profile folder as fallback\n");
    saved_env roaming, profile;
    env_save(&roaming, "APPDATA");
    env_save(&profile, "USERPROFILE");
    char path[512];
    put_env("APPDATA", "C:/opendana-c51-roaming");
    od_settings_default_path(path, sizeof path);
    CHECK(strcmp(path, "C:/opendana-c51-roaming/" OD_SETTINGS_APP_DIR "/settings.ini") == 0);

    put_env("APPDATA", "");
    put_env("USERPROFILE", "C:/opendana-c51-profile");
    od_settings_default_path(path, sizeof path);
    CHECK(strcmp(path, "C:/opendana-c51-profile\\AppData\\Roaming/" OD_SETTINGS_APP_DIR
                       "/settings.ini") == 0);
    env_restore(&roaming);
    env_restore(&profile);
}

OD_TEST(test_windows_save_default_path)
OD_TEST(test_windows_userdir)
OD_TEST(test_windows_settings_path)
#endif /* _WIN32 */
