#include "opendana/import.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int main(void)
{
    char path[4096];
#ifdef _WIN32
    _putenv("LOCALAPPDATA=C:\\Users\\player\\AppData\\Local");
    _putenv("HOME=");
    _putenv("XDG_DATA_HOME=");
    assert(od_import_default_dir(path, sizeof(path), NULL) == OD_OK);
    assert(strcmp(path, "C:\\Users\\player\\AppData\\Local/opendana/content/base") == 0);
#else
    setenv("HOME", "/home/player", 1);
    unsetenv("XDG_DATA_HOME");
    assert(od_import_default_dir(path, sizeof(path), NULL) == OD_OK);
    assert(strcmp(path, "/home/player/.local/share/opendana/content/base") == 0);
    setenv("XDG_DATA_HOME", "/custom", 1);
    assert(od_import_default_dir(path, sizeof(path), NULL) == OD_OK);
    assert(strcmp(path, "/custom/opendana/content/base") == 0);
#endif
    assert(od_import_default_dir(path, 3, NULL) == OD_ERR_RANGE);
    assert(od_import_default_dir(path, sizeof(path), "override") == OD_OK);
    assert(strcmp(path, "override") == 0);
    puts("windows-import-dir: all assertions passed");
    return 0;
}
