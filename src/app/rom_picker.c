/* rom_picker.c - system file chooser for the one-time ROM import (C6.21).
 * See include/opendana/rom_picker.h. Non-blocking on POSIX: fork/exec (no shell) and
 * a non-blocking pipe the front end polls; the Win32 common dialog is modal by
 * nature and is reported as an immediate terminal result. It remembers the last
 * folder so "Open ROM..." reopens where the player left off.
 *
 * C6.29 (C6.22 UI-20, PUB-8):
 *   - the child is killed and reaped when the game exits (atexit), when it is
 *     cancelled, and by the kernel if the game dies first (PR_SET_PDEATHSIG);
 *   - nothing that is not async-signal-safe runs between fork and exec: the
 *     environment, the argument vector and the paths are built before fork(),
 *     the pipe and /dev/null are O_CLOEXEC and the child closes every other
 *     inherited descriptor;
 *   - on Windows the dialog is GetOpenFileNameW and every path is UTF-8 on our
 *     side, so a ROM under a folder named with "ñ" or "日本" is chosen and then
 *     opened by the importer with _wfopen. */
#if !defined(_WIN32)
#define _GNU_SOURCE
#endif

#include "opendana/rom_picker.h"
#include "opendana/userdir.h"

#include "../import/fsutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
extern char **environ;
#endif

#define OD_PICK_TITLE "Select your Solomon's Key 2 (EU) ROM"

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0)
        snprintf(err, cap, "%s", msg);
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

/* --- remembered folder ---------------------------------------------------- */
/* Path of the small file that stores the last chosen folder. */
static int remember_file(char *out, size_t cap)
{
    /* C6.25: the per-user folder comes from the one helper (C6.31), UTF-8
     * on every platform (C6.36). */
    if (od_userdir_path(OD_USERDIR_DATA, "last-rom-dir", out, cap) != OD_OK)
        return 0;
    return 1;
}

int od_rom_picker_last_dir(char *out, size_t cap)
{
    if (out == NULL || cap == 0)
        return 0;
    out[0] = '\0';
    char f[1200];
    if (!remember_file(f, sizeof f))
        return 0;
    FILE *fp = od_fs_fopen(f, "r");
    if (fp == NULL)
        return 0;
    char line[1024] = {0};
    char *got = fgets(line, sizeof line, fp);
    fclose(fp);
    if (got == NULL)
        return 0;
    trim(line);
    if (line[0] == '\0')
        return 0;
    snprintf(out, cap, "%s", line);
    return 1;
}

void od_rom_picker_remember(const char *chosen_path)
{
    if (chosen_path == NULL || chosen_path[0] == '\0')
        return;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", chosen_path);
    char *slash = strrchr(dir, '/');
#if defined(_WIN32)
    char *bs = strrchr(dir, '\\');
    if (bs != NULL && (slash == NULL || bs > slash))
        slash = bs;
#endif
    if (slash != NULL) {
        if (slash == dir)
            slash[1] = '\0';        /* keep "/" */
        else
            *slash = '\0';
    } else {
        snprintf(dir, sizeof dir, ".");
    }
    char f[1200];
    if (!remember_file(f, sizeof f))
        return;
    /* Best effort: the directory usually already exists (the import created
     * it); if not, make it so the small file can be written. */
    (void)od_fs_mkdir_parent(f);
    FILE *fp = od_fs_fopen(f, "w");
    if (fp == NULL)
        return;
    fprintf(fp, "%s\n", dir);
    fclose(fp);
}

#if defined(_WIN32)
/* --- Windows: the Win32 common open-file dialog (modal, UTF-16) ----------- */
static od_pick_result win_open(const char *start, char *out, size_t cap,
                                char *err, size_t errcap)
{
    if (out == NULL || cap == 0)
        return OD_PICK_ERROR;
    static wchar_t file[4096];
    static wchar_t wstart[4096];
    memset(file, 0, sizeof file);
    memset(wstart, 0, sizeof wstart);
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = L"NES ROM (*.nes)\0*.nes\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)(sizeof file / sizeof file[0]);
    ofn.lpstrTitle = L"Select your Solomon's Key 2 (EU) ROM";
    if (start != NULL && start[0] != '\0' &&
        MultiByteToWideChar(CP_UTF8, 0, start, -1, wstart,
                            (int)(sizeof wstart / sizeof wstart[0])) > 0)
        ofn.lpstrInitialDir = wstart;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        /* UTF-16 -> UTF-8: the importer opens it with _wfopen (fsutil.c). */
        int n = WideCharToMultiByte(CP_UTF8, 0, file, -1, out, (int)cap, NULL, NULL);
        if (n <= 0) {
            set_err(err, errcap, "the chosen path is too long");
            return OD_PICK_ERROR;
        }
        return OD_PICK_OK;
    }
    if (CommDlgExtendedError() == 0)
        return OD_PICK_CANCEL;
    set_err(err, errcap, "the Windows file dialog failed");
    return OD_PICK_ERROR;
}

void od_rom_picker_begin_at(od_rom_picker *p, const char *start_dir)
{
    if (p == NULL)
        return;
    memset(p, 0, sizeof(*p));
    p->fd = -1;
    if (start_dir != NULL)
        snprintf(p->start, sizeof p->start, "%s", start_dir);
}

void od_rom_picker_begin(od_rom_picker *p)
{
    od_rom_picker_begin_at(p, NULL);
}

void od_rom_picker_cancel(od_rom_picker *p)
{
    (void)p;   /* the Win32 dialog is modal: nothing outlives the call */
}

od_pick_result od_rom_picker_poll(od_rom_picker *p, char *out, size_t cap,
                                    char *err, size_t errcap)
{
    if (p == NULL)
        return OD_PICK_ERROR;
    p->pid = -1;   /* mark as consumed so a second poll does not reopen */
    return win_open(p->start, out, cap, err, errcap);
}

od_pick_result od_rom_picker_open(char *out, size_t cap, char *err, size_t errcap)
{
    return win_open(NULL, out, cap, err, errcap);
}
#else
/* --- POSIX: fork/exec a helper, poll it without blocking ------------------ */
static int find_in_path(const char *name, char *out, size_t cap)
{
    const char *path = getenv("PATH");
    if (path == NULL)
        path = "/usr/local/bin:/usr/bin:/bin";
    char dir[1024];
    while (*path != '\0') {
        const char *colon = strchr(path, ':');
        size_t n = colon ? (size_t)(colon - path) : strlen(path);
        if (n == 0)
            snprintf(dir, sizeof dir, ".");
        else if (n >= sizeof dir)
            n = sizeof dir - 1, memcpy(dir, path, n), dir[n] = '\0';
        else
            memcpy(dir, path, n), dir[n] = '\0';
        char cand[1200];
        snprintf(cand, sizeof cand, "%s/%s", dir, name);
        if (access(cand, X_OK) == 0) {
            if (strlen(cand) >= cap)
                return 0;
            snprintf(out, cap, "%s", cand);
            return 1;
        }
        if (colon == NULL)
            break;
        path = colon + 1;
    }
    return 0;
}

/* Live chooser children, killed and reaped at exit (UI-20). */
#define OD_PICK_MAX_CHILDREN 8
static pid_t g_children[OD_PICK_MAX_CHILDREN];
static int g_atexit_installed;

static void kill_and_reap(pid_t pid)
{
    if (pid <= 0)
        return;
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == pid)
        return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 50; i++) {          /* up to ~0.5 s to close nicely */
        if (waitpid(pid, &status, WNOHANG) == pid)
            return;
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    (void)waitpid(pid, &status, 0);
}

static void forget_child(pid_t pid)
{
    for (int i = 0; i < OD_PICK_MAX_CHILDREN; i++)
        if (g_children[i] == pid)
            g_children[i] = 0;
}

static void kill_children_at_exit(void)
{
    for (int i = 0; i < OD_PICK_MAX_CHILDREN; i++) {
        kill_and_reap(g_children[i]);
        g_children[i] = 0;
    }
}

static void remember_child(pid_t pid)
{
    if (!g_atexit_installed) {
        g_atexit_installed = 1;
        atexit(kill_children_at_exit);
    }
    for (int i = 0; i < OD_PICK_MAX_CHILDREN; i++) {
        if (g_children[i] == 0) {
            g_children[i] = pid;
            return;
        }
    }
}

/* environ + the two chooser variables, built BEFORE fork(): setenv/getenv are
 * not async-signal-safe and the game is multithreaded (SDL audio). */
static char **chooser_env(void)
{
    size_t n = 0;
    while (environ != NULL && environ[n] != NULL)
        n++;
    char **env = (char **)calloc(n + 3, sizeof *env);
    if (env == NULL)
        return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (strncmp(environ[i], "GTK_USE_PORTAL=", 15) == 0 ||
            strncmp(environ[i], "NO_AT_BRIDGE=", 13) == 0)
            continue;
        env[k++] = environ[i];
    }
    env[k++] = (char *)"GTK_USE_PORTAL=0";
    env[k++] = (char *)"NO_AT_BRIDGE=1";
    env[k] = NULL;
    return env;
}

static int cloexec_pipe(int fds[2])
{
#if defined(__linux__)
    return pipe2(fds, O_CLOEXEC);
#else
    if (pipe(fds) != 0)
        return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

static void spawn_tool(od_rom_picker *p, const char *tool, int kdialog,
                       const char *start)
{
    /* Everything the child needs is prepared here, in the parent. */
    const char *home = getenv("HOME");
    if (start == NULL || start[0] == '\0')
        start = (home && home[0]) ? home : "/";
    static char dirarg[1200], startarg[1200], toolarg[1200];
    snprintf(dirarg, sizeof dirarg, "%s/", start);
    snprintf(startarg, sizeof startarg, "%s", start);
    snprintf(toolarg, sizeof toolarg, "%s", tool);
    char *argv_k[] = { toolarg, (char *)"--title", (char *)OD_PICK_TITLE,
                       (char *)"--getopenfilename", startarg,
                       (char *)"*.nes|NES ROM", NULL };
    char *argv_z[] = { toolarg, (char *)"--file-selection", (char *)"--title",
                       (char *)OD_PICK_TITLE, (char *)"--filename", dirarg,
                       (char *)"--file-filter", (char *)"NES ROM (*.nes) | *.nes",
                       (char *)"--file-filter", (char *)"All files | *", NULL };
    char **child_argv = kdialog ? argv_k : argv_z;
    char **env = chooser_env();
    if (env == NULL)
        return;
    int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    int fds[2];
    if (cloexec_pipe(fds) != 0) {
        if (devnull >= 0) close(devnull);
        free(env);
        return;
    }
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        if (devnull >= 0) close(devnull);
        free(env);
        return;
    }
    if (pid == 0) {
        /* Child: only async-signal-safe calls until execve. */
#if defined(__linux__)
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent)
            _exit(127);           /* the game died before prctl took effect */
#else
        (void)parent;
#endif
        if (dup2(fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        /* Do not leak the game's descriptors (window, audio...) to the tool. */
        for (int fd = STDERR_FILENO + 1; fd < 1024; fd++)
            close(fd);
        execve(toolarg, child_argv, env);
        _exit(127);
    }
    /* Parent. */
    close(fds[1]);
    if (devnull >= 0) close(devnull);
    free(env);
    int flags = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    p->fd = fds[0];
    p->pid = (long)pid;
    p->len = 0;
    p->buf[0] = '\0';
    remember_child(pid);
}

void od_rom_picker_cancel(od_rom_picker *p)
{
    if (p == NULL)
        return;
    if (p->pid > 0) {
        kill_and_reap((pid_t)p->pid);
        forget_child((pid_t)p->pid);
        p->pid = 0;
    }
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
}

void od_rom_picker_begin_at(od_rom_picker *p, const char *start_dir)
{
    if (p == NULL)
        return;
    memset(p, 0, sizeof(*p));
    p->fd = -1;
    if (start_dir != NULL)
        snprintf(p->start, sizeof p->start, "%s", start_dir);
    char tool[1200];
    if (find_in_path("zenity", tool, sizeof tool))
        spawn_tool(p, tool, 0, p->start);
    else if (find_in_path("kdialog", tool, sizeof tool))
        spawn_tool(p, tool, 1, p->start), p->tried_kdialog = 1;
    /* else: no tool; poll() reports UNAVAILABLE. */
}

void od_rom_picker_begin(od_rom_picker *p)
{
    od_rom_picker_begin_at(p, NULL);
}

static void reap(od_rom_picker *p, int *exited, int *code)
{
    *exited = 1;
    *code = -1;
    if (p->pid > 0) {
        int status = 0;
        pid_t r = waitpid((pid_t)p->pid, &status, WNOHANG);
        if (r == 0) {
            *exited = 0;
            return;
        }
        if (r == (pid_t)p->pid) {
            *code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            forget_child((pid_t)p->pid);
            p->pid = 0;
        }
    }
}

od_pick_result od_rom_picker_poll(od_rom_picker *p, char *out, size_t cap,
                                    char *err, size_t errcap)
{
    if (p == NULL)
        return OD_PICK_ERROR;
    if (p->fd < 0 && p->pid <= 0) {
        set_err(err, errcap, "no file chooser available (install zenity or kdialog)");
        return OD_PICK_UNAVAILABLE;
    }
    if (p->fd >= 0) {
        for (;;) {
            if (p->len + 1 >= sizeof p->buf)
                break;
            ssize_t n = read(p->fd, p->buf + p->len, sizeof p->buf - 1 - p->len);
            if (n > 0) {
                p->len += (size_t)n;
                continue;
            }
            break;   /* n == 0 (EOF) or n < 0 (EAGAIN) */
        }
        p->buf[p->len] = '\0';
    }
    int exited = 0, code = -1;
    reap(p, &exited, &code);
    if (!exited)
        return OD_PICK_PENDING;
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
    if (code == 0) {
        trim(p->buf);
        if (p->buf[0] == '\0')
            return OD_PICK_CANCEL;
        if (snprintf(out, cap, "%s", p->buf) >= (int)cap) {
            set_err(err, errcap, "the chosen path is too long");
            return OD_PICK_ERROR;
        }
        return OD_PICK_OK;
    }
    if (code == 1)
        return OD_PICK_CANCEL;
    set_err(err, errcap, "the file chooser failed");
    return OD_PICK_ERROR;
}

od_pick_result od_rom_picker_open(char *out, size_t cap, char *err, size_t errcap)
{
    od_rom_picker p;
    od_rom_picker_begin(&p);
    for (;;) {
        od_pick_result r = od_rom_picker_poll(&p, out, cap, err, errcap);
        if (r != OD_PICK_PENDING)
            return r;
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}
#endif
