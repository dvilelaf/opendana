/* ui_import_view.c - native-resolution first-run screen (C6.48). See the
 * header. */
#include "opendana/ui_import_view.h"

#include "opendana/ui_strings.h"
#include "opendana/ui_ttf.h"
#include "opendana/platform.h"
#include "../import/fsutil.h"

#include "stb_image.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_BG 0xFF14181Fu
#define C_TEXT 0xFFE9ECF1u
#define C_DIM 0xFF8A93A0u
#define C_PANEL 0xFF1E242Eu
#define C_BTN 0xFF3D7BEAu
#define C_BTN_HOT 0xFF5791F3u
#define C_BTN_DOWN 0xFF2E64C6u
#define C_BTN_BUSY 0xFF3A4660u
#define C_RING 0xFFA9C8FFu
#define C_ERROR 0xFFFF7A7Au
#define C_OK 0xFF7FD99Au


struct od_ui_import_view {
    od_ttf *font;
    unsigned char *icon;     /* RGBA, icon_w x icon_h                          */
    int icon_w, icon_h;
    uint32_t *scaled;        /* premultiplied ARGB at scaled_s x scaled_s     */
    int scaled_s;
};

static unsigned char *load_png(const char *path, int *w, int *h)
{
    FILE *fp = od_fs_fopen(path, "rb");
    if (fp == NULL)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *raw = n > 0 && n < (8L << 20) ? (unsigned char *)malloc((size_t)n) : NULL;
    if (raw != NULL && fread(raw, 1, (size_t)n, fp) != (size_t)n) {
        free(raw);
        raw = NULL;
    }
    fclose(fp);
    if (raw == NULL)
        return NULL;
    int comp = 0;
    unsigned char *px = stbi_load_from_memory(raw, (int)n, w, h, &comp, 4);
    free(raw);
    return px;
}

static int find_icon(char *out, size_t cap)
{
    char exe[1100];
    if (!od_platform_executable_dir(exe, sizeof exe))
        return 0;
    static const char *REL[] = {"/../share/opendana/ui/opendana-128.png",
                                "/../packaging/icon/png/opendana-128.png",
                                "/packaging/icon/png/opendana-128.png"};
    for (size_t i = 0; i < sizeof REL / sizeof REL[0]; i++) {
        snprintf(out, cap, "%s%s", exe, REL[i]);
        FILE *fp = od_fs_fopen(out, "rb");
        if (fp != NULL) {
            fclose(fp);
            return 1;
        }
    }
    return 0;
}

od_ui_import_view *od_ui_import_view_open(const char *font_dir, const char *icon_path)
{
    char dir[1100], ico[1200];
    if (font_dir == NULL) {
        if (!od_ttf_find_dir(dir, sizeof dir)) {
            fprintf(stderr, "first run: Inter fonts not found, using the basic screen\n");
            return NULL;
        }
        font_dir = dir;
    }
    od_ttf *font = od_ttf_open(font_dir);
    if (font == NULL) {
        fprintf(stderr, "first run: cannot load the fonts from %s, using the basic screen\n",
                font_dir);
        return NULL;
    }
    od_ui_import_view *v = (od_ui_import_view *)calloc(1, sizeof *v);
    if (v == NULL) {
        od_ttf_close(font);
        return NULL;
    }
    v->font = font;
    if (icon_path == NULL && find_icon(ico, sizeof ico))
        icon_path = ico;
    if (icon_path != NULL)
        v->icon = load_png(icon_path, &v->icon_w, &v->icon_h);
    if (v->icon == NULL)
        fprintf(stderr, "first run: icon not found, drawing the screen without it\n");
    return v;
}

void od_ui_import_view_close(od_ui_import_view *v)
{
    if (v == NULL)
        return;
    od_ttf_close(v->font);
    if (v->icon != NULL)
        stbi_image_free(v->icon);
    free(v->scaled);
    free(v);
}

static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }

static od_iv_item *add(od_iv_layout *L, int kind, int x, int y, int w, int h,
                       const char *text, int face, int px, uint32_t color)
{
    if (L->n >= OD_IV_MAX_ITEMS)
        return NULL;
    od_iv_item *it = &L->item[L->n++];
    memset(it, 0, sizeof *it);
    it->kind = kind;
    it->box.x = x;
    it->box.y = y;
    it->box.w = w;
    it->box.h = h;
    if (text != NULL)
        snprintf(it->text, sizeof it->text, "%.1090s", text);
    it->face = face;
    it->px = px;
    it->color = color;
    return it;
}

/* Wrapped, centred lines of `text`; returns the height used. */
static int add_lines(od_ui_import_view *v, od_iv_layout *L, int x, int y, int w,
                     const char *text, int face, int px, uint32_t color, int max_lines)
{
    char lines[4][300];
    if (max_lines > 4)
        max_lines = 4;
    int n = od_ttf_wrap(v->font, face, px, text, w, &lines[0][0], sizeof lines[0], max_lines);
    int lh = od_ttf_line_height(v->font, face, px);
    for (int i = 0; i < n; i++)
        add(L, OD_IV_TEXT, x, y + i * lh, w, lh, lines[i], face, px, color);
    return n * lh;
}

void od_ui_import_view_layout(od_ui_import_view *v, const od_ui_import *st,
                              int w, int h, od_iv_layout *L)
{
    memset(L, 0, sizeof *L);
    L->w = w;
    L->h = h;
    L->button = -1;
    float u = fminf((float)h, (float)w * 1.2f);
    int title_px = imax(16, (int)lroundf(0.045f * u));
    int body_px = imax(12, (int)lroundf(0.027f * u));
    int small_px = imax(11, (int)lroundf(0.022f * u));
    int colw = imin((int)(w * 0.88f), (int)(1.1f * u));
    int cx = w / 2;
    int colx = cx - colw / 2;
    int icon_s = v->icon != NULL ? imax(32, (int)lroundf(0.17f * u)) : 0;
    int g_icon = (int)lroundf(0.03f * u), g_title = (int)lroundf(0.012f * u),
        g_btn = (int)lroundf(0.05f * u), g_hint = (int)lroundf(0.02f * u);

    /* The block icon..hint is laid out from y = 0 and shifted afterwards. */
    int y = 0;
    if (icon_s > 0) {
        add(L, OD_IV_ICON, cx - icon_s / 2, y, icon_s, icon_s, NULL, 0, 0, 0);
        y += icon_s + g_icon;
    }
    int lh = od_ttf_line_height(v->font, OD_TTF_SEMIBOLD, title_px);
    add(L, OD_IV_TEXT, colx, y, colw, lh, od_str(OD_STR_FIRST_RUN_TITLE), OD_TTF_SEMIBOLD, title_px, C_TEXT);
    y += lh + g_title;
    y += add_lines(v, L, colx, y, colw, od_str(st->stale ? OD_STR_FR_SENTENCE_STALE : OD_STR_FR_SENTENCE),
                   OD_TTF_REGULAR, body_px, C_DIM, 3);
    y += g_btn;

    int bh = (int)lroundf(0.075f * u);
    int label_w = od_ttf_width(v->font, OD_TTF_SEMIBOLD, body_px, od_str(OD_STR_FR_BUTTON));
    int bw = imax((int)lroundf(0.3f * u), label_w + 2 * (int)lroundf(0.045f * u));
    bw = imin(bw, colw);
    od_iv_item *b = add(L, OD_IV_BUTTON, cx - bw / 2, y, bw, bh, od_str(OD_STR_FR_BUTTON), OD_TTF_SEMIBOLD,
                        body_px, 0xFFFFFFFFu);
    if (b != NULL)
        L->button = L->n - 1;
    y += bh + g_hint;
    int hlh = od_ttf_line_height(v->font, OD_TTF_REGULAR, small_px);
    add(L, OD_IV_TEXT, colx, y, colw, hlh, od_str(OD_STR_FR_HINT), OD_TTF_REGULAR, small_px, C_DIM);
    y += hlh;
    if (st->stale) {
        y += hlh / 3;
        y += add_lines(v, L, colx, y, colw, od_str(OD_STR_FR_NOTE_STALE), OD_TTF_REGULAR, small_px, C_DIM, 2);
    }
    int block = y;
    int reserve = (int)lroundf(0.09f * u);
    int top = (h - block - reserve) / 2;
    if (top < (int)(0.02f * h))
        top = (int)(0.02f * h);
    for (int i = 0; i < L->n; i++)
        L->item[i].box.y += top;

    /* Below the block, growing downwards so nothing above ever moves. */
    int by = top + block + (int)lroundf(0.035f * u);
    if (st->typing || st->no_chooser) {
        int fh = (int)lroundf(0.06f * u);
        const char *shown = st->path_len ? st->path : od_str(OD_STR_FR_PLACEHOLDER);
        uint32_t col = st->path_len ? C_TEXT : C_DIM;
        int pad = (int)lroundf(0.02f * u);
        int inner = colw - 2 * pad - (int)lroundf(0.01f * u);
        char vis[1100];
        snprintf(vis, sizeof vis, "%s", shown);
        if (od_ttf_width(v->font, OD_TTF_REGULAR, body_px, vis) > inner) {
            /* the tail of a long path, after an ellipsis */
            const char *p = shown;
            for (;;) {
                snprintf(vis, sizeof vis, "%s%s", od_str(OD_STR_FR_ELLIPSIS), p);
                if (*p == '\0' || od_ttf_width(v->font, OD_TTF_REGULAR, body_px, vis) <= inner)
                    break;
                p++;
                while (((unsigned char)*p & 0xC0) == 0x80)
                    p++;
            }
        }
        od_iv_item *f = add(L, OD_IV_FIELD, colx, by, colw, fh, vis, OD_TTF_REGULAR, body_px, col);
        if (f != NULL)
            f->left = 1;
        if (st->typing && st->path_len) {
            int tw = od_ttf_width(v->font, OD_TTF_REGULAR, body_px, vis);
            int cw = imax(2, body_px / 10);
            int ch = od_ttf_line_height(v->font, OD_TTF_REGULAR, body_px);
            add(L, OD_IV_CARET, colx + pad + tw + 2, by + (fh - ch) / 2, cw, ch, NULL, 0, 0,
                C_TEXT);
        }
        by += fh + (int)lroundf(0.02f * u);
    }
    if (!od_ui_import_status_idle(st)) {
        uint32_t col = st->status_error ? C_ERROR : (st->done ? C_OK : C_DIM);
        const char *msg = st->done ? od_str(OD_STR_FR_DONE) : st->status;
        add_lines(v, L, colx, by, colw, msg, OD_TTF_REGULAR, small_px, col, 3);
    }
}

int od_ui_import_view_text_width(od_ui_import_view *v, const od_iv_item *it)
{
    return od_ttf_width(v->font, it->face, it->px, it->text);
}

/* ------------------------------------------------------------------ draw */

static void blend(uint32_t *p, uint32_t argb, float a)
{
    if (a <= 0.f)
        return;
    if (a > 1.f)
        a = 1.f;
    uint32_t d = *p;
    float dr = (float)((d >> 16) & 255), dg = (float)((d >> 8) & 255), db = (float)(d & 255);
    float sr = (float)((argb >> 16) & 255), sg = (float)((argb >> 8) & 255),
          sb = (float)(argb & 255);
    uint32_t r = (uint32_t)(sr * a + dr * (1.f - a) + .5f);
    uint32_t g = (uint32_t)(sg * a + dg * (1.f - a) + .5f);
    uint32_t bl = (uint32_t)(sb * a + db * (1.f - a) + .5f);
    *p = 0xFF000000u | (r << 16) | (g << 8) | bl;
}

static void rrect(uint32_t *buf, int bw, int bh, float x, float y, float w, float h,
                  float r, uint32_t color)
{
    if (r > h / 2)
        r = h / 2;
    if (r > w / 2)
        r = w / 2;
    int x0 = (int)floorf(x) - 1, x1 = (int)ceilf(x + w) + 1;
    int y0 = (int)floorf(y) - 1, y1 = (int)ceilf(y + h) + 1;
    float cx = x + w / 2, cy = y + h / 2;
    for (int yy = imax(0, y0); yy < imin(bh, y1); yy++) {
        for (int xx = imax(0, x0); xx < imin(bw, x1); xx++) {
            float dx = fabsf((float)xx + .5f - cx) - (w / 2 - r);
            float dy = fabsf((float)yy + .5f - cy) - (h / 2 - r);
            float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(dx, dy), 0.f) - r;
            blend(&buf[yy * bw + xx], color, 0.5f - d);
        }
    }
}

static void build_icon(od_ui_import_view *v, int s)
{
    if (v->scaled_s == s && v->scaled != NULL)
        return;
    free(v->scaled);
    v->scaled = (uint32_t *)calloc((size_t)s * (size_t)s, sizeof(uint32_t));
    v->scaled_s = s;
    if (v->scaled == NULL)
        return;
    float k = (float)v->icon_w / (float)s;
    for (int y = 0; y < s; y++) {
        for (int x = 0; x < s; x++) {
            float ar = 0, ag = 0, ab = 0, aa = 0;
            for (int sy = 0; sy < 3; sy++) {
                for (int sx = 0; sx < 3; sx++) {
                    float fx = ((float)x + ((float)sx + .5f) / 3.f) * k - .5f;
                    float fy = ((float)y + ((float)sy + .5f) / 3.f) * k - .5f;
                    int ix = (int)floorf(fx), iy = (int)floorf(fy);
                    float tx = fx - (float)ix, ty = fy - (float)iy;
                    for (int j = 0; j < 2; j++) {
                        for (int i = 0; i < 2; i++) {
                            int px = imin(imax(ix + i, 0), v->icon_w - 1);
                            int py = imin(imax(iy + j, 0), v->icon_h - 1);
                            const unsigned char *q = v->icon + 4 * (py * v->icon_w + px);
                            float wgt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty) / 9.f;
                            float a = (float)q[3] / 255.f;
                            ar += wgt * a * q[0];
                            ag += wgt * a * q[1];
                            ab += wgt * a * q[2];
                            aa += wgt * a;
                        }
                    }
                }
            }
            /* premultiplied ARGB */
            v->scaled[y * s + x] = ((uint32_t)(aa * 255.f + .5f) << 24) |
                                   ((uint32_t)(ar + .5f) << 16) | ((uint32_t)(ag + .5f) << 8) |
                                   (uint32_t)(ab + .5f);
        }
    }
}

static void draw_icon(od_ui_import_view *v, uint32_t *buf, int bw, int bh, const od_ui_box *b)
{
    build_icon(v, b->w);
    if (v->scaled == NULL)
        return;
    for (int y = 0; y < b->w; y++) {
        int yy = b->y + y;
        if (yy < 0 || yy >= bh)
            continue;
        for (int x = 0; x < b->w; x++) {
            int xx = b->x + x;
            uint32_t s = v->scaled[y * b->w + x];
            uint32_t a = s >> 24;
            if (xx < 0 || xx >= bw || a == 0)
                continue;
            uint32_t d = buf[yy * bw + xx];
            uint32_t r = ((s >> 16) & 255) + (((d >> 16) & 255) * (255 - a) + 127) / 255;
            uint32_t g = ((s >> 8) & 255) + (((d >> 8) & 255) * (255 - a) + 127) / 255;
            uint32_t bl = (s & 255) + ((d & 255) * (255 - a) + 127) / 255;
            buf[yy * bw + xx] = 0xFF000000u | (imin((int)r, 255) << 16) |
                                (imin((int)g, 255) << 8) | (uint32_t)imin((int)bl, 255);
        }
    }
}

void od_ui_import_view_draw(od_ui_import_view *v, const od_ui_import *st,
                            uint32_t *buf, int w, int h)
{
    od_iv_layout L;
    od_ui_import_view_layout(v, st, w, h, &L);
    for (int i = 0; i < w * h; i++)
        buf[i] = C_BG;
    float u = fminf((float)h, (float)w * 1.2f);
    for (int i = 0; i < L.n; i++) {
        const od_iv_item *it = &L.item[i];
        const od_ui_box *b = &it->box;
        switch (it->kind) {
        case OD_IV_ICON:
            draw_icon(v, buf, w, h, b);
            break;
        case OD_IV_TEXT: {
            int tw = od_ttf_width(v->font, it->face, it->px, it->text);
            od_ttf_draw(v->font, it->face, it->px, buf, w, h, b->x + (b->w - tw) / 2,
                        b->y + od_ttf_ascent(v->font, it->face, it->px), it->text, it->color);
            break;
        }
        case OD_IV_BUTTON: {
            uint32_t fill = st->busy ? C_BTN_BUSY
                          : st->pressed ? C_BTN_DOWN
                          : st->hot ? C_BTN_HOT : C_BTN;
            float rad = (float)b->h * 0.3f;
            if (st->kbd && !st->busy) {
                float ring = fmaxf(2.f, 0.005f * u), gap = fmaxf(2.f, 0.004f * u);
                rrect(buf, w, h, b->x - gap - ring, b->y - gap - ring, b->w + 2 * (gap + ring),
                      b->h + 2 * (gap + ring), rad + gap + ring, C_RING);
                rrect(buf, w, h, b->x - gap, b->y - gap, b->w + 2 * gap, b->h + 2 * gap,
                      rad + gap, C_BG);
            }
            rrect(buf, w, h, (float)b->x, (float)b->y, (float)b->w, (float)b->h, rad, fill);
            int tw = od_ttf_width(v->font, it->face, it->px, it->text);
            int lh = od_ttf_line_height(v->font, it->face, it->px);
            int asc = od_ttf_ascent(v->font, it->face, it->px);
            od_ttf_draw(v->font, it->face, it->px, buf, w, h, b->x + (b->w - tw) / 2,
                        b->y + (b->h - lh) / 2 + asc, it->text, it->color);
            break;
        }
        case OD_IV_FIELD: {
            rrect(buf, w, h, (float)b->x, (float)b->y, (float)b->w, (float)b->h,
                  (float)b->h * 0.25f, C_PANEL);
            int lh = od_ttf_line_height(v->font, it->face, it->px);
            int asc = od_ttf_ascent(v->font, it->face, it->px);
            od_ttf_draw(v->font, it->face, it->px, buf, w, h, b->x + (int)lroundf(0.02f * u),
                        b->y + (b->h - lh) / 2 + asc, it->text, it->color);
            break;
        }
        case OD_IV_CARET:
            for (int yy = b->y; yy < b->y + b->h && yy < h; yy++)
                for (int xx = b->x; xx < b->x + b->w && xx < w; xx++)
                    buf[yy * w + xx] = it->color;
            break;
        }
    }
}

int od_ui_import_view_button_hit(od_ui_import_view *v, const od_ui_import *st,
                                 int w, int h, int x, int y)
{
    od_iv_layout L;
    od_ui_import_view_layout(v, st, w, h, &L);
    if (L.button < 0)
        return 0;
    const od_ui_box *b = &L.item[L.button].box;
    return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}
