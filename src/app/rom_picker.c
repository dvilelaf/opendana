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
static od_pick_result win_open(HWND owner, const char *start, char *out, size_t cap,
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
    ofn.hwndOwner = owner;          /* C6.76: the dialog is owned by the game window */
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

/* C6.76: the dialog runs in its own thread, owned by the game window, so the game keeps
 * drawing while it is open (the common dialog is modal for its owner: the game window does
 * not take input meanwhile, which is what a modal child is). One job per dialog: state 0 =
 * running, 1 = finished and waiting to be collected, 2 = abandoned (cancel while open: the
 * thread frees the block when the dialog ends). */
typedef struct {
    volatile LONG state;
    od_pick_result result;
    char path[1024];
    char err[160];
    char start[1024];
    HWND owner;
} win_job;

static DWORD WINAPI win_thread(LPVOID arg)
{
    win_job *j = (win_job *)arg;
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    j->result = win_open(j->owner, j->start, j->path, sizeof j->path, j->err, sizeof j->err);
    if (SUCCEEDED(co))
        CoUninitialize();
    if (InterlockedCompareExchange(&j->state, 1, 0) == 2)
        free(j);
    return 0;
}

void od_rom_picker_begin_child(od_rom_picker *p, const char *start_dir,
                               const od_picker_parent *parent)
{
    if (p == NULL)
        return;
    memset(p, 0, sizeof(*p));
    p->fd = -1;
    if (start_dir != NULL)
        snprintf(p->start, sizeof p->start, "%s", start_dir);
    p->kind = 3;
    win_job *j = (win_job *)calloc(1, sizeof *j);
    if (j == NULL)
        return;         /* poll() runs the dialog on the calling thread, as before C6.76 */
    j->owner = parent != NULL ? (HWND)parent->native_window : NULL;
    snprintf(j->start, sizeof j->start, "%s", p->start);
    HANDLE th = CreateThread(NULL, 0, win_thread, j, 0, NULL);
    if (th == NULL) {
        free(j);
        return;
    }
    p->thread = th;
    p->shared = j;
    p->pid = 1;                         /* a dialog is open */
    p->attached = j->owner != NULL;
}

void od_rom_picker_begin_at(od_rom_picker *p, const char *start_dir)
{
    od_rom_picker_begin_child(p, start_dir, NULL);
}

void od_rom_picker_begin(od_rom_picker *p)
{
    od_rom_picker_begin_at(p, NULL);
}

void od_rom_picker_cancel(od_rom_picker *p)
{
    if (p == NULL || p->shared == NULL)
        return;
    win_job *j = (win_job *)p->shared;
    if (InterlockedCompareExchange(&j->state, 2, 0) == 1)
        free(j);                        /* it had finished: nobody else frees it */
    if (p->thread != NULL)
        CloseHandle((HANDLE)p->thread);
    p->shared = NULL;
    p->thread = NULL;
    p->pid = 0;
}

od_pick_result od_rom_picker_poll(od_rom_picker *p, char *out, size_t cap,
                                    char *err, size_t errcap)
{
    if (p == NULL)
        return OD_PICK_ERROR;
    if (p->shared != NULL) {
        win_job *j = (win_job *)p->shared;
        if (j->state != 1)
            return OD_PICK_PENDING;
        od_pick_result r = j->result;
        if (r == OD_PICK_OK) {
            if (snprintf(out, cap, "%s", j->path) >= (int)cap) {
                set_err(err, errcap, "the chosen path is too long");
                r = OD_PICK_ERROR;
            }
        } else if (j->err[0] != '\0') {
            set_err(err, errcap, j->err);
        }
        WaitForSingleObject((HANDLE)p->thread, INFINITE);
        CloseHandle((HANDLE)p->thread);
        free(j);
        p->shared = NULL;
        p->thread = NULL;
        p->pid = 0;
        return r;
    }
    p->pid = -1;   /* no thread (out of memory): the dialog runs here, once */
    return win_open(NULL, p->start, out, cap, err, errcap);
}

od_pick_result od_rom_picker_open(char *out, size_t cap, char *err, size_t errcap)
{
    return win_open(NULL, NULL, out, cap, err, errcap);
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

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

/* kind: 0 zenity, 1 kdialog, 2 osascript (macOS, C6.70). `attach_x11` (C6.76, Linux): the
 * X11 id of the game window; the dialog is made its modal child (zenity --attach --modal,
 * kdialog --attach). 0 = no parent. */
static void spawn_tool(od_rom_picker *p, const char *tool, int kind,
                       const char *start, unsigned long attach_x11)
{
    /* Everything the child needs is prepared here, in the parent. */
    const char *home = getenv("HOME");
    if (start == NULL || start[0] == '\0')
        start = (home && home[0]) ? home : "/";
    static char dirarg[1200], startarg[1200], toolarg[1200], attacharg[64];
    snprintf(dirarg, sizeof dirarg, "%s/", start);
    snprintf(startarg, sizeof startarg, "%s", start);
    snprintf(toolarg, sizeof toolarg, "%s", tool);
    snprintf(attacharg, sizeof attacharg, "%lu", attach_x11);
    char *av[20];
    int n = 0;
    av[n++] = toolarg;
    int attached = 0;
    if (kind == 1) {
        av[n++] = (char *)"--title"; av[n++] = (char *)OD_PICK_TITLE;
        if (attach_x11) {
            av[n++] = (char *)"--attach"; av[n++] = attacharg;
            attached = 1;
        }
        av[n++] = (char *)"--getopenfilename"; av[n++] = startarg;
        av[n++] = (char *)"*.nes|NES ROM";
    } else if (kind == 0) {
        av[n++] = (char *)"--file-selection"; av[n++] = (char *)"--title";
        av[n++] = (char *)OD_PICK_TITLE; av[n++] = (char *)"--filename"; av[n++] = dirarg;
        av[n++] = (char *)"--file-filter"; av[n++] = (char *)"NES ROM (*.nes) | *.nes";
        av[n++] = (char *)"--file-filter"; av[n++] = (char *)"All files | *";
        if (attach_x11) {
            static char zattach[80];
            snprintf(zattach, sizeof zattach, "--attach=%lu", attach_x11);
            av[n++] = zattach; av[n++] = (char *)"--modal";
            attached = 1;
        }
    }
#if defined(__APPLE__)
    if (kind == 2) {
        /* C6.70: the native chooser of macOS through AppleScript's `choose file`. The prompt
         * and the folder travel as arguments of `on run argv`, never inside the script text,
         * so no path can alter the script. A folder that does not exist is dropped (the call
         * would fail); Cancel makes osascript exit 1, which is reported as a cancel like the
         * others. C6.76: `activate` brings the dialog to the front of the game window (the
         * game is no longer minimised); it needs no permission, it activates osascript itself. */
        struct stat start_st;
        int have_start = stat(startarg, &start_st) == 0 && S_ISDIR(start_st.st_mode);
        n = 0;
        av[n++] = toolarg;
        av[n++] = (char *)"-e"; av[n++] = (char *)"on run argv";
        av[n++] = (char *)"-e"; av[n++] = (char *)"tell current application to activate";
        av[n++] = (char *)"-e"; av[n++] = (char *)(have_start
            ? "set f to choose file with prompt (item 1 of argv) default location (POSIX file (item 2 of argv))"
            : "set f to choose file with prompt (item 1 of argv)");
        av[n++] = (char *)"-e"; av[n++] = (char *)"return POSIX path of f";
        av[n++] = (char *)"-e"; av[n++] = (char *)"end run";
        av[n++] = (char *)OD_PICK_TITLE;
        if (have_start)
            av[n++] = startarg;
        attached = 1;       /* in front of the game window, which stays where it is */
    }
#endif
    av[n] = NULL;
    char **child_argv = av;
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
    p->kind = kind;
    p->attached = attached;
    p->started_ms = now_ms();
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

void od_rom_picker_begin_child(od_rom_picker *p, const char *start_dir,
                               const od_picker_parent *parent)
{
    if (p == NULL)
        return;
    memset(p, 0, sizeof(*p));
    p->fd = -1;
    if (start_dir != NULL)
        snprintf(p->start, sizeof p->start, "%s", start_dir);
    unsigned long x11 = parent != NULL ? parent->x11_window : 0;
    p->parent_x11 = x11;
    p->parent_native = parent != NULL ? parent->native_window : NULL;
    char tool[1200];
#if defined(__APPLE__)
    /* C6.70: macOS ships osascript in /usr/bin; PATH first so tests can put a stand-in there. */
    if (find_in_path("osascript", tool, sizeof tool) ||
        (access("/usr/bin/osascript", X_OK) == 0 &&
         snprintf(tool, sizeof tool, "/usr/bin/osascript") > 0))
        spawn_tool(p, tool, 2, p->start, 0);
#else
    if (find_in_path("zenity", tool, sizeof tool))
        spawn_tool(p, tool, 0, p->start, x11);
    else if (find_in_path("kdialog", tool, sizeof tool))
        spawn_tool(p, tool, 1, p->start, x11), p->tried_kdialog = 1;
#endif
    /* else: no tool; poll() reports UNAVAILABLE. */
}

void od_rom_picker_begin_at(od_rom_picker *p, const char *start_dir)
{
    od_rom_picker_begin_child(p, start_dir, NULL);
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
#if defined(__APPLE__)
        set_err(err, errcap, "no file chooser available (osascript was not found)");
#else
        set_err(err, errcap, "no file chooser available (install zenity or kdialog)");
#endif
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
    /* C6.76: a tool that does not know --attach/--modal exits at once with a usage error
     * (neither 0 nor 1, which are OK and Cancel). Start it again without a parent: the
     * front end then sees attached == 0 and minimises the game window, the last resort. */
    if (code != 0 && code != 1 && p->attached && p->kind < 2 &&
        now_ms() - p->started_ms < 3000) {
        char tool[1200];
        int kind = p->kind;
        if (find_in_path(kind == 0 ? "zenity" : "kdialog", tool, sizeof tool)) {
            p->pid = 0;
            spawn_tool(p, tool, kind, p->start, 0);
            if (p->pid > 0)
                return OD_PICK_PENDING;
        }
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
