/* the save, states and settings paths a Windows build
 * resolves. tests/session-run.sh cross-compiles it with mingw and runs it
 * under wine with HOME/XDG unset: before the fix save.dat fell back to the
 * current directory ("opendana.sav"); now every path is under %LOCALAPPDATA% or
 * %APPDATA%. Prints one "name path" line each. */
#include "opendana/ext/settings.h"
#include "opendana/save.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef SESSION_HAVE_USERDIR
#include "opendana/userdir.h"
#endif

#ifdef SESSION_STUB_PROGRESS
#include <string.h>
/* save.c needs only this from worldmap.c (which pulls the font and CHR). */
void od_progress_reset(od_progress *p)
{
    memset(p, 0, sizeof *p);
}
#endif

int main(void)
{
    char path[2048];
    /* What the process sees (wine fills these from its own profile). */
    const char *local = getenv("LOCALAPPDATA"), *roaming = getenv("APPDATA");
    printf("env-local %s\n", local ? local : "(unset)");
    printf("env-roaming %s\n", roaming ? roaming : "(unset)");
    od_save_default_path(path, sizeof path);
    printf("save %s\n", path);
    od_settings_default_path(path, sizeof path);
    printf("settings %s\n", path);
#ifdef SESSION_HAVE_USERDIR
    if (od_userdir_path(OD_USERDIR_DATA, "states/slot-1.odstate", path, sizeof path) != OD_OK)
        snprintf(path, sizeof path, "(none)");
    printf("states %s\n", path);
#endif
    return 0;
}
