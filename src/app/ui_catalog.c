/* ui_catalog.c - custom-level catalogue browser (C5.5).
 *
 * Pure C11, no SDL. Lists the catalogue directory through the C5.4 model and
 * exposes requests the host translates into the game's own play path. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "opendana/ui_catalog.h"
#include "opendana/userdir.h"
#include "opendana/ui_font.h"
#include "opendana/ui_strings.h"
#include "../../external/stb/stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define C_PANEL   0xFF14142Cu
#define C_BORDER  0xFFB0B0D0u
#define C_TITLE   0xFFFFE080u
#define C_TEXT    0xFFE8E8F0u
#define C_SEL_BG  0xFF2E4E9Eu
#define C_SEL_TX  0xFFFFFFFFu
#define C_MUTED   0xFF9090A0u
#define C_BTN     0xFF303048u
#define C_ERR     0xFFFF8080u
#define C_OK      0xFF90E090u

/* C6.24: the catalogue shares the editor's 384x360 canvas. Boxes are sized
 * from the 8x8 font (a label is 8*n pixels plus 4 on each side, a box 14
 * pixels high), and every text is cut with "..." to its box. */
#define TITLE_H 10
#define LIST_Y 10                /* rows and thumbnails as in C6.10 */
#define ROW_H  36
#define LIST_H (ROW_H * OD_UCAT_THUMB_SLOTS)      /* 7 rows: 10..262 */
#define TEXT_X 38                                   /* after the 32x32 thumb */
#define PATH_Y (LIST_Y + LIST_H + 4)
#define PATH_H 14
#define STATUS_Y (PATH_Y + PATH_H + 4)
#define STATUS_LINE_H 11
#define TB_ROW0_Y (STATUS_Y + 2 * STATUS_LINE_H + 4)
#define TB_ROW1_Y (TB_ROW0_Y + TB_H + 4)
#define TB_H 14
#define HELP_Y (TB_ROW1_Y + TB_H + 6)
#define BTN_W(n) ((n) * 8 + 8)

_Static_assert(OD_UCAT_THUMB_SLOTS >= LIST_H / ROW_H,
               "visible catalogue rows must fit the thumbnail cache");
_Static_assert(HELP_Y + 8 <= OD_UCAT_H, "catalogue layout fits the canvas");

typedef struct { int btn, req, x, y, w; od_string label; } btn_rect;
static const btn_rect k_buttons[OD_UCAT_BTN_COUNT] = {
    { OD_UCAT_BTN_PLAY,   OD_UCAT_REQ_PLAY,      4, TB_ROW0_Y, BTN_W(4), OD_STR_PLAY },
    { OD_UCAT_BTN_EDIT,   OD_UCAT_REQ_EDIT,     48, TB_ROW0_Y, BTN_W(4), OD_STR_EDIT },
    { OD_UCAT_BTN_DUP,    OD_UCAT_REQ_DUP,      92, TB_ROW0_Y, BTN_W(3), OD_STR_DUP },
    { OD_UCAT_BTN_DELETE, OD_UCAT_REQ_DELETE,  128, TB_ROW0_Y, BTN_W(6), OD_STR_DELETE },
    { OD_UCAT_BTN_SORT,   OD_UCAT_REQ_NONE,    188, TB_ROW0_Y, BTN_W(4), OD_STR_SORT },
    { OD_UCAT_BTN_BACK,   OD_UCAT_REQ_BACK,    340, TB_ROW0_Y, BTN_W(4), OD_STR_BACK },
    { OD_UCAT_BTN_IMPORT, OD_UCAT_REQ_IMPORT,    4, TB_ROW1_Y, BTN_W(6), OD_STR_IMPORT },
    { OD_UCAT_BTN_EXPORT, OD_UCAT_REQ_EXPORT,   64, TB_ROW1_Y, BTN_W(6), OD_STR_EXPORT },
};

static int inside(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

void od_ucat_default_dir(char *out, size_t cap)
{
    if (out == NULL || cap == 0)
        return;
#ifndef _WIN32
    /* C6.25: the per-user folder comes from the one helper (C6.31). */
    if (od_userdir_path(OD_USERDIR_DATA, "levels", out, cap) == OD_OK)
        return;
#endif
    snprintf(out, cap, "levels");
}

void od_ucat_default_io_path(const char *dir, char *out, size_t cap)
{
    if (out == NULL || cap == 0)
        return;
    char parent[1024];
    snprintf(parent, sizeof parent, "%s", dir != NULL ? dir : "levels");
    size_t n = strlen(parent);
    while (n > 1 && parent[n - 1] == '/')
        parent[--n] = '\0';
    char *slash = strrchr(parent, '/');
    if (slash != NULL && slash != parent)
        *slash = '\0';
    else
        snprintf(parent, sizeof parent, ".");
    snprintf(out, cap, "%s/exports/", parent);
}

void od_ucat_init(od_ui_catalog *c, const char *dir)
{
    if (c == NULL)
        return;
    memset(c, 0, sizeof *c);
    if (dir != NULL && dir[0] != '\0') {
        snprintf(c->dir, sizeof c->dir, "%s", dir);
    } else {
        od_ucat_default_dir(c->dir, sizeof c->dir);
    }
    od_ucat_default_io_path(c->dir, c->io_path, sizeof c->io_path);
    c->path_cursor = (int)strlen(c->io_path);
    c->order = OD_CATALOG_SORT_NAME;
    c->open = 1;
    snprintf(c->status, sizeof c->status, "%s", od_str(OD_STR_CUSTOM_LEVELS));
}

void od_ucat_free(od_ui_catalog *c)
{
    if (c) memset(c->thumbnails, 0, sizeof c->thumbnails);
}

int od_ucat_refresh(od_ui_catalog *c)
{
    if (c == NULL)
        return OD_ERR_RANGE;
    od_ucat_free(c);
    int n = 0;
    od_status st = od_catalog_scan(c->dir, c->entries, OD_UCAT_MAX, &n);
    c->n = n;
    if (c->focus >= n) c->focus = n > 0 ? n - 1 : 0;
    if (c->focus < 0) c->focus = 0;
    if (st == OD_ERR_RANGE) {
        snprintf(c->status, sizeof c->status, od_str(OD_STR_CATALOG_LIMIT), c->n);
        c->status_error = 1;
    } else if (st != OD_OK) {
        snprintf(c->status, sizeof c->status, "%s", od_str(OD_STR_CATALOG_ERROR));
        c->status_error = 1;
    } else {
        snprintf(c->status, sizeof c->status, od_str(OD_STR_CUSTOM_LEVEL_COUNT), c->n);
        c->status_error = 0;
    }
    return (int)st;
}

void od_ucat_open(od_ui_catalog *c)
{
    if (c == NULL)
        return;
    c->open = 1;
    od_ucat_refresh(c);
}

void od_ucat_button_rect(int button, int *x, int *y, int *w, int *h)
{
    for (int i = 0; i < OD_UCAT_BTN_COUNT; i++) {
        if (k_buttons[i].btn == button) {
            if (x) *x = k_buttons[i].x;
            if (y) *y = k_buttons[i].y;
            if (w) *w = k_buttons[i].w;
            if (h) *h = TB_H;
            return;
        }
    }
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = 0;
    if (h) *h = 0;
}

void od_ucat_row_rect(int index, int *x, int *y, int *w, int *h)
{
    if (x) *x = 2;
    if (y) *y = LIST_Y + index * ROW_H;
    if (w) *w = OD_UCAT_W - 4;
    if (h) *h = ROW_H - 1;
}

int od_ucat_entry_at(const od_ui_catalog *c, int x, int y)
{
    if (c == NULL || x < 2 || x >= OD_UCAT_W - 2)
        return -1;
    if (y < LIST_Y || y >= LIST_Y + LIST_H)
        return -1;
    int idx = c->scroll + (y - LIST_Y) / ROW_H;
    if (idx < 0 || idx >= c->n)
        return -1;
    return idx;
}

static void set_request(od_ui_catalog *c, int req, const char *id)
{
    c->request = req;
    if (id != NULL)
        snprintf(c->request_id, sizeof c->request_id, "%.*s",
                 (int)sizeof c->request_id - 1, id);
    else
        c->request_id[0] = '\0';
}

static const char *focus_id(const od_ui_catalog *c)
{
    if (c->focus < 0 || c->focus >= c->n)
        return NULL;
    return c->entries[c->focus].id;
}

static void toggle_sort(od_ui_catalog *c)
{
    c->order = c->order == OD_CATALOG_SORT_NAME ? OD_CATALOG_SORT_MODIFIED
                                                 : OD_CATALOG_SORT_NAME;
    od_catalog_sort(c->entries, c->n, (od_catalog_order)c->order);
    snprintf(c->status, sizeof c->status, od_str(OD_STR_SORT_ORDER),
             od_str(c->order == OD_CATALOG_SORT_NAME ? OD_STR_SORT_NAME : OD_STR_SORT_DATE));
    c->status_error = 0;
}

void od_ucat_do_request(od_ui_catalog *c, int req);

/* 1 when `path` would land inside the catalogue directory itself. */
static int path_in_catalog(const od_ui_catalog *c, const char *path)
{
    size_t n = strlen(c->dir);
    return n > 0 && strncmp(path, c->dir, n) == 0 &&
           (path[n] == '/' || path[n] == '\0') && strchr(path + n + 1, '/') == NULL;
}

static void set_status(od_ui_catalog *c, int error, od_string fmt, const char *arg)
{
    snprintf(c->status, sizeof c->status, od_str(fmt), arg ? arg : "");
    c->status_error = error;
}

/* Duplicate, import and export are catalogue operations (C5.4 model), so they
 * run here; the host only serves Play, Edit and Back. The message is written
 * after the list is re-read, so it is not replaced by the level count
 * (C6.32, UI-5). */
static void do_duplicate(od_ui_catalog *c, const char *id)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/%s.json", c->dir, id);
    od_led_doc d;
    memset(&d, 0, sizeof d);
    char newid[OD_LED_ID_MAX] = "";
    int ok = od_led_load_json(&d, path) == OD_OK;
    if (ok) {
        d.id[0] = '\0';
        const char *suffix = od_str(OD_STR_DUP_SUFFIX);
        size_t n = strlen(d.name);
        if (n + strlen(suffix) < sizeof d.name)
            snprintf(d.name + n, sizeof d.name - n, "%s", suffix);
        ok = od_catalog_save(c->dir, &d, newid) == OD_OK;
    }
    od_led_free(&d);
    od_ucat_refresh(c);
    if (ok)
        set_status(c, 0, OD_STR_LEVEL_DUPLICATED, newid);
    else
        set_status(c, 1, OD_STR_LEVEL_LOAD_ERROR, id);
}

static void do_import(od_ui_catalog *c)
{
    char id[OD_LED_ID_MAX] = "";
    int ok = od_catalog_import(c->dir, c->io_path, id) == OD_OK;
    od_ucat_refresh(c);
    if (ok)
        set_status(c, 0, OD_STR_LEVEL_IMPORTED, id);
    else
        set_status(c, 1, OD_STR_IMPORT_ERROR, NULL);
}

static void do_export(od_ui_catalog *c, const char *id)
{
    char path[1400];
    size_t n = strlen(c->io_path);
    if (n == 0 || c->io_path[n - 1] == '/')
        snprintf(path, sizeof path, "%s%s.json", c->io_path, id);
    else
        snprintf(path, sizeof path, "%s", c->io_path);
    if (path_in_catalog(c, path)) {
        set_status(c, 1, OD_STR_EXPORT_INSIDE, NULL);
        return;
    }
    /* Create the (default) exports folder. */
    char folder[1400];
    snprintf(folder, sizeof folder, "%s", path);
    char *slash = strrchr(folder, '/');
    if (slash != NULL && slash != folder) {
        *slash = '\0';
        (void)od_userdir_mkdir_p(folder);   /* UTF-8 on Windows (C6.36) */
    }
    if (od_catalog_export(c->dir, id, path) == OD_OK)
        set_status(c, 0, OD_STR_LEVEL_EXPORTED, path);
    else
        set_status(c, 1, OD_STR_EXPORT_ERROR, NULL);
}

static void do_delete(od_ui_catalog *c, const char *id)
{
    char copy[OD_LED_ID_MAX];
    snprintf(copy, sizeof copy, "%s", id);
    if (od_catalog_delete(c->dir, copy) == OD_OK) {
        od_ucat_refresh(c);
        set_status(c, 0, OD_STR_LEVEL_DELETED, copy);
    } else {
        set_status(c, 1, OD_STR_LEVEL_DELETE_ERROR, copy);
    }
}

void od_ucat_do_request(od_ui_catalog *c, int req);

void od_ucat_do_request(od_ui_catalog *c, int req)
{
    const char *id = focus_id(c);
    switch (req) {
    case OD_UCAT_REQ_PLAY:
    case OD_UCAT_REQ_EDIT:
        if (id != NULL) set_request(c, req, id);
        break;
    case OD_UCAT_REQ_DUP:
        if (id != NULL) do_duplicate(c, id);
        break;
    case OD_UCAT_REQ_DELETE:
        /* C6.32 (UI-23): deleting asks first. */
        if (id != NULL) {
            c->confirm_delete = 1;
            snprintf(c->confirm_id, sizeof c->confirm_id, "%s", id);
        }
        break;
    case OD_UCAT_REQ_IMPORT:
        do_import(c);
        break;
    case OD_UCAT_REQ_EXPORT:
        if (id != NULL) do_export(c, id);
        break;
    case OD_UCAT_REQ_BACK:
        set_request(c, req, NULL);
        break;
    default:
        break;
    }
}

static void answer_delete(od_ui_catalog *c, int yes)
{
    c->confirm_delete = 0;
    if (yes && c->confirm_id[0] != '\0')
        do_delete(c, c->confirm_id);
    c->confirm_id[0] = '\0';
}

#define CONFIRM_X 72
#define CONFIRM_Y 100
#define CONFIRM_W 240
#define CONFIRM_H 64

void od_ucat_confirm_button_rect(int yes, int *x, int *y, int *w, int *h)
{
    int bw = BTN_W(3);
    if (x) *x = yes ? CONFIRM_X + 16 : CONFIRM_X + CONFIRM_W - 16 - bw;
    if (y) *y = CONFIRM_Y + CONFIRM_H - TB_H - 8;
    if (w) *w = bw;
    if (h) *h = TB_H;
}

/* Append typed UTF-8 to the path field. */
void od_ucat_text(od_ui_catalog *c, const char *utf8)
{
    if (c == NULL || utf8 == NULL || !c->editing_path)
        return;
    size_t n = strlen(c->io_path), add = strlen(utf8);
    if (n + add + 1 > sizeof c->io_path)
        return;
    for (size_t i = 0; i < add; i++)
        if ((unsigned char)utf8[i] < 32)
            return;                         /* no control characters */
    memcpy(c->io_path + n, utf8, add + 1);
    c->path_cursor = (int)(n + add);
}

int od_ucat_pointer(od_ui_catalog *c, int x, int y, int buttons, int click,
                     int wheel)
{
    if (c == NULL || !c->open)
        return 0;
    if (c->confirm_delete) {
        for (int yes = 0; yes <= 1; yes++) {
            int bx, by, bw, bh;
            od_ucat_confirm_button_rect(yes, &bx, &by, &bw, &bh);
            if (click && (buttons & 1) && inside(x, y, bx, by, bw, bh)) {
                answer_delete(c, yes);
                break;
            }
        }
        return 1;
    }
    for (int i = 0; i < OD_UCAT_BTN_COUNT; i++) {
        int bx, by, bw, bh;
        od_ucat_button_rect(k_buttons[i].btn, &bx, &by, &bw, &bh);
        if (!inside(x, y, bx, by, bw, bh))
            continue;
        if (click && (buttons & 1)) {
            if (k_buttons[i].btn == OD_UCAT_BTN_SORT)
                toggle_sort(c);
            else
                od_ucat_do_request(c, k_buttons[i].req);
        }
        return 1;
    }
    if (y >= PATH_Y && y < PATH_Y + PATH_H) {
        if (click && (buttons & 1)) {
            c->editing_path = 1;
            c->path_cursor = (int)strlen(c->io_path);
        }
        return 1;
    }
    if (wheel != 0) {
        c->scroll += wheel > 0 ? 1 : -1;
        int max = c->n - (LIST_H / ROW_H);
        if (max < 0) max = 0;
        if (c->scroll < 0) c->scroll = 0;
        if (c->scroll > max) c->scroll = max;
        return 1;
    }
    int idx = od_ucat_entry_at(c, x, y);
    if (idx >= 0) {
        /* C6.32 (UI-8): hovering does not move the selection; a click
         * selects, and a click on the selected row plays it. */
        if (click && (buttons & 1)) {
            c->editing_path = 0;
            if (idx == c->focus)
                od_ucat_do_request(c, OD_UCAT_REQ_PLAY);
            else
                c->focus = idx;
        }
        return 1;
    }
    return 0;
}

int od_ucat_key(od_ui_catalog *c, int key, int ch)
{
    if (c == NULL || !c->open)
        return 0;
    if (c->confirm_delete) {
        if (key == OD_UCAT_KEY_PLAY || key == OD_UCAT_KEY_BACK)
            answer_delete(c, key == OD_UCAT_KEY_PLAY);
        return 1;
    }
    if (c->editing_path) {
        if (key == OD_UCAT_KEY_CHAR && ch >= 32 && ch < 127) {
            size_t n = strlen(c->io_path);
            if (n + 1 < sizeof c->io_path) {
                c->io_path[n] = (char)ch;
                c->io_path[n + 1] = '\0';
                c->path_cursor = (int)(n + 1);
            }
            return 1;
        }
        if (key == OD_UCAT_KEY_BACKSPACE) {
            size_t n = strlen(c->io_path);
            /* One whole character, not one byte of it. */
            while (n > 0 && ((unsigned char)c->io_path[n - 1] & 0xC0) == 0x80)
                n--;
            if (n > 0)
                n--;
            c->io_path[n] = '\0';
            c->path_cursor = (int)n;
            return 1;
        }
        if (key == OD_UCAT_KEY_BACK) {
            c->editing_path = 0;
            return 1;
        }
    }
    switch (key) {
    case OD_UCAT_KEY_UP:
        if (c->n > 0) {
            c->focus = (c->focus + c->n - 1) % c->n;
            if (c->focus < c->scroll) c->scroll = c->focus;
        }
        return 1;
    case OD_UCAT_KEY_DOWN:
        if (c->n > 0) {
            c->focus = (c->focus + 1) % c->n;
            int vis = LIST_H / ROW_H;
            if (c->focus >= c->scroll + vis) c->scroll = c->focus - vis + 1;
        }
        return 1;
    case OD_UCAT_KEY_PLAY:
        od_ucat_do_request(c, OD_UCAT_REQ_PLAY);
        return 1;
    case OD_UCAT_KEY_DELETE:
        od_ucat_do_request(c, OD_UCAT_REQ_DELETE);
        return 1;
    case OD_UCAT_KEY_BACK:
        od_ucat_do_request(c, OD_UCAT_REQ_BACK);
        return 1;
    default:
        return 0;
    }
}

/* ---- drawing ------------------------------------------------------------ */

/* Read only a model-generated basename, never an index-supplied path or a
 * symlink. No VFS/base-content lookup: thumbnails belong to the user catalogue. */
static FILE *thumbnail_file(const od_ui_catalog *c, const od_catalog_entry *en)
{
    char name[256];
    snprintf(name, sizeof name, "%s.png", en->id);
    if (!en->id[0] || strcmp(name, en->thumb) != 0) return NULL;
    for (const char *p = en->id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_')) return NULL;
#if defined(_WIN32)
    char path[1500];
    int len = snprintf(path, sizeof path, "%s/%s", c->dir, name);
    if (len < 0 || (size_t)len >= sizeof path) return NULL;
    HANDLE handle = (HANDLE)od_userdir_win_open_noreparse(path);   /* wide (C6.36) */
    if (handle == NULL) return NULL;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle, &info) ||
        GetFileType(handle) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        CloseHandle(handle); return NULL;
    }
    int fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (fd < 0) { CloseHandle(handle); return NULL; }
    FILE *f = _fdopen(fd, "rb");
    if (!f) _close(fd);
    if(f&&en->file[0]) {
        char json[256],jp[1500];snprintf(json,sizeof json,"%s.json",en->id);
        int n=snprintf(jp,sizeof jp,"%s/%s",c->dir,json);
        if(strcmp(json,en->file)||n<0||(size_t)n>=sizeof jp){fclose(f);return NULL;}
        HANDLE j=(HANDLE)od_userdir_win_open_noreparse(jp);
        BY_HANDLE_FILE_INFORMATION ji;
        int fresh=j!=NULL&&GetFileInformationByHandle(j,&ji)&&
            !(ji.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&
            CompareFileTime(&info.ftLastWriteTime,&ji.ftLastWriteTime)>=0;
        if(j!=NULL)CloseHandle(j);
        if(!fresh){fclose(f);return NULL;}
    }
    return f;
#else
    int dir = open(c->dir, O_RDONLY | O_DIRECTORY);
    if (dir < 0) return NULL;
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    close(dir);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    FILE *f = fdopen(fd, "rb");
    if (!f) close(fd);
    /* A new JSON save with a failed/missing preview must not display an old
     * PNG. File identity is model-generated id, never index-supplied paths. */
    if(f&&en->file[0]) {
        char json[256];snprintf(json,sizeof json,"%s.json",en->id);
        if(strcmp(json,en->file)){fclose(f);return NULL;}
        /* The folder may be an alias (C6.32, UI-14); the file may not. */
        int root=open(c->dir,O_RDONLY|O_DIRECTORY);
        int jf=root<0?-1:openat(root,json,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
        if(root>=0)close(root);
        struct stat js;int fresh=0;
        if(jf>=0&&!fstat(jf,&js)&&S_ISREG(js.st_mode)) {
#if defined(__APPLE__)
            fresh=st.st_mtimespec.tv_sec>js.st_mtimespec.tv_sec||
                (st.st_mtimespec.tv_sec==js.st_mtimespec.tv_sec&&st.st_mtimespec.tv_nsec>=js.st_mtimespec.tv_nsec);
#else
            fresh=st.st_mtim.tv_sec>js.st_mtim.tv_sec||
                (st.st_mtim.tv_sec==js.st_mtim.tv_sec&&st.st_mtim.tv_nsec>=js.st_mtim.tv_nsec);
#endif
        }
        if(jf>=0)close(jf);
        if(!fresh){fclose(f);return NULL;}
    }
    return f;
#endif
}

static uint32_t png_u32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

/* stb's malloc-based PNG inflater can grow beyond the declared dimensions.
 * Preflight the exact IDAT stream with its fixed-buffer inflater first. */
static int thumbnail_inflate_bounded(const unsigned char *data, size_t n)
{
    unsigned char *idat = malloc(n);
    char *raw = malloc(4 * 1024 * 1024);
    size_t used = 0, pos = 8;
    int ok = 0, end = 0;
    if (!idat || !raw) goto done;
    while (pos + 12 <= n) {
        uint32_t len = png_u32(data + pos);
        if (len > n - pos - 12) goto done;
        if (!memcmp(data + pos + 4, "IDAT", 4)) {
            memcpy(idat + used, data + pos + 8, len);
            used += len;
        }
        if (!memcmp(data + pos + 4, "IEND", 4)) { end = 1; break; }
        pos += (size_t)len + 12;
    }
    if (end && used && stbi_zlib_decode_buffer(raw, 4 * 1024 * 1024,
                                              (const char *)idat, (int)used) > 0)
        ok = 1;
done:
    free(raw); free(idat);
    return ok;
}

static void thumbnail_load(od_ui_catalog *c, const od_catalog_entry *en,
                           od_ucat_thumbnail *t)
{
    if (t->attempted && !strcmp(t->file, en->thumb)) return;
    memset(t, 0, sizeof *t);
    snprintf(t->file, sizeof t->file, "%s", en->thumb);
    t->attempted = 1;
    if (!en->thumb[0]) return;
    FILE *f = thumbnail_file(c, en);
    if (!f) return;
    unsigned char *data = malloc(1024 * 1024 + 1);
    if (!data) { fclose(f); return; }
    size_t n = fread(data, 1, 1024 * 1024 + 1, f);
    int bad = ferror(f);
    fclose(f);
    int w = 0, h = 0, channels = 0;
    unsigned char *rgba = NULL;
    if (bad || n > 1024 * 1024 || n < 33 ||
        memcmp(data, "\211PNG\r\n\032\n", 8) ||
        !stbi_info_from_memory(data, (int)n, &w, &h, &channels) ||
        w <= 0 || h <= 0 || w > 512 || h > 512 ||
        !thumbnail_inflate_bounded(data, n)) goto done;
    rgba = stbi_load_from_memory(data, (int)n, &w, &h, &channels, 4);
    if (!rgba || w <= 0 || h <= 0 || w > 512 || h > 512) goto done;
    t->w = w >= h ? OD_UCAT_THUMB_SIZE : OD_UCAT_THUMB_SIZE * w / h;
    t->h = h >= w ? OD_UCAT_THUMB_SIZE : OD_UCAT_THUMB_SIZE * h / w;
    if (!t->w) t->w = 1;
    if (!t->h) t->h = 1;
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            const unsigned char *p = rgba + 4 * ((y * h / t->h) * w + x * w / t->w);
            t->pixels[y * OD_UCAT_THUMB_SIZE + x] = (uint32_t)p[3] << 24 |
                (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
done:
    stbi_image_free(rgba);
    free(data);
}

static void fill_rect(uint32_t *cv, int cw, int ch, int x, int y, int w, int h,
                      uint32_t argb)
{
    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= ch) continue;
        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= cw) continue;
            cv[(size_t)py * cw + px] = argb;
        }
    }
}

static void border(uint32_t *cv, int cw, int ch, int x, int y, int w, int h,
                   uint32_t argb)
{
    fill_rect(cv, cw, ch, x, y, w, 1, argb);
    fill_rect(cv, cw, ch, x, y + h - 1, w, 1, argb);
    fill_rect(cv, cw, ch, x, y, 1, h, argb);
    fill_rect(cv, cw, ch, x + w - 1, y, 1, h, argb);
}

/* Text cut to `w` pixels (ending in "..." when it does not fit). */
static void text_in(uint32_t *cv, int cw, int ch, int x, int y, int w,
                    const char *s, uint32_t argb)
{
    char buf[200];
    od_ui_font_text(cv, cw, ch, x, y, od_ui_font_fit(s, w, 1, buf, sizeof buf),
                     argb, 1);
}

/* The path field keeps its end (where typing happens) visible: a long path
 * shows "Path: ..." and its last characters. */
static void path_text(const od_ui_catalog *c, int w, char *out, size_t cap)
{
    const char *fmt = od_str(OD_STR_PATH);
    const char *pct = strchr(fmt, '%');
    size_t plen = pct ? (size_t)(pct - fmt) : strlen(fmt);
    int max = w / OD_UI_FONT_W;
    const char *cursor = c->editing_path ? "_" : "";
    size_t len = strlen(c->io_path) + strlen(cursor);
    if ((int)(plen + len) <= max || max < (int)plen + 4) {
        char full[1300];
        snprintf(full, sizeof full, "%.*s%s%s", (int)plen, fmt, c->io_path, cursor);
        od_ui_font_fit(full, w, 1, out, cap);
        return;
    }
    size_t keep = (size_t)max - plen - 3 - strlen(cursor);
    const char *tail = c->io_path + strlen(c->io_path) - keep;
    snprintf(out, cap, "%.*s...%s%s", (int)plen, fmt, tail, cursor);
}

void od_ucat_draw(od_ui_catalog *c, uint32_t *cv, int cw, int ch)
{
    if (c == NULL || cv == NULL)
        return;
    fill_rect(cv, cw, ch, 0, 0, cw, ch, C_PANEL);
    fill_rect(cv, cw, ch, 0, 0, cw, TITLE_H, C_SEL_BG);
    {
        char title[64];
        snprintf(title, sizeof title, od_str(OD_STR_CATALOG_TITLE), c->n);
        text_in(cv, cw, ch, 4, (TITLE_H - OD_UI_FONT_H) / 2, cw - 8, title,
                C_TITLE);
    }

    int vis = LIST_H / ROW_H;
    for (int row = 0; row < vis; row++) {
        int idx = c->scroll + row;
        if (idx >= c->n)
            break;
        int rx, ry, rw, rh;
        od_ucat_row_rect(row, &rx, &ry, &rw, &rh);
        int selected = idx == c->focus;
        if (selected)
            fill_rect(cv, cw, ch, rx, ry, rw, rh, C_SEL_BG);
        const od_catalog_entry *en = &c->entries[idx];
        od_ucat_thumbnail *thumb = &c->thumbnails[row];
        thumbnail_load(c, en, thumb);
        int tx = rx + 2 + (OD_UCAT_THUMB_SIZE - thumb->w) / 2;
        int ty = ry + 1 + (OD_UCAT_THUMB_SIZE - thumb->h) / 2;
        for (int y = 0; y < thumb->h; y++)
            for (int x = 0; x < thumb->w; x++) {
                int px = tx + x, py = ty + y;
                if (px < 0 || px >= cw || py < 0 || py >= ch) continue;
                uint32_t src = thumb->pixels[y * OD_UCAT_THUMB_SIZE + x];
                uint32_t dst = cv[(size_t)py * cw + px];
                unsigned a = src >> 24;
                uint32_t out = 0xFF000000u;
                for (int shift = 0; shift <= 16; shift += 8)
                    out |= ((((src >> shift) & 255) * a +
                             ((dst >> shift) & 255) * (255 - a)) / 255) << shift;
                cv[(size_t)py * cw + px] = out;
            }
        char line[200];
        char when[16] = "";
        time_t t = (time_t)en->modified;
        struct tm *tm = localtime(&t);
        if (tm)
            strftime(when, sizeof when, "%y-%m-%d", tm);
        int text_w = rw - TEXT_X - 4;
        snprintf(line, sizeof line, "%s", en->name[0] ? en->name : en->id);
        text_in(cv, cw, ch, rx + TEXT_X, ry + 6, text_w, line,
                selected ? C_SEL_TX : C_TEXT);
        /* The author is cut first so the world and the date stay visible. */
        char tail[48], author[120];
        snprintf(tail, sizeof tail, "  W%d  %s", en->theme_world, when);
        int author_w = text_w - od_ui_font_width(tail, 1);
        od_ui_font_fit(en->author[0] ? en->author : od_str(OD_STR_ANON),
                        author_w, 1, author, sizeof author);
        snprintf(line, sizeof line, "%s%s", author, tail);
        text_in(cv, cw, ch, rx + TEXT_X, ry + 20, text_w, line,
                selected ? C_SEL_TX : C_MUTED);
    }
    if (c->n == 0) {
        text_in(cv, cw, ch, 8, LIST_Y + 8, cw - 16,
                od_str(OD_STR_CATALOG_EMPTY), C_MUTED);
    }

    /* Path field. */
    fill_rect(cv, cw, ch, 2, PATH_Y, cw - 4, PATH_H, 0xFF080810u);
    border(cv, cw, ch, 2, PATH_Y, cw - 4, PATH_H, c->editing_path ? C_TITLE : C_BORDER);
    {
        char field[320];
        path_text(c, cw - 12, field, sizeof field);
        od_ui_font_text(cv, cw, ch, 6, PATH_Y + (PATH_H - OD_UI_FONT_H) / 2,
                         field, c->editing_path ? C_SEL_TX : C_TEXT, 1);
    }

    /* Buttons. */
    for (int i = 0; i < OD_UCAT_BTN_COUNT; i++) {
        int bx, by, bw, bh;
        od_ucat_button_rect(k_buttons[i].btn, &bx, &by, &bw, &bh);
        fill_rect(cv, cw, ch, bx, by, bw, bh, C_BTN);
        border(cv, cw, ch, bx, by, bw, bh, C_BORDER);
        char label[32];
        od_ui_font_fit(od_str(k_buttons[i].label), bw - 4, 1, label, sizeof label);
        int lw = od_ui_font_width(label, 1);
        od_ui_font_text(cv, cw, ch, bx + (bw - lw) / 2,
                         by + (bh - OD_UI_FONT_H) / 2, label, C_TEXT, 1);
    }
    text_in(cv, cw, ch, 4, HELP_Y, cw - 8, od_str(OD_STR_CATALOG_HELP), C_MUTED);

    /* Every message is English at its source (C6.32, UI-18). */
    const char *status = c->status;
    char lines[2][64];
    int n = od_ui_font_wrap(status, cw - 8, 1, &lines[0][0], sizeof lines[0], 2);
    for (int i = 0; i < n; i++)
        od_ui_font_text(cv, cw, ch, 4, STATUS_Y + i * STATUS_LINE_H, lines[i],
                         c->status_error ? C_ERR : C_OK, 1);

    if (c->confirm_delete) {
        fill_rect(cv, cw, ch, CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, C_PANEL);
        border(cv, cw, ch, CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, C_TITLE);
        char question[128];
        snprintf(question, sizeof question, od_str(OD_STR_CONFIRM_DELETE_LEVEL),
                 c->confirm_id);
        text_in(cv, cw, ch, CONFIRM_X + 8, CONFIRM_Y + 12, CONFIRM_W - 16,
                question, C_TITLE);
        for (int yes = 0; yes <= 1; yes++) {
            int bx, by, bw, bh;
            od_ucat_confirm_button_rect(yes, &bx, &by, &bw, &bh);
            fill_rect(cv, cw, ch, bx, by, bw, bh, C_BTN);
            border(cv, cw, ch, bx, by, bw, bh, C_BORDER);
            const char *label = od_str(yes ? OD_STR_YES : OD_STR_NO);
            od_ui_font_text(cv, cw, ch, bx + (bw - od_ui_font_width(label, 1)) / 2,
                             by + (bh - OD_UI_FONT_H) / 2, label, C_TEXT, 1);
        }
    }
}
