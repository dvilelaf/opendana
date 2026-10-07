#include "fsutil.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

int main(int argc, char **argv)
{
    assert(argc == 2);
    char root[4096], other[4096], path[4096], joined[4096];
    snprintf(root, sizeof(root), "%s/fs-XXXXXX", argv[1]);
    snprintf(other, sizeof(other), "%s/fs-XXXXXX", argv[1]);
    assert(od_fs_mkdtemp(root) == 0);
    assert(od_fs_mkdtemp(other) == 0);
    assert(strcmp(root, other) != 0);
    assert(od_fs_mkdtemp(other) != 0); /* never adopt an existing directory */
    od_fs_join(path, sizeof(path), root, "nested/deep/file");
    assert(od_fs_write(path, "hello", 5) == 0);
    assert(od_fs_mkdir_p(path) != 0); /* EEXIST is not enough */
    size_t size;
    char *data = od_fs_read(path, &size);
    assert(data && size == 5 && memcmp(data, "hello", 5) == 0);
    free(data);
    assert(od_fs_remove_r(NULL) != 0);
    assert(od_fs_remove_r("") != 0);
    assert(od_fs_mkdir_parent(NULL) != 0);
    char longpath[5000];
    memset(longpath, 'x', sizeof(longpath) - 1);
    longpath[sizeof(longpath) - 1] = 0;
    assert(od_fs_mkdir_parent(longpath) != 0);
#ifdef _WIN32
    od_fs_join(joined, sizeof(joined), "C:\\temp\\", "file");
    assert(strcmp(joined, "C:\\temp\\file") == 0);
    snprintf(path, sizeof(path), "%s\\back\\slash\\file", root);
    assert(od_fs_write(path, "windows", 7) == 0);
    assert(od_fs_exists(path));
    assert(od_fs_mkdir_p("Z:\\") == 0);
#else
    od_fs_join(path, sizeof(path), root, "link");
    assert(symlink(other, path) == 0);
    assert(od_fs_remove_r(path) == 0);
    assert(od_fs_exists(other));
    (void)joined;
#endif
    assert(od_fs_remove_r(root) == 0);
    assert(od_fs_remove_r(other) == 0);
    assert(od_fs_remove_r(root) == 0);
    puts("windows-fs: all assertions passed");
    return 0;
}
