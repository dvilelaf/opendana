/* the ROM chooser child never outlives the game.
 *
 *   import-picker-child-probe MODE
 *
 * A fake `zenity` (first in PATH, written by tests/import-picker-child.sh)
 * records its pid, its open descriptors and two environment variables, then
 * sleeps. Modes:
 *   exit    begin the picker with a descriptor open WITHOUT close-on-exec,
 *           wait until the fake chooser runs, then return from main: the atexit
 *           handler must kill and reap it;
 *   cancel  begin, then od_rom_picker_cancel(): the child is gone and reaped
 *           (waitpid says no such child) before the call returns;
 *   hang    begin and sleep forever (the script kills this process with
 *           SIGKILL: on Linux the kernel must end the chooser, PDEATHSIG).
 */
#include "opendana/rom_picker.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void wait_for_file(const char *path)
{
    for (int i = 0; i < 500 && access(path, F_OK) != 0; i++) {
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    const char *ready = getenv("FAKE_PICKER_READY");
    if (ready == NULL) return 2;
    /* A descriptor the game holds without O_CLOEXEC (e.g. a log or audio fd):
     * the chooser must not inherit it. */
    int leak = open("/dev/null", O_RDONLY);
    printf("probe: leakable fd %d\n", leak);
    od_rom_picker p;
    od_rom_picker_begin(&p);
    if (p.pid <= 0) { printf("probe: no chooser started\n"); return 1; }
    printf("probe: chooser pid %ld\n", p.pid);
    fflush(stdout);
    wait_for_file(ready);
    if (!strcmp(argv[1], "cancel")) {
        od_rom_picker_cancel(&p);
        int status;
        pid_t r = waitpid(-1, &status, WNOHANG);
        printf("probe: after cancel waitpid=%d errno=%d fd=%d\n", (int)r, r < 0 ? errno : 0, p.fd);
        return (r < 0 && errno == ECHILD && p.fd < 0) ? 0 : 1;
    }
    if (!strcmp(argv[1], "hang")) {
        for (;;) pause();
    }
    return 0;   /* "exit": atexit must clean up */
}
