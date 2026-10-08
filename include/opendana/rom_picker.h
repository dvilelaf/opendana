/* rom_picker.h - system file chooser for the one-time ROM import (C6.21).
 *
 * The first-run screen must not force the player to type a filesystem path
 * (owner request 2026-10-06: native dialog). This module asks the desktop for
 * its own "choose file" dialog and returns the chosen path.
 *
 * It is deliberately NON-BLOCKING: `begin` spawns the chooser and returns at
 * once, and the SDL front end calls `poll` every frame, so the game keeps
 * running and the window is not frozen while the dialog is open. The chooser is
 * started without a shell and with GTK_USE_PORTAL=0/NO_AT_BRIDGE=1, because a
 * portal dialog with no parent window is what hung inside the AppImage.
 *
 * It is SDL-free and compiles no material of the original game. */
#ifndef OD_ROM_PICKER_H
#define OD_ROM_PICKER_H

#include <stddef.h>

typedef enum {
    OD_PICK_OK = 0,       /* `out` holds the chosen path                   */
    OD_PICK_CANCEL,       /* the user closed the chooser (not an error)    */
    OD_PICK_UNAVAILABLE,  /* this machine has no system chooser            */
    OD_PICK_ERROR,        /* the chooser failed or the path does not fit   */
    OD_PICK_PENDING       /* the dialog is still open (poll again)         */
} od_pick_result;

/* The game window the dialog belongs to (C6.76). The chooser is opened on top of it,
 * as its child, instead of minimising the game: the front end fills what it knows
 * from SDL_GetWindowWMInfo and leaves the rest zero. */
typedef struct {
    unsigned long x11_window;   /* X11 window id (Linux, X11 only; 0 = unknown)  */
    void *native_window;        /* Windows: the HWND (0 elsewhere)               */
} od_picker_parent;

typedef struct {
    int fd;                 /* read end of the child's stdout (-1 = none)   */
    long pid;               /* child pid (0 = none)                         */
    int tried_kdialog;      /* zenity was not usable, kdialog is running    */
    char start[1024];       /* preferred start directory ("" = default)     */
    char buf[4096];
    size_t len;
    /* C6.76 */
    int attached;           /* 1: the dialog is a child of / in front of the game window,
                             * 0: it could not be (no window id, Wayland, the tool refused
                             * --attach): the front end then minimises as a last resort */
    int kind;               /* the running tool: 0 zenity, 1 kdialog, 2 osascript, 3 Windows thread */
    unsigned long parent_x11;
    void *parent_native;
    long started_ms;        /* when the child started (to tell a refused option from a cancel) */
    void *thread;           /* Windows: the dialog thread (opaque) */
    void *shared;           /* Windows: the thread's result block (opaque) */
} od_rom_picker;

/* Start the chooser (or record UNAVAILABLE). Returns at once.
 * `start_dir` (may be NULL) is the folder the dialog opens in. */
void od_rom_picker_begin_at(od_rom_picker *p, const char *start_dir);
void od_rom_picker_begin(od_rom_picker *p);

/* C6.76: the same, with the dialog a CHILD of the game window (Linux: --attach and
 * --modal for zenity, --attach for kdialog; Windows: the window is the owner, in its own
 * thread so the game keeps drawing; macOS: osascript's dialog is brought to the front).
 * `parent` may be NULL. `p->attached` tells whether it worked; when it is 0 and a
 * chooser is running, the front end minimises the game window as a last resort. If a tool
 * rejects --attach (it exits at once with a usage error), the picker restarts it without. */
void od_rom_picker_begin_child(od_rom_picker *p, const char *start_dir,
                               const od_picker_parent *parent);

/* Remembered last folder: persisted under the user's XDG data directory.
 * `od_rom_picker_last_dir` returns 1 when a folder was stored.
 * `od_rom_picker_remember` stores the directory of a chosen ROM path. */
int od_rom_picker_last_dir(char *out, size_t cap);
void od_rom_picker_remember(const char *chosen_path);

/* Poll the chooser. OD_PICK_PENDING while it is open; on a terminal result,
 * `out` (cap bytes) holds the path when OK. `err` gets a short message for
 * UNAVAILABLE/ERROR. */
od_pick_result od_rom_picker_poll(od_rom_picker *p, char *out, size_t cap,
                                    char *err, size_t errcap);

/* Close the chooser if it is still open: terminate the child, reap it and
 * close the pipe (C6.29, UI-20). Safe to call at any time. Children that are
 * still alive when the game exits are also killed and reaped by an atexit
 * handler, and on Linux the kernel terminates them if the game dies first. */
void od_rom_picker_cancel(od_rom_picker *p);

/* Blocking convenience (headless tests): begin + poll until terminal. */
od_pick_result od_rom_picker_open(char *out, size_t cap, char *err, size_t errcap);

#endif /* OD_ROM_PICKER_H */
