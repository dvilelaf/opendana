/* ui-screens-layout.c - layout gate of the 256x240 overlay screens.
 *
 * The menu, the settings, the states screen (up to 16 slots, its status and
 * its confirmation), the toasts and the first-run screen, plus the editor's
 * and the catalogue's confirmation dialogs. Like editor-ui-layout.c it is
 * built with -Wl,--wrap=od_ui_font_text and records every text: each one must
 * stay inside the canvas, not overlap another text, and remain visible on the
 * final canvas; every slot must be reachable and drawn; dialogs keep their
 * labels inside their buttons. The overlay is scaled with the game frame
 * (od_video_viewport), a whole multiple at window scales 2-4 and with the
 * 8:7 widening applied to both, so containment in the canvas is containment
 * on screen.
 *
 * Positive control: built against the earlier sources (OD_UI_SRC) it must
 * fail (texts leaving the screen, slots 9-16 off the panel). */
#include "opendana/ui.h"
#include "opendana/ui_import.h"
#include "opendana/ui_font.h"
#include "opendana/ui_strings.h"
#include "opendana/ext/video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ui.c presents through SDL; this gate only draws, so the few SDL calls it
 * links are stubs (no SDL library, no window). */
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

static int checks, failures;
#define EXPECT(cond, ...)                                                     \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

typedef struct { int x, y, w, h, scale; uint32_t argb; char text[200]; } run;
static run runs[1024];
static int nruns;

int __real_od_ui_font_text(uint32_t *canvas, int cw, int ch, int x, int y,
                            const char *text, uint32_t argb, int scale);
int __wrap_od_ui_font_text(uint32_t *canvas, int cw, int ch, int x, int y,
                            const char *text, uint32_t argb, int scale)
{
    if (text != NULL && text[0] != '\0' && strspn(text, " ") != strlen(text) &&
        nruns < (int)(sizeof runs / sizeof runs[0])) {
        run *r = &runs[nruns++];
        r->x = x;
        r->y = y;
        r->w = od_ui_font_width(text, scale);
        r->h = OD_UI_FONT_H * scale;
        r->scale = scale;
        r->argb = argb;
        snprintf(r->text, sizeof r->text, "%s", text);
    }
    return __real_od_ui_font_text(canvas, cw, ch, x, y, text, argb, scale);
}

static int overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static int inside(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax >= bx && ay >= by && ax + aw <= bx + bw && ay + ah <= by + bh;
}

/* Inside the canvas, apart, and still visible (alpha-opaque glyphs). Runs
 * from `first` on are checked; earlier ones may lie under a dialog. */
static void check_runs(const char *screen, const uint32_t *final, int cw, int ch,
                       int first)
{
    static uint32_t probe[256 * 240];
    for (int i = first; i < nruns; i++) {
        const run *r = &runs[i];
        EXPECT(inside(r->x, r->y, r->w, r->h, 0, 0, cw, ch),
               "%s: text \"%s\" at %d,%d %dx%d leaves the %dx%d screen",
               screen, r->text, r->x, r->y, r->w, r->h, cw, ch);
        for (int j = i + 1; j < nruns; j++) {
            const run *s = &runs[j];
            EXPECT(!overlap(r->x, r->y, r->w, r->h, s->x, s->y, s->w, s->h),
                   "%s: text \"%s\" at %d,%d overlaps \"%s\" at %d,%d", screen,
                   r->text, r->x, r->y, s->text, s->x, s->y);
        }
        if ((r->argb >> 24) != 0xFFu)
            continue;
        memset(probe, 0, sizeof probe);
        __real_od_ui_font_text(probe, cw, ch, r->x, r->y, r->text, r->argb, r->scale);
        int ink = 0, hidden = 0;
        for (int p = 0; p < cw * ch; p++)
            if (probe[p] != 0) {
                ink++;
                hidden += final[p] != probe[p];
            }
        EXPECT(hidden == 0, "%s: text \"%s\" at %d,%d is hidden (%d of %d glyph "
                            "pixels overdrawn)", screen, r->text, r->x, r->y,
               hidden, ink);
    }
}

static int find_run(const char *prefix)
{
    for (int i = 0; i < nruns; i++)
        if (strncmp(runs[i].text, prefix, strlen(prefix)) == 0)
            return i;
    return -1;
}

static od_settings settings;
static od_ui ui;

static void draw_ui(const char *screen, int first)
{
    nruns = 0;
    od_ui_draw(&ui);
    check_runs(screen, ui.canvas, OD_UI_SCREEN_W, OD_UI_SCREEN_H, first);
}

static void test_menu_settings(void)
{
#if defined(OD_UI_REVISION) && OD_UI_REVISION >= 632
    /* The New game question over the menu. */
    ui.screen = OD_UI_MENU;
    ui.selected = 6;                         /* New game (Controls is row 6) */
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
    EXPECT(ui.confirm == OD_UI_CONFIRM_NEW_GAME, "New game asks");
    nruns = 0;
    od_ui_draw(&ui);
    int first = find_run("Start a new game?");
    EXPECT(first >= 0, "the New game question is drawn");
    if (first >= 0)
        check_runs("menu, New game question", ui.canvas, OD_UI_SCREEN_W,
                   OD_UI_SCREEN_H, first);
    od_ui_handle_key(&ui, OD_UI_KEY_BACK);
#endif
#if defined(OD_UI_REVISION) && OD_UI_REVISION >= 646
    /* the applications-menu question (two-line question) over the menu,
     * in full: no run is cut with "...", the whole sentence is drawn. */
    ui.screen = OD_UI_MENU;
    ui.confirm = OD_UI_CONFIRM_DESKTOP;
    ui.confirm_yes = 0;
    nruns = 0;
    od_ui_draw(&ui);
    int q = find_run("Add OpenDana to the");
    EXPECT(q >= 0, "the applications-menu question is drawn");
    if (q >= 0) {
        check_runs("menu, applications-menu question", ui.canvas, OD_UI_SCREEN_W,
                   OD_UI_SCREEN_H, q);
        char joined[512] = "";
        for (int r = 0; r < nruns; r++) {
            EXPECT(!strstr(runs[r].text, "..."), "a text is cut: \"%s\"", runs[r].text);
            strncat(joined, runs[r].text, sizeof joined - strlen(joined) - 2);
            strcat(joined, " ");
        }
        EXPECT(strstr(joined, "Add OpenDana to the applications menu?") != NULL ||
               (strstr(joined, "Add OpenDana to the") && strstr(joined, "applications menu?")),
               "the whole question is drawn: %s", joined);
        EXPECT(strstr(joined, "Undo in Settings.") != NULL ||
               strstr(joined, "Settings.") != NULL, "the detail is drawn: %s", joined);
    }
    ui.confirm = OD_UI_CONFIRM_NONE;
#endif
    for (int i = 0; i < 8; i++) {
        ui.screen = OD_UI_MENU;
        ui.selected = i;
        char name[32];
        snprintf(name, sizeof name, "menu row %d", i);
        draw_ui(name, 0);
    }
#if defined(OD_UI_REVISION) && OD_UI_REVISION >= 646 && defined(__linux__)
    const int settings_rows = 8;             /* Applications menu row */
#else
    const int settings_rows = 7;
#endif
    for (int i = 0; i < settings_rows; i++) {
        ui.desktop_installed = i & 1;         /* both values get drawn */
        ui.screen = OD_UI_SETTINGS;
        ui.selected = i;
        char name[32];
        snprintf(name, sizeof name, "settings row %d", i);
        draw_ui(name, 0);
    }
}

/* the controls screen (from F1 in the game and from the menu entry)
 * and the one-time hint toast. Every text inside the canvas, apart, visible;
 * every key of the table and the close hint drawn in full. The canvas is
 * scaled with the game viewport, so containment holds at scales 2-4. */
static void test_controls(void)
{
#if defined(OD_UI_REVISION) && OD_UI_REVISION >= 640
    static const char *const full[] = {
        "CONTROLS", "GAME", "EDITOR", "Arrows", "Move, jump", "Z, Space",
        "A (wand)", "X", "B (wand)", "Enter", "START", "Right Shift", "SELECT",
        "Esc / Guide", "Menu", "M", "Mute", "F5 / F9", "Save/load state",
        "F6 / F7", "Prev/next slot", "F12", "Screenshot", "F2", "Console RESET",
        "F11", "Fullscreen", "F1", "This screen", "Ctrl+Z / Y",
        "Undo / redo", "Ctrl+S", "Save level", "F5", "Test level",
        "L/R click", "Paint / erase", "Mouse wheel", "Change material",
        "Middle click", "Pick material", "Esc or B: close",
    };
    for (int from = 0; from < 2; from++) {
        ui.screen = from ? OD_UI_MENU : OD_UI_HIDDEN;
        ui.toast_ms = 0;
        od_ui_handle_key(&ui, OD_UI_KEY_CONTROLS);
        EXPECT(ui.screen == OD_UI_CONTROLS, "F1 opens the controls");
        draw_ui(from ? "controls from the menu" : "controls from the game", 0);
        for (size_t i = 0; i < sizeof full / sizeof full[0]; i++) {
            int at = -1;      /* an exact run: a cut one ends in "..." */
            for (int r = 0; r < nruns && at < 0; r++)
                if (strcmp(runs[r].text, full[i]) == 0)
                    at = r;
            EXPECT(at >= 0, "controls: \"%s\" is not drawn in full", full[i]);
        }
        od_ui_handle_key(&ui, OD_UI_KEY_BACK);
    }
    /* With a toast over it (the hint is shown at the start of a game). */
    ui.screen = OD_UI_CONTROLS;
    od_ui_toast(&ui, od_str(OD_STR_CONTROLS_HINT_ONCE), 4000);
    draw_ui("controls with a toast", 0);
    /* The hint over the game, and over the menu. */
    EXPECT(strcmp(od_str(OD_STR_CONTROLS_HINT_ONCE), "Esc: menu  |  F1: controls") == 0,
           "the hint text");
    ui.screen = OD_UI_HIDDEN;
    od_ui_toast(&ui, od_str(OD_STR_CONTROLS_HINT_ONCE), 4000);
    draw_ui("controls hint over the game", 0);
    EXPECT(find_run("Esc: menu  |  F1: controls") >= 0 &&
           strcmp(runs[find_run("Esc: menu")].text, "Esc: menu  |  F1: controls") == 0,
           "the hint is drawn in full");
    ui.toast_ms = 0;
    ui.screen = OD_UI_HIDDEN;
#else
    EXPECT(0, "no controls screen (earlier)");
#endif
}

static void test_states(void)
{
    settings.states_slots = 16;
    ui.screen = OD_UI_MENU;
    ui.selected = 1;                         /* States */
    od_ui_handle_key(&ui, OD_UI_KEY_ACCEPT);
    EXPECT(ui.screen == OD_UI_STATES, "the menu opens the states screen");
    snprintf(ui.state_status, sizeof ui.state_status, "%s",
             "Slot 16: Saved with different imported content");
    for (int slot = 0; slot < 16; slot++) {
        ui.slot_focus = 0;
        for (int k = 0; k < slot; k++)
            od_ui_handle_key(&ui, OD_UI_KEY_DOWN);
        EXPECT(ui.slot_focus == slot, "keyboard reaches slot %d (at %d)", slot + 1,
               ui.slot_focus + 1);
        char name[40];
        snprintf(name, sizeof name, "states, slot %d focused", slot + 1);
        draw_ui(name, 0);
        char label[8];
        snprintf(label, sizeof label, "%d ", slot + 1);
        EXPECT(find_run(label) >= 0, "states: slot %d is not drawn when focused",
               slot + 1);
        /* Fixed labels are drawn in full, never cut with "...". */
        static const char *const buttons[] = { "Save here", "Delete", "Back" };
        for (int k = 0; k < 3; k++) {
            int at = find_run(buttons[k]);
            EXPECT(at >= 0 && strcmp(runs[at].text, buttons[k]) == 0,
                   "states: button \"%s\" is not drawn in full", buttons[k]);
        }
    }
#ifdef OD_UI_STATES_VISIBLE
    /* Delete and overwrite ask first: the dialog fits and is legible. */
    ui.slot_occupied[3] = 1;
    ui.slot_focus = 3;
    ui.slot_scroll = 0;
    od_ui_handle_key(&ui, OD_UI_KEY_DELETE);
    EXPECT(ui.confirm == OD_UI_CONFIRM_DELETE, "Delete asks first");
    nruns = 0;
    od_ui_draw(&ui);
    int first = find_run("Delete slot");
    EXPECT(first >= 0, "the delete question is drawn");
    if (first >= 0)
        check_runs("states, delete confirmation", ui.canvas, OD_UI_SCREEN_W,
                   OD_UI_SCREEN_H, first);
    od_ui_handle_key(&ui, OD_UI_KEY_BACK);
#else
    EXPECT(0, "no confirmation before deleting a slot (earlier)");
#endif
    settings.states_slots = 8;
    ui.screen = OD_UI_HIDDEN;
}

static void test_toasts(void)
{
    static const char *const toasts[] = {
        "Muted", "Slot 16", "Screenshot saved",
        "Save/load unavailable: content identity is not verified",
        "This state is from an incompatible game version.",
        "This state was saved with different imported content.",
        /* C6.98: the state messages name their slot (two digits, longest forms). */
        "State saved to slot 16", "State loaded from slot 16", "State removed from slot 16",
        "Slot 16: Saved by an older OpenDana version",
        "Slot 16: Saved with different imported content", "Slot 16: Could not save the state",
        "A toast long enough to need two lines on the 256 pixel overlay x",
#if defined(OD_UI_REVISION) && OD_UI_REVISION >= 646
        "Added to the applications menu", "Removed from the applications menu",
        "! Another launcher exists; left alone", "! Package icons not found",
        "! Could not change the menu entry",
#endif
    };
    for (size_t i = 0; i < sizeof toasts / sizeof toasts[0]; i++) {
        ui.screen = OD_UI_HIDDEN;
        od_ui_toast(&ui, toasts[i], 1000);
        draw_ui(toasts[i], 0);
        ui.screen = OD_UI_MENU;
        draw_ui(toasts[i], 0);
    }
    ui.toast_ms = 0;
    ui.screen = OD_UI_HIDDEN;
}

static void test_first_run(void)
{
    static od_ui_import st;
    static uint32_t canvas[256 * 240];
    static const char *const statuses[] = {
        "No content. Import your ROM.",
        "the file is not the supported one: SHA-256 0123456789abcdef "
        "0123456789abcdef0123456789abcdef0123456789abcdef does not match",
    };
    for (size_t i = 0; i < sizeof statuses / sizeof statuses[0]; i++) {
        od_ui_import_init(&st, "/tmp/content");
        od_ui_import_set_path(&st, "/home/someone/Games/Console/Roms/europe/"
                                    "Sample Game Title (Europe).rom");
        snprintf(st.status, sizeof st.status, "%s", statuses[i]);
        st.status_error = 1;
        nruns = 0;
        od_ui_import_draw(&st, canvas);
        check_runs(i ? "first run, long error" : "first run", canvas, 256, 240, 0);
        int at = find_run("OpenDana");
        EXPECT(at >= 0 && strcmp(runs[at].text, "OpenDana") == 0,
               "first run: the title is not drawn in full (\"%s\")",
               at >= 0 ? runs[at].text : "");
        at = find_run("Open ROM");
        EXPECT(at >= 0 && strcmp(runs[at].text, "Open ROM...") == 0,
               "first run: the button label is not drawn in full");
    }
#ifdef OD_UI_IMPORT_STALE
    /* the "content from another version" variant, with its own status
     * and with a long import error after a failed attempt. Every text inside
     * the canvas (so on screen at scales 2-4), apart and visible; the
     * headline, the three body lines and the hints are drawn in full. */
    for (int k = 0; k < 2; k++) {
        od_ui_import_init(&st, "/tmp/content");
        od_ui_import_set_stale(&st, 1);
        if (k) {
            od_ui_import_set_path(&st, "/home/someone/Games/Console/Roms/"
                                        "europe/Sample Game Title (Europe).rom");
            od_ui_import_set_status(&st, statuses[1], 1);
        }
        nruns = 0;
        od_ui_import_draw(&st, canvas);
        check_runs(k ? "stale content, long error" : "stale content", canvas, 256,
                   240, 0);
        static const char *const full[] = {
            "Content update needed", "Import your ROM again. Your",
            "current files are kept until", "the new import succeeds.",
            "Click Open ROM... below,", "or drop the ROM here.", "Open ROM...",
        };
        for (size_t i = 0; i < sizeof full / sizeof full[0]; i++) {
            int at = find_run(full[i]);
            EXPECT(at >= 0 && strcmp(runs[at].text, full[i]) == 0,
                   "stale content: \"%s\" is not drawn in full", full[i]);
        }
        EXPECT(find_run("First run setup") < 0 && find_run("To play you need") < 0,
               "stale content: the first-run text is still drawn");
        if (!k)
            EXPECT(find_run("Your imported content is from") >= 0 &&
                   find_run("another version.") >= 0,
                   "stale content: the status does not say why");
    }
#endif
}

static void test_scales(void)
{
    /* The overlay shares the game viewport: at scales 2-4 (and 8:7, with the
     * window widened by the renderer) it is a whole multiple of 256x240. */
    for (int k = 2; k <= 4; k++)
        for (int aspect = 0; aspect <= 1; aspect++) {
            od_video_rect r;
            od_video_viewport(od_video_scaled_width(k, aspect), 240 * k, 1,
                               aspect, &r);
            EXPECT(r.h == 240 * k && r.x >= 0 && r.y >= 0,
                   "overlay at scale %d (8:7 %d): %dx%d", k, aspect, r.w, r.h);
        }
}

int main(void)
{
    od_settings_defaults(&settings);
    od_ui_init(&ui, &settings);
    test_menu_settings();
    test_controls();
    test_states();
    test_toasts();
    test_first_run();
    test_scales();
    printf("ui-screens-layout: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
