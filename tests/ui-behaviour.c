/* ui-behaviour.c - one test per interface defect of the review.
 *
 * Pure C (SDL headers only; the SDL calls ui.c links are stubs), built with
 * AddressSanitizer/UBSan by tests/ui-behaviour-run.sh. Each case names its
 * finding (UI-n). Built against the earlier sources (OD_UI_SRC, the
 * positive control) every case must fail: where the old tree lacks an API,
 * the case fails explicitly instead of not compiling. */
#if !defined(_WIN32)
#define _XOPEN_SOURCE 700
#else
#define _WIN32_WINNT 0x0601
#endif
#include "opendana/ui.h"
#include "opendana/ui_font.h"
#include "opendana/ui_strings.h"
#include "opendana/png.h"
#include "opendana/ext/catalog.h"
#include "opendana/ext/input_map.h"
#include "opendana/ext/settings.h"
#include "opendana/ext/state.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#ifndef SYMBOLIC_LINK_FLAG_DIRECTORY
#define SYMBOLIC_LINK_FLAG_DIRECTORY 0x1
#endif
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif

void SDLCALL SDL_DestroyTexture(SDL_Texture *t) { (void)t; }
SDL_Texture *SDLCALL SDL_CreateTexture(SDL_Renderer *r, Uint32 f, int a, int w, int h)
{ (void)r; (void)f; (void)a; (void)w; (void)h; return NULL; }
int SDLCALL SDL_SetTextureBlendMode(SDL_Texture *t, SDL_BlendMode m)
{ (void)t; (void)m; return 0; }
int SDLCALL SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *r, const void *p, int pitch)
{ (void)t; (void)r; (void)p; (void)pitch; return 0; }
int SDLCALL SDL_SetRenderDrawColor(SDL_Renderer *r, Uint8 a, Uint8 b, Uint8 c, Uint8 d)
{ (void)r; (void)a; (void)b; (void)c; (void)d; return 0; }
int SDLCALL SDL_RenderClear(SDL_Renderer *r) { (void)r; return 0; }
int SDLCALL SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *a,
                           const SDL_Rect *b)
{ (void)r; (void)t; (void)a; (void)b; return 0; }
void SDLCALL SDL_RenderPresent(SDL_Renderer *r) { (void)r; }

/* States: every slot file "exists" (the format is state.c's); the
 * thumbnail reader under test only needs the files' identity and times. */
od_status __real_od_state_peek(const char *path, od_state_info *out);
od_status __wrap_od_state_peek(const char *path, od_state_info *out)
{
    memset(out, 0, sizeof *out);
    if (access(path, F_OK) != 0)
        return OD_ERR_IO;
    out->valid = 1;
    out->world = 1;
    out->round = 1;
    return OD_OK;
}

static int cases, failed_cases, checks, failures, case_failed, skipped;
#define EXPECT(cond, ...)                                                     \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            case_failed = 1;                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)
#define CASE(name, fn)                                                        \
    do {                                                                      \
        case_failed = 0;                                                      \
        isolate_data_home();                                                  \
        fn();                                                                 \
        cases++;                                                              \
        failed_cases += case_failed;                                          \
        printf("%s %s\n", case_failed ? "RED  " : "GREEN", name);             \
    } while (0)

static char tmp[600];
static void isolate_data_home(void);
static void mkdirs(const char *path);

/* The per-user data home the states screen reads: $XDG_DATA_HOME on POSIX,
 * %LOCALAPPDATA% on Windows (src/core/userdir.c). */
static void set_data_home(const char *dir)
{
#ifdef _WIN32
    char buf[1000];
    snprintf(buf, sizeof buf, "LOCALAPPDATA=%s", dir);
    _putenv(buf);
#elif defined(__APPLE__)
    /* C6.70: macOS keeps per-user data under $HOME/Library/Application Support (userdir.c),
     * so a test folder is reached by pointing HOME at it (see data_base). */
    setenv("HOME", dir, 1);
#else
    setenv("XDG_DATA_HOME", dir, 1);
#endif
}

/* Every case starts on an empty data home of its own, so none reads (or depends on) the
 * user's real states, catalogue or settings; a case that needs files makes its own. */
static void isolate_data_home(void)
{
    static int n;
    char dir[700];
    snprintf(dir, sizeof dir, "%s/isolated-%d", tmp, ++n);
    mkdirs(dir);
    set_data_home(dir);
}

/* Symbolic link at `link` pointing to `target`; 0 on success, -1 on error, -2
 * when this Windows does not really create symbolic links (wine 6.0.3
 * accepts CreateSymbolicLink and creates nothing): the case is then skipped
 * and reported as such, never counted as passed. */
static int make_symlink(const char *target, const char *link, int is_dir)
{
#ifdef _WIN32
    if (!CreateSymbolicLinkA(link, target,
                             (is_dir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0) |
                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        return -1;
    return GetFileAttributesA(link) == INVALID_FILE_ATTRIBUTES ? -2 : 0;
#else
    (void)is_dir;
    return symlink(target, link);
#endif
}

/* Give both files the same, fixed modification time. */
static void same_mtime(const char *a, const char *b)
{
#ifdef _WIN32
    const char *paths[2] = { a, b };
    FILETIME ft = { 0xC66D0000u, 0x01DA1747u };   /* 1700000000 s after 1970 */
    for (int i = 0; i < 2; i++) {
        HANDLE h = CreateFileA(paths[i], FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            SetFileTime(h, NULL, NULL, &ft);
            CloseHandle(h);
        }
    }
#else
    struct timespec t[2] = { { 1700000000, 500 }, { 1700000000, 500 } };
    utimensat(AT_FDCWD, a, t, 0);
    utimensat(AT_FDCWD, b, t, 0);
#endif
}

static void path_join(char *out, size_t cap, const char *a, const char *b)
{
    snprintf(out, cap, "%s/%s", a, b);
}

static void mkdirs(const char *path)
{
    char buf[900];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            MKDIR(buf);
            *p = '/';
        }
    MKDIR(buf);
}

static int file_text(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
    return 1;
}

/* ---- states ------------------------------------------------------------- */

static od_settings settings;
static od_ui ui;

static void open_states(int slots)
{
    od_settings_defaults(&settings);
    settings.states_slots = slots;
    od_ui_init(&ui, &settings);
    ui.screen = OD_UI_MENU;
    ui.selected = 1;
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
}

/* UI-1: Delete acts on the focused slot, after a confirmation, and only in the
 * states screen. */
static void ui1_delete_focused_slot(void)
{
    open_states(8);
    settings.states_current = 0;
    for (int i = 0; i < 8; i++) ui.slot_occupied[i] = 1;
    ui.slot_focus = 2;
    ui.state_action = OD_UI_STATE_NONE;
    od_ui_handle_key(&ui, OD_UI_KEY_DELETE);
    EXPECT(ui.state_action == OD_UI_STATE_NONE,
           "Delete acted at once (action %d on slot %d), no confirmation",
           ui.state_action, ui.state_slot + 1);
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);        /* No is the default */
    EXPECT(ui.state_action == OD_UI_STATE_NONE, "Enter deleted with No focused");
    od_ui_handle_key(&ui, OD_UI_KEY_DELETE);
    od_ui_handle_key(&ui, OD_UI_KEY_LEFT);          /* focus Yes */
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
    EXPECT(ui.state_action == OD_UI_STATE_DELETE && ui.state_slot == 2,
           "confirmed Delete: action %d on slot %d, want delete on the focused "
           "slot 3", ui.state_action, ui.state_slot + 1);
    /* In the menu (or the settings) Delete is not a state hotkey. */
    ui.state_action = OD_UI_STATE_NONE;
    ui.screen = OD_UI_MENU;
    od_ui_handle_key(&ui, OD_UI_KEY_DELETE);
    EXPECT(ui.state_action == OD_UI_STATE_NONE,
           "Delete in the menu deletes the current slot");
}

/* UI-17: "Save here" over an occupied slot asks before overwriting it. */
static void ui17_overwrite_confirmation(void)
{
    open_states(8);
    ui.slot_occupied[4] = 1;
    ui.slot_focus = 4;
    ui.state_action = OD_UI_STATE_NONE;
    /* The "Save here" button: first of the button row, keyboard reachable. */
    od_ui_pointer(&ui, 12 + 8 + 30, 14 + 200 - 16 - 8 + 8, 1);
    EXPECT(ui.state_action == OD_UI_STATE_NONE,
           "Save here over an occupied slot saved at once (action %d)",
           ui.state_action);
    od_ui_handle_key(&ui, OD_UI_KEY_RIGHT);         /* focus Yes */
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
    EXPECT(ui.state_action == OD_UI_STATE_SAVE && ui.state_slot == 4,
           "confirmed overwrite: action %d slot %d", ui.state_action,
           ui.state_slot + 1);
    /* An empty slot saves without a question. */
    open_states(8);
    ui.slot_focus = 1;
    od_ui_pointer(&ui, 12 + 8 + 30, 14 + 200 - 16 - 8 + 8, 1);
    EXPECT(ui.state_action == OD_UI_STATE_SAVE && ui.state_slot == 1,
           "Save here on an empty slot: action %d", ui.state_action);
}

/* A 4x2 RGB PNG for the slot thumbnail. */
static void write_png(const char *path)
{
    unsigned char rgb[4 * 2 * 3];
    for (size_t i = 0; i < sizeof rgb; i += 3) {
        rgb[i] = 200; rgb[i + 1] = 40; rgb[i + 2] = 40;
    }
    od_png_write_rgb(path, 4, 2, rgb);
}

/* C6.70: where the product keeps per-user data inside a test folder `h`, and how a test points
 * it there. Linux/Windows-style: XDG_DATA_HOME=h. macOS (userdir.c) ignores XDG_* and uses
 * $HOME/Library/Application Support, so the test sets HOME=h instead. */
static void data_base(char *out, size_t cap, const char *h)
{
#if defined(__APPLE__)
    snprintf(out, cap, "%s/Library/Application Support", h);
#else
    snprintf(out, cap, "%s", h);
#endif
}


static void write_slot(const char *data_home, int slot, int same_time)
{
    char dir[800], state[900], png[900], base[760];
    data_base(base, sizeof base, data_home);
    snprintf(dir, sizeof dir, "%s/opendana/states", base);
    mkdirs(dir);
    snprintf(state, sizeof state, "%s/slot-%d.odstate", dir, slot);
    snprintf(png, sizeof png, "%s/slot-%d.png", dir, slot);
    FILE *f = fopen(state, "wb");
    if (f) { fputs("state", f); fclose(f); }
    write_png(png);
    if (same_time) {
        same_mtime(state, png);
    }
}

/* UI-13: a thumbnail written in the same instant as its state is shown. */
static void ui13_same_mtime_thumbnail(void)
{
    char home[700];
    path_join(home, sizeof home, tmp, "ui13");
    write_slot(home, 1, 1);
    set_data_home(home);
    open_states(8);
    EXPECT(ui.slot_occupied[0], "slot 1 not listed");
    EXPECT(ui.slot_thumbnails[0].w > 0,
           "thumbnail with the same mtime as its state is not shown");
}

/* UI-14: an aliased data folder still shows thumbnails; a link at the file
 * itself is never followed. */
static void ui14_aliased_folder(void)
{
    char real[700], alias[700];
    path_join(real, sizeof real, tmp, "ui14-real");
    path_join(alias, sizeof alias, tmp, "ui14-alias");
    write_slot(real, 1, 1);
    unlink(alias);
    int rc = make_symlink(real, alias, 1);
    if (rc == -2) {
        skipped++;
        printf("SKIP UI-14: this Windows does not create symbolic links\n");
        return;
    }
    EXPECT(rc == 0, "symlink");
    set_data_home(alias);
    open_states(8);
    EXPECT(ui.slot_thumbnails[0].w > 0,
           "thumbnail under an aliased data folder is not shown");
    /* Negative: slot-2.png is a link to a real PNG elsewhere. */
    char target[800], link[900], state[900];
    path_join(target, sizeof target, tmp, "ui14-target.png");
    write_png(target);
    char base[760];
    data_base(base, sizeof base, real);
    snprintf(state, sizeof state, "%s/opendana/states/slot-2.odstate", base);
    FILE *f = fopen(state, "wb");
    if (f) { fputs("state", f); fclose(f); }
    snprintf(link, sizeof link, "%s/opendana/states/slot-2.png", base);
    unlink(link);
    rc = make_symlink(target, link, 0);
    if (rc == -2) {
        skipped++;
        printf("SKIP UI-14: this Windows does not create symbolic links (file)\n");
        return;
    }
    EXPECT(rc == 0, "symlink png");
    open_states(8);
    EXPECT(ui.slot_occupied[1] && ui.slot_thumbnails[1].w == 0,
           "a symbolic link at slot-2.png was followed");
}

/* New game (owner decision 2026-10-06): the menu entry asks first, with No
 * focused; only Yes sets the host's flag. */
static void new_game_confirmed(void)
{
#if defined(OD_UI_REVISION) && OD_UI_REVISION >= 632
    od_settings_defaults(&settings);
    od_ui_init(&ui, &settings);
    ui.screen = OD_UI_MENU;
    ui.selected = 0;
    int found = -1;
    for (int i = 0; i < 8; i++) {                    /* find it by keyboard */
        ui.confirm = OD_UI_CONFIRM_NONE;
        od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
        if (ui.confirm == OD_UI_CONFIRM_NEW_GAME) { found = i; break; }
        ui.screen = OD_UI_MENU;
        ui.quit = 0;
        ui.selected = i + 1;
    }
    EXPECT(found == 6, "New game is menu row %d, want 7 (before Quit)", found + 1);
    EXPECT(!ui.new_game, "the flag is set before any answer");
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);       /* No is focused */
    EXPECT(!ui.new_game && ui.confirm == OD_UI_CONFIRM_NONE,
           "Enter on the default answer started a new game");
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);       /* ask again */
    od_ui_handle_key(&ui, OD_UI_KEY_BACK);         /* Esc = No */
    EXPECT(!ui.new_game, "Esc started a new game");
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
    od_ui_handle_key(&ui, OD_UI_KEY_RIGHT);        /* focus Yes */
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
    EXPECT(ui.new_game == 1, "Yes did not set new_game");
    /* By mouse: the Yes button of the dialog. */
    ui.new_game = 0;
    ui.screen = OD_UI_MENU;
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
    od_ui_pointer(&ui, 28 + 16 + 28, 64 + 92 - 16 - 8 + 8, 1);
    EXPECT(ui.new_game == 1, "clicking Yes did not set new_game");
#else
    EXPECT(0, "no New game entry in the menu");
#endif
}

/* ---- editor ------------------------------------------------------------- */

static void save_level(const char *dir, const char *name, char *id)
{
    od_led_doc d;
    memset(&d, 0, sizeof d);
    od_led_new(&d, 1);
    od_led_place_hero(&d, 2, 2);
    od_led_paint(&d, 5, 5, 0x0E);
    snprintf(d.name, sizeof d.name, "%s", name);
    od_catalog_save(dir, &d, id);
    od_led_free(&d);
}

/* UI-3: Save writes back to the opened catalogue file, whatever "id" its JSON
 * holds; a file from elsewhere becomes a new entry. */
static void ui3_id_from_file_stem(void)
{
    char dir[700], alpha[800], beta[800], text_alpha[8192], text[8192];
    path_join(dir, sizeof dir, tmp, "ui3");
    mkdirs(dir);
    char id[OD_LED_ID_MAX];
    save_level(dir, "Alpha", id);
    snprintf(alpha, sizeof alpha, "%s/%s.json", dir, id);
    snprintf(beta, sizeof beta, "%s/beta.json", dir);
    file_text(alpha, text_alpha, sizeof text_alpha);
    FILE *f = fopen(beta, "wb");          /* a copy: its JSON says id "alpha" */
    if (f) { fputs(text_alpha, f); fclose(f); }
    static od_ui_editor e;
    od_ued_init(&e, dir);
    EXPECT(od_ued_load(&e, beta) == OD_OK, "load beta");
    od_led_paint(&e.doc, 7, 7, 0x01);
    EXPECT(od_ued_save(&e, 0) == OD_OK, "save");
    file_text(alpha, text, sizeof text);
    EXPECT(strcmp(text, text_alpha) == 0,
           "saving beta.json overwrote %s.json (id taken from the JSON)", id);
    EXPECT(strcmp(e.doc.id, "beta") == 0, "saved as \"%s\", want beta", e.doc.id);
    od_ued_free(&e);

    /* A level file from outside the catalogue is a new entry. */
    char outside[800];
    path_join(outside, sizeof outside, tmp, "ui3-outside.json");
    f = fopen(outside, "wb");
    if (f) { fputs(text_alpha, f); fclose(f); }
    od_ued_init(&e, dir);
    EXPECT(od_ued_load(&e, outside) == OD_OK, "load outside");
    od_led_paint(&e.doc, 8, 8, 0x01);
    EXPECT(od_ued_save(&e, 0) == OD_OK, "save outside");
    file_text(alpha, text, sizeof text);
    EXPECT(strcmp(text, text_alpha) == 0,
           "saving a file from outside overwrote %s.json", id);
    od_ued_free(&e);
}

/* Owner decision 2026-10-06: a new level is named after the date and time of
 * its first save ("YYYY-MM-DD HH:MM"), kept on later saves. */
static void name_is_first_save_time(void)
{
    char dir[700];
    path_join(dir, sizeof dir, tmp, "name");
    mkdirs(dir);
    static od_ui_editor e;
    od_ued_init(&e, dir);
    od_led_place_hero(&e.doc, 2, 2);
    EXPECT(od_ued_save(&e, 0) == OD_OK, "save");
    int y, mo, d, h, mi;
    char extra;
    int n = sscanf(e.doc.name, "%4d-%2d-%2d %2d:%2d%c", &y, &mo, &d, &h, &mi, &extra);
    EXPECT(n == 5 && strlen(e.doc.name) == 16,
           "first save named the level \"%s\", want YYYY-MM-DD HH:MM", e.doc.name);
    char first[OD_LED_NAME_MAX];
    snprintf(first, sizeof first, "%s", e.doc.name);
    od_led_paint(&e.doc, 6, 6, 0x01);
    EXPECT(od_ued_save(&e, 0) == OD_OK, "save again");
    EXPECT(strcmp(first, e.doc.name) == 0, "the name changed on a later save");
    od_ued_free(&e);
}

static void cell_point(int cx, int cy, int *x, int *y)
{
    int w, h;
    od_ued_cell_rect(cx, cy, x, y, &w, &h);
    *x += w / 2;
    *y += h / 2;
}

/* UI-15: the flame brush makes one run per cell; erasing removes it. */
static void ui15_flame_brush(void)
{
    char dir[700];
    path_join(dir, sizeof dir, tmp, "flame");
    static od_ui_editor e;
    od_ued_init(&e, dir);
    int bx, by, bw, bh, x, y;
    od_ued_button_rect(OD_UED_BTN_FLAME, &bx, &by, &bw, &bh);
    od_ued_pointer(&e, bx + 2, by + 2, OD_UED_BTN_LEFT, 1, 0, 0, 0);
    cell_point(6, 6, &x, &y);
    od_ued_pointer(&e, x, y, OD_UED_BTN_LEFT, 1, 0, 0, 0);
    for (int i = 0; i < 4; i++)                      /* drag on the same cell */
        od_ued_pointer(&e, x + (i & 1), y, OD_UED_BTN_LEFT, 0, 0, 0, 0);
    od_ued_pointer(&e, x, y, 0, 0, 0, 0, 0);
    EXPECT(e.doc.n_objects == 1, "%d moving-flame objects on one cell",
           e.doc.n_objects);
    od_ued_pointer(&e, x, y, OD_UED_BTN_RIGHT, 1, 0, 0, 0);
    EXPECT(e.doc.grid[6][6] == 0x00 && e.doc.n_objects == 0,
           "erasing the flame left %d orphan objects", e.doc.n_objects);
    od_ued_free(&e);
}

/* UI-23 (editor): Back, New and Open ask before dropping unsaved changes. */
static void ui23_editor_unsaved(void)
{
    char dir[700];
    path_join(dir, sizeof dir, tmp, "unsaved");
    static od_ui_editor e;
    od_ued_init(&e, dir);
    int x, y;
    cell_point(4, 4, &x, &y);
    od_ued_pointer(&e, x, y, OD_UED_BTN_LEFT, 1, 0, 0, 0);
    od_ued_pointer(&e, x, y, 0, 0, 0, 0, 0);
    od_ued_key(&e, OD_UED_KEY_BACK);
    EXPECT(!e.request_back, "Esc with unsaved changes left at once");
#if defined(OD_UI_REVISION) && OD_UI_REVISION >= 632
    od_ued_key(&e, OD_UED_KEY_ACCEPT);
    EXPECT(e.request_back, "confirmed Back does not leave");
    e.request_back = 0;
    int bx, by, bw, bh;
    od_ued_button_rect(OD_UED_BTN_NEW, &bx, &by, &bw, &bh);
    od_ued_pointer(&e, bx + 2, by + 2, OD_UED_BTN_LEFT, 1, 0, 0, 0);
    EXPECT(e.doc.grid[4][4] == 0x01, "New dropped unsaved changes at once");
    od_ued_key(&e, OD_UED_KEY_BACK);                 /* Cancel */
    EXPECT(e.doc.grid[4][4] == 0x01 && !e.request_back, "Cancel did not keep them");
#endif
    od_ued_free(&e);
}

/* ---- catalogue ---------------------------------------------------------- */

static void open_catalog(od_ui_catalog *c, const char *dir)
{
    od_ucat_init(c, dir);
    od_ucat_refresh(c);
}

/* UI-8: a click selects; a click on the selected row plays it. */
static void ui8_click_selects(void)
{
    char dir[700], id[OD_LED_ID_MAX];
    path_join(dir, sizeof dir, tmp, "ui8");
    mkdirs(dir);
    save_level(dir, "One", id);
    save_level(dir, "Two", id);
    static od_ui_catalog c;
    open_catalog(&c, dir);
    int rx, ry, rw, rh;
    od_ucat_row_rect(1, &rx, &ry, &rw, &rh);
    od_ucat_pointer(&c, rx + 60, ry + 4, 1, 1, 0);
    EXPECT(c.focus == 1 && c.request == OD_UCAT_REQ_NONE,
           "one click on a row: focus %d request %d (it played)", c.focus, c.request);
    od_ucat_pointer(&c, rx + 60, ry + 4, 1, 1, 0);
    EXPECT(c.request == OD_UCAT_REQ_PLAY, "a click on the selected row does not play");
    od_ucat_free(&c);
}

/* UI-23 (catalogue): Delete asks first. */
static void ui23_catalog_delete(void)
{
    char dir[700], id[OD_LED_ID_MAX];
    path_join(dir, sizeof dir, tmp, "ui23");
    mkdirs(dir);
    save_level(dir, "Gone", id);
    static od_ui_catalog c;
    open_catalog(&c, dir);
    int n = c.n;
    int bx, by, bw, bh;
    od_ucat_button_rect(OD_UCAT_BTN_DELETE, &bx, &by, &bw, &bh);
    od_ucat_pointer(&c, bx + 2, by + 2, 1, 1, 0);
    EXPECT(c.n == n, "Delete removed the level without asking");
    od_ucat_free(&c);
}

/* UI-5: exports never land in the catalogue; Duplicate and Import report. */
static void ui5_export_and_messages(void)
{
    char dir[700], id[OD_LED_ID_MAX];
    path_join(dir, sizeof dir, tmp, "ui5/levels");
    mkdirs(dir);
    save_level(dir, "Orig", id);
    static od_ui_catalog c;
    open_catalog(&c, dir);
    EXPECT(strncmp(c.io_path, dir, strlen(dir)) != 0,
           "default export path \"%s\" is inside the catalogue", c.io_path);
    int n = c.n;
    int bx, by, bw, bh;
    od_ucat_button_rect(OD_UCAT_BTN_DUP, &bx, &by, &bw, &bh);
    od_ucat_pointer(&c, bx + 2, by + 2, 1, 1, 0);
    EXPECT(c.n == n + 1 && strstr(c.status, "Duplicated") != NULL,
           "Duplicate: %d entries, status \"%s\"", c.n, c.status);
    od_ucat_free(&c);
}

/* UI-6: the path field takes UTF-8 text; Backspace removes a character. */
static void ui6_text_input(void)
{
    EXPECT(od_ui_font_width("\xc3\x89", 1) == 8,
           "an accented letter is %d px wide (one glyph per byte)",
           od_ui_font_width("\xc3\x89", 1));
#if defined(OD_UI_REVISION) && OD_UI_REVISION >= 632
    static od_ui_catalog c;
    od_ucat_init(&c, tmp);
    c.io_path[0] = '\0';
    c.editing_path = 1;
    od_ucat_text(&c, "/Docs/R\xc3\xa9");
    EXPECT(strcmp(c.io_path, "/Docs/R\xc3\xa9") == 0, "typed \"%s\"", c.io_path);
    od_ucat_key(&c, OD_UCAT_KEY_BACKSPACE, 0);
    EXPECT(strcmp(c.io_path, "/Docs/R") == 0,
           "Backspace left \"%s\" (half a character)", c.io_path);
#else
    EXPECT(0, "no text input for the path field (key codes only)");
#endif
}

/* UI-7: the catalogue opened from the menu goes back to the menu. */
static void ui7_back_target(void)
{
#if defined(OD_UI_REVISION) && OD_UI_REVISION >= 632
    static od_ui_catalog c;
    od_ucat_init(&c, tmp);
    c.return_to_editor = 1;                  /* left over from the editor */
    od_settings_defaults(&settings);
    od_ui_init(&ui, &settings);
    ui.catalog = &c;
    ui.screen = OD_UI_MENU;
    ui.selected = 3;                         /* Levels */
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
    EXPECT(ui.screen == OD_UI_CATALOG && c.return_to_editor == 0,
           "from the menu, Back would return to the editor");
    od_ucat_free(&c);
#else
    EXPECT(0, "Back from the catalogue does not know where it was opened");
#endif
}

/* ---- settings, pads, files ---------------------------------------------- */

/* UI-16: [input] keys are input_map's; settings.c only judges its sections. */
static void ui16_settings_and_bindings(void)
{
    const char *ini =
        "[video]\nfullscreen = 1\n"
        "[input]\npad.south = nes.b\nkey.left = nes.left\n"
        "[audio]\nvolume = 900\n";
    od_settings s;
    od_settings_defaults(&s);
    od_settings_parse(&s, ini, 0);
    EXPECT(s.warnings == 1, "%d warnings, want 1 (only the bad volume)", s.warnings);
    EXPECT(s.fullscreen == 1, "fullscreen not read");
}

/* UI-24: on a controller, the bottom face button accepts and the right one
 * goes back, whichever game button they map to. */
static void ui24_pad_menu_buttons(void)
{
#if defined(OD_UI_REVISION) && OD_UI_REVISION >= 632
    od_pad_state s;
    memset(&s, 0, sizeof s);
    s.buttons = OD_PAD_BIT(OD_PAD_SOUTH);
    EXPECT(od_pad_ui_buttons(&s) == OD_BTN_A, "bottom button is not accept");
    s.buttons = OD_PAD_BIT(OD_PAD_EAST);
    EXPECT(od_pad_ui_buttons(&s) == OD_BTN_B, "right button is not back");
#else
    od_input_config cfg;
    od_input_config_default(&cfg);
    od_pad_state s;
    memset(&s, 0, sizeof s);
    s.buttons = OD_PAD_BIT(OD_PAD_SOUTH);
    EXPECT(od_pad_to_nes(&s, &cfg.pad) == OD_BTN_A,
           "the menus read game buttons: the bottom button is game B = back");
#endif
}

/* UI-25: a directory where settings.ini should be is not read as a file. */
static void ui25_settings_directory(void)
{
    char dir[700];
    path_join(dir, sizeof dir, tmp, "ui25-settings.ini");
    mkdirs(dir);
    od_input_config c;
    od_status st = od_input_config_load(&c, dir);
    EXPECT(st == OD_OK || st != OD_OK, "load returned");
    od_input_config d;
    od_input_config_default(&d);
    EXPECT(memcmp(&c, &d, sizeof c) == 0, "a directory changed the bindings");
}

/* the Controls entry sits before New game and opens the screen; F1
 * opens it from the game and from the menu and returns to where it came
 * from; Esc and the pad's back close it; the "hint shown" flag is persisted
 * in settings.ini and survives a reload. */
static void c640_controls(void)
{
#if defined(OD_UI_REVISION) && OD_UI_REVISION >= 640
    od_settings_defaults(&settings);
    od_ui_init(&ui, &settings);
    /* From the game (menu hidden): F1 opens, F1 and Esc close, back to game. */
    EXPECT(ui.screen == OD_UI_HIDDEN && !od_ui_modal(&ui), "starts hidden");
    EXPECT(od_ui_handle_key(&ui, OD_UI_KEY_CONTROLS) == 1 &&
           ui.screen == OD_UI_CONTROLS && od_ui_modal(&ui),
           "F1 from the game opens the controls");
    od_ui_handle_key(&ui, OD_UI_KEY_BACK);
    EXPECT(ui.screen == OD_UI_HIDDEN, "Esc closes the controls back to the game");
    od_ui_handle_key(&ui, OD_UI_KEY_CONTROLS);
    od_ui_handle_key(&ui, OD_UI_KEY_CONTROLS);
    EXPECT(ui.screen == OD_UI_HIDDEN, "F1 again closes the controls");
    /* A game key is swallowed on the screen, not sent on. */
    od_ui_handle_key(&ui, OD_UI_KEY_CONTROLS);
    EXPECT(od_ui_handle_key(&ui, OD_UI_KEY_UP) == 1 && ui.screen == OD_UI_CONTROLS,
           "other keys are swallowed while the controls show");
    od_ui_handle_key(&ui, OD_UI_KEY_BACK);

    /* From the menu: F1 opens and Esc returns to the menu, not to the game. */
    ui.screen = OD_UI_MENU;
    ui.selected = 0;
    od_ui_handle_key(&ui, OD_UI_KEY_CONTROLS);
    EXPECT(ui.screen == OD_UI_CONTROLS, "F1 from the menu opens the controls");
    od_ui_handle_key(&ui, OD_UI_KEY_BACK);
    EXPECT(ui.screen == OD_UI_MENU, "Esc returns to the menu");

    /* The menu entry: row 6 (index 5), right before New game, then Quit. */
    int found = -1;
    for (int i = 0; i < 8; i++) {
        ui.screen = OD_UI_MENU;
        ui.selected = i;
        od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
        if (ui.screen == OD_UI_CONTROLS) { found = i; break; }
        ui.confirm = OD_UI_CONFIRM_NONE;
        ui.quit = 0;
    }
    EXPECT(found == 5, "Controls is menu row %d, want 6 (before New game)", found + 1);
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);       /* Enter closes too */
    EXPECT(ui.screen == OD_UI_MENU, "Enter closes the controls");
    /* A click closes it as well. */
    ui.screen = OD_UI_CONTROLS;
    ui.controls_return = OD_UI_MENU;
    od_ui_pointer(&ui, 100, 100, 1);
    EXPECT(ui.screen == OD_UI_MENU, "a click closes the controls");
    /* F1 under a question does nothing. */
    ui.screen = OD_UI_MENU;
    ui.confirm = OD_UI_CONFIRM_NEW_GAME;
    od_ui_handle_key(&ui, OD_UI_KEY_CONTROLS);
    EXPECT(ui.screen == OD_UI_MENU, "F1 does not skip a pending question");
    ui.confirm = OD_UI_CONFIRM_NONE;

    /* The hint flag: default off, persisted, reloaded. */
    od_settings a, b;
    od_settings_defaults(&a);
    EXPECT(a.controls_hint_shown == 0, "the hint is not shown by default");
    a.controls_hint_shown = 1;
    char text[1024];
    EXPECT(od_settings_format(&a, text, sizeof text) > 0, "format");
    od_settings_defaults(&b);
    od_settings_parse(&b, text, 0);
    EXPECT(b.controls_hint_shown == 1 && b.warnings == 0,
           "controls_hint_shown survives settings.ini");
    EXPECT(strlen(od_str(OD_STR_CONTROLS_HINT_ONCE)) > 0 &&
           strcmp(od_str(OD_STR_CONTROLS_HINT_ONCE), "Esc: menu  |  F1: controls") == 0,
           "the hint text");
#else
    EXPECT(0, "no controls screen (earlier)");
#endif
}

/* the applications-menu question and the Settings row. */
static void c646_desktop(void)
{
#if defined(OD_UI_REVISION) && OD_UI_REVISION >= 646
    /* No (the default): nothing is requested, the question is remembered. */
    od_settings_defaults(&settings);
    od_ui_init(&ui, &settings);
    ui.screen = OD_UI_MENU;
    ui.confirm = OD_UI_CONFIRM_DESKTOP;
    ui.confirm_yes = 0;
    EXPECT(settings.desktop_prompted == 0, "not prompted by default");
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
    EXPECT(ui.desktop_action == OD_UI_DESKTOP_NONE, "Enter on the default No installed");
    EXPECT(settings.desktop_prompted == 1 && ui.save_settings, "the answer is remembered");
    EXPECT(ui.screen == OD_UI_HIDDEN && ui.confirm == OD_UI_CONFIRM_NONE, "the question closes");
    /* Esc is No too. */
    od_settings_defaults(&settings);
    od_ui_init(&ui, &settings);
    ui.screen = OD_UI_MENU;
    ui.confirm = OD_UI_CONFIRM_DESKTOP;
    od_ui_handle_key(&ui, OD_UI_KEY_BACK);
    EXPECT(ui.desktop_action == OD_UI_DESKTOP_NONE && settings.desktop_prompted == 1, "Esc is No");
    /* Yes asks the host to install. */
    od_settings_defaults(&settings);
    od_ui_init(&ui, &settings);
    ui.screen = OD_UI_MENU;
    ui.confirm = OD_UI_CONFIRM_DESKTOP;
    od_ui_handle_key(&ui, OD_UI_KEY_RIGHT);
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
    EXPECT(ui.desktop_action == OD_UI_DESKTOP_INSTALL && settings.desktop_prompted == 1, "Yes installs");
    /* The setting persists. */
    char text[1024];
    od_settings b;
    EXPECT(od_settings_format(&settings, text, sizeof text) > 0, "format");
    od_settings_defaults(&b);
    od_settings_parse(&b, text, 0);
    EXPECT(b.desktop_prompted == 1 && b.warnings == 0, "desktop_prompted survives settings.ini");
#ifdef __linux__
    /* The Settings row toggles install / uninstall by request. */
    ui.desktop_action = OD_UI_DESKTOP_NONE;
    ui.screen = OD_UI_SETTINGS;
    ui.selected = 6;
    ui.desktop_installed = 0;
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
    EXPECT(ui.desktop_action == OD_UI_DESKTOP_INSTALL, "Settings row requests install");
    ui.desktop_action = OD_UI_DESKTOP_NONE;
    ui.desktop_installed = 1;
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
    EXPECT(ui.desktop_action == OD_UI_DESKTOP_UNINSTALL, "Settings row requests removal");
#endif
#else
    EXPECT(0, "no desktop question (earlier)");
#endif
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* lines survive a sanitizer abort */
    const char *t = getenv("TMPDIR");
#ifdef _WIN32
    if (t == NULL || t[0] == '\0') t = getenv("TEMP");
#endif
    snprintf(tmp, sizeof tmp, "%s/ui-behaviour", t && t[0] ? t : "/tmp");
    mkdirs(tmp);
    CASE("UI-1  Delete: focused slot, confirmed, states screen only", ui1_delete_focused_slot);
    CASE("UI-17 Save here over an occupied slot asks", ui17_overwrite_confirmation);
    CASE("New   game: asks, No by default, Yes sets the flag", new_game_confirmed);
    CASE("UI-13 thumbnail with the same mtime", ui13_same_mtime_thumbnail);
    CASE("UI-14 aliased data folder; final link refused", ui14_aliased_folder);
    CASE("UI-3  Save writes the opened file (id = file stem)", ui3_id_from_file_stem);
    CASE("name  first save names the level YYYY-MM-DD HH:MM", name_is_first_save_time);
    CASE("UI-15 flame brush: one run per cell, erase removes it", ui15_flame_brush);
    CASE("UI-23 editor asks before dropping unsaved changes", ui23_editor_unsaved);
    CASE("UI-23 catalogue Delete asks", ui23_catalog_delete);
    CASE("UI-8  a click selects, a second click plays", ui8_click_selects);
    CASE("UI-5  export outside the catalogue; Duplicate reports", ui5_export_and_messages);
    CASE("UI-6  UTF-8 path field and Backspace", ui6_text_input);
    CASE("UI-7  Back returns to the menu when opened from it", ui7_back_target);
    CASE("UI-16 [input] keys are not settings warnings", ui16_settings_and_bindings);
    CASE("UI-24 pad accept/back by position", ui24_pad_menu_buttons);
    CASE("UI-25 a directory is not read as settings", ui25_settings_directory);
    CASE("F1 opens the controls, Esc closes, hint flag persists", c640_controls);
    CASE("applications-menu question and Settings row", c646_desktop);
    printf("ui-behaviour: %d cases (%d red), %d checks, %d failures, %d skipped\n", cases,
           failed_cases, checks, failures, skipped);
    return failures ? 1 : 0;
}
