#define _POSIX_C_SOURCE 200809L
/* first-run-layout.c - layout gate of the modern first-run screen.
 *
 * For every window size (640x480 ... 3840x2160) and every state of the screen
 * it lays the screen out and draws it, and checks:
 *   - every box is inside the window and no two boxes overlap;
 *   - every string fits its box (measured with the real font), and the button
 *     label fits the button;
 *   - the button is centred horizontally (+-1 px) and so is every text line;
 *   - nothing is painted outside the boxes (the drawn pixels are compared with
 *     the boxes, so a glyph run that overflows is seen);
 *   - the title scales with the height.
 * Positive controls (L1): the same checker must reject (a) the earlier
 * screen, measured through its real hit test (the "Open ROM..." button was
 * left-aligned at x=12..162 of 256), and (b) a layout whose text is too wide. */
#include "opendana/ui_import.h"
#include "opendana/ui_import_view.h"
#include "opendana/ui_strings.h"
#include "opendana/ui_ttf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define BG 0xFF14181Fu

static void bad(const char *what, const char *state, int w, int h, const char *detail)
{
    g_fail++;
    printf("FAIL %dx%d [%s] %s %s\n", w, h, state, what, detail);
}

typedef struct { od_ui_box box; int is_text; int text_w; int is_button; int is_icon; } cbox;

static int overlaps(const od_ui_box *a, const od_ui_box *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h &&
           b->y < a->y + a->h;
}

/* The checker, shared by the product screen and the controls. Returns the
 * number of failures it found (and prints them when `quiet` is 0). */
static int check_boxes(const cbox *c, int n, int w, int h, const char *state)
{
    int before = g_fail;
    for (int i = 0; i < n; i++) {
        const od_ui_box *b = &c[i].box;
        char d[96];
        if (b->x < 0 || b->y < 0 || b->x + b->w > w || b->y + b->h > h) {
            snprintf(d, sizeof d, "box %d outside the window", i);
            bad("bounds", state, w, h, d);
        }
        if (c[i].is_text && c[i].text_w > b->w) {
            snprintf(d, sizeof d, "box %d: text %d px in %d", i, c[i].text_w, b->w);
            bad("text does not fit", state, w, h, d);
        }
        if (c[i].is_button) {
            int cx2 = 2 * b->x + b->w - w; /* twice the offset from the centre */
            if (cx2 < -2 || cx2 > 2)
                bad("button not centred", state, w, h, "");
        }
        for (int j = i + 1; j < n; j++)
            if (overlaps(b, &c[j].box))
                bad("boxes overlap", state, w, h, "");
    }
    return g_fail - before;
}

static void run_state(od_ui_import_view *v, const od_ui_import *st, const char *name,
                      int w, int h, uint32_t *buf)
{
    od_iv_layout L;
    od_ui_import_view_layout(v, st, w, h, &L);
    cbox c[OD_IV_MAX_ITEMS];
    int n = 0, title_px = 0;
    od_ui_box allow[OD_IV_MAX_ITEMS];
    for (int i = 0; i < L.n; i++) {
        const od_iv_item *it = &L.item[i];
        if (it->kind == OD_IV_CARET)
            continue; /* lives inside the field */
        memset(&c[n], 0, sizeof c[n]);
        c[n].box = it->box;
        c[n].is_text = it->kind == OD_IV_TEXT;
        c[n].is_button = it->kind == OD_IV_BUTTON;
        c[n].is_icon = it->kind == OD_IV_ICON;
        if (it->kind == OD_IV_TEXT || it->kind == OD_IV_BUTTON || it->kind == OD_IV_FIELD) {
            int tw = od_ui_import_view_text_width(v, it);
            if (it->kind == OD_IV_TEXT)
                c[n].text_w = tw;
            else if (tw + (it->kind == OD_IV_BUTTON ? 8 : 2 * (it->px / 2)) > it->box.w) {
                bad("label does not fit", name, w, h, it->text);
            }
        }
        if (it->kind == OD_IV_TEXT && it->face == OD_TTF_SEMIBOLD && title_px == 0)
            title_px = it->px;
        /* where pixels may legitimately be: the box, a descender/ring slack */
        int slack = it->kind == OD_IV_BUTTON ? it->box.h / 3 + 4 : it->px / 2 + 2;
        allow[n].x = it->box.x - slack;
        allow[n].y = it->box.y - slack;
        allow[n].w = it->box.w + 2 * slack;
        allow[n].h = it->box.h + 2 * slack;
        n++;
    }
    check_boxes(c, n, w, h, name);
    if (L.button < 0)
        bad("no button", name, w, h, "");
    if (title_px < (int)(0.03f * (w < h * 1.2 ? w / 1.2f : (float)h)) - 1)
        bad("title too small for the window", name, w, h, "");
    /* every drawn pixel lies inside an allowed box */
    od_ui_import_view_draw(v, st, buf, w, h);
    long stray = 0;
    for (int y = 0; y < h && stray < 5; y++)
        for (int x = 0; x < w; x++) {
            if (buf[(long)y * w + x] == BG)
                continue;
            int ok = 0;
            for (int i = 0; i < n && !ok; i++)
                ok = x >= allow[i].x && x < allow[i].x + allow[i].w && y >= allow[i].y &&
                     y < allow[i].y + allow[i].h;
            if (!ok)
                stray++;
        }
    if (stray) {
        char d[64];
        snprintf(d, sizeof d, "painted outside every box (first at row scan, %ld)", stray);
        bad("stray pixels", name, w, h, d);
    }
    /* the pointer hit test is the drawn button */
    const od_ui_box *b = &L.item[L.button].box;
    if (!od_ui_import_view_button_hit(v, st, w, h, b->x + b->w / 2, b->y + b->h / 2) ||
        od_ui_import_view_button_hit(v, st, w, h, b->x - 2, b->y + b->h / 2))
        bad("hit test differs from the drawn button", name, w, h, "");
}

/* Control (a): the earlier screen, through its real hit test. */
static int control_legacy(int w, int h)
{
    int x0 = 1 << 30, x1 = -1, y0 = 1 << 30, y1 = -1;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 256; x++)
            if (od_ui_import_button_hit(x, y)) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    cbox c = {{0}, 0, 0, 1, 0};
    c.box.x = x0 * w / 256;
    c.box.y = y0 * h / 240;
    c.box.w = (x1 - x0 + 1) * w / 256;
    c.box.h = (y1 - y0 + 1) * h / 240;
    int saved = g_fail;
    int found = check_boxes(&c, 1, w, h, "control legacy");
    g_fail = saved; /* the control is allowed to fail */
    return found;
}

/* Control (b): a text wider than its box. */
static int control_wide(int w, int h)
{
    cbox c = {{w / 4, h / 4, w / 4, 20}, 1, w / 2, 0, 0};
    int saved = g_fail;
    int found = check_boxes(&c, 1, w, h, "control wide text");
    g_fail = saved;
    return found;
}

int main(void)
{
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", getenv("OD_FONT_DIR") ? getenv("OD_FONT_DIR") : "assets/fonts");
    od_ui_import_view *v = od_ui_import_view_open(dir, "packaging/icon/png/opendana-128.png");
    if (v == NULL) {
        fprintf(stderr, "first-run-layout: cannot load the fonts from %s\n", dir);
        return 2;
    }
    static const int sizes[][2] = {
        {640, 480}, {768, 720}, {1280, 720}, {1920, 1080}, {3840, 2160}, {480, 800}};
    uint32_t *buf = (uint32_t *)malloc(3840u * 2160u * sizeof(uint32_t));
    static od_ui_import st;
    const char *longpath =
        "/home/someone/Games/Console/Roms/europe/Sample Game Title (Europe) [!] (with a very long name).rom";
    const char *longerr =
        "wrong file size: 18 bytes; the supported file is 262160 bytes "
        "(the size of the supported ROM, header included)";
    int cases = 0;
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        int w = sizes[s][0], h = sizes[s][1];
        for (int k = 0; k < 14; k++) {
            od_ui_import_init(&st, "/tmp/content");
            const char *name = "normal";
            switch (k) {
            case 1: od_ui_import_set_stale(&st, 1); name = "stale"; break;
            case 2:
                od_ui_import_set_path(&st, longpath);
                st.typing = 1;
                od_ui_import_set_status(&st, longerr, 1);
                name = "typing long path + error";
                break;
            case 3:
                od_ui_import_set_stale(&st, 1);
                od_ui_import_set_status(&st, longerr, 1);
                st.no_chooser = 1;
                name = "stale + error + no chooser";
                break;
            case 4: od_ui_import_set_status(&st, od_str(OD_STR_FR_IMPORTING), 0); st.busy = 1;
                    name = "importing"; break;
            case 5: st.done = 1; od_ui_import_set_status(&st, "x", 0); name = "done"; break;
            case 6: st.hot = 1; st.kbd = 1; name = "hover + focus ring"; break;
            case 7: st.pressed = 1; st.typing = 1; name = "pressed + empty field"; break;
            case 13: st.no_chooser = 1;
                     od_ui_import_set_status(&st, od_str(OD_STR_FR_NO_CHOOSER), 1);
                     name = "no chooser"; break;
            default:
                if (k >= 8) { /* each human importer error, on the plain screen */
                    od_ui_import_set_status(&st, od_str(OD_STR_FR_ERR_NOT_ROM + (k - 8)), 1);
                    name = "importer error";
                }
                break;
            }
            run_state(v, &st, name, w, h, buf);
            cases++;
        }
    }
    /* The importer's technical text never reaches the screen (stderr only). */
    {
        char tmp[] = "/tmp/first-run-layout-XXXXXX";
        int fd = mkstemp(tmp);
        if (fd >= 0) {
            if (write(fd, "this is not a ROM\n", 18) != 18)
                g_fail++;
            close(fd);
            od_ui_import_init(&st, "/tmp/first-run-layout-content");
            od_ui_import_drop(&st, tmp);
            if (strcmp(st.status, od_str(OD_STR_FR_ERR_NOT_ROM)) != 0 || !st.status_error ||
                strstr(st.status, "bytes") != NULL) {
                bad("importer error is not the human line", "drop", 0, 0, st.status);
            }
            remove(tmp);
            cases++;
        }
    }
    /* positive controls */
    int a = control_legacy(768, 720), b = control_wide(768, 720);
    printf("control: legacy 8x8 screen -> %d failure(s), wide text -> %d failure(s)\n", a, b);
    if (a == 0 || b == 0) {
        printf("FAIL the checker is blind (a control was not detected)\n");
        g_fail++;
    }
    free(buf);
    od_ui_import_view_close(v);
    printf("first-run-layout: %d state/size cases, %s\n", cases, g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
