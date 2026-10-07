/* ui_ttf.c - Inter text through stb_truetype (C6.48). See ui_ttf.h. */
#include "opendana/ui_ttf.h"

#include "opendana/platform.h"
#include "../import/fsutil.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define CACHE_N 1024

typedef struct {
    int face, px;
    uint32_t cp;
    int used;
    int w, h, xoff, yoff;
    float adv;
    unsigned char *bmp;
} glyph;

struct od_ttf {
    unsigned char *data[2];
    stbtt_fontinfo info[2];
    glyph cache[CACHE_N];
};

static const char *FILES[2] = {"Inter-Regular.ttf", "Inter-SemiBold.ttf"};

static int dir_has_fonts(const char *dir)
{
    for (int i = 0; i < 2; i++) {
        char p[1400];
        od_platform_join(p, sizeof p, dir, FILES[i]);
        FILE *fp = od_fs_fopen(p, "rb");
        if (fp == NULL)
            return 0;
        fclose(fp);
    }
    return 1;
}

int od_ttf_find_dir(char *out, size_t cap)
{
    const char *env = getenv("OD_FONT_DIR");
    if (env != NULL && env[0] != '\0') {
        snprintf(out, cap, "%s", env);
        return dir_has_fonts(out);
    }
    char exe[1100];
    if (!od_platform_executable_dir(exe, sizeof exe))
        return 0;
    static const char *REL[] = {"/../share/opendana/fonts", "/../assets/fonts",
                                "/assets/fonts"};
    for (size_t i = 0; i < sizeof REL / sizeof REL[0]; i++) {
        snprintf(out, cap, "%s%s", exe, REL[i]);
        if (dir_has_fonts(out))
            return 1;
    }
    out[0] = '\0';
    return 0;
}

static unsigned char *read_file(const char *path)
{
    FILE *fp = od_fs_fopen(path, "rb");
    if (fp == NULL)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *b = n > 0 && n < (32L << 20) ? (unsigned char *)malloc((size_t)n) : NULL;
    if (b != NULL && fread(b, 1, (size_t)n, fp) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(fp);
    return b;
}

static void flush(od_ttf *f)
{
    for (int i = 0; i < CACHE_N; i++)
        if (f->cache[i].used)
            free(f->cache[i].bmp);
    memset(f->cache, 0, sizeof f->cache);
}

void od_ttf_close(od_ttf *f)
{
    if (f == NULL)
        return;
    flush(f);
    free(f->data[0]);
    free(f->data[1]);
    free(f);
}

od_ttf *od_ttf_open(const char *dir)
{
    if (dir == NULL)
        return NULL;
    od_ttf *f = (od_ttf *)calloc(1, sizeof *f);
    if (f == NULL)
        return NULL;
    for (int i = 0; i < 2; i++) {
        char p[1400];
        od_platform_join(p, sizeof p, dir, FILES[i]);
        f->data[i] = read_file(p);
        if (f->data[i] == NULL ||
            !stbtt_InitFont(&f->info[i], f->data[i],
                            stbtt_GetFontOffsetForIndex(f->data[i], 0))) {
            od_ttf_close(f);
            return NULL;
        }
    }
    return f;
}

static float scale_of(od_ttf *f, int face, int px)
{
    return stbtt_ScaleForMappingEmToPixels(&f->info[face], (float)px);
}

static glyph *get_glyph(od_ttf *f, int face, int px, uint32_t cp)
{
    uint32_t h = (cp * 2654435761u) ^ ((uint32_t)px * 40503u) ^ (uint32_t)face;
    glyph *g = NULL;
    for (int round = 0; round < 2 && g == NULL; round++) {
        for (int probe = 0; probe < CACHE_N; probe++) {
            glyph *c = &f->cache[(h + (uint32_t)probe) % CACHE_N];
            if (!c->used) {
                g = c;
                break;
            }
            if (c->face == face && c->px == px && c->cp == cp)
                return c;
        }
        if (g == NULL)
            flush(f);
    }
    float sc = scale_of(f, face, px);
    int gi = stbtt_FindGlyphIndex(&f->info[face], (int)cp);
    int adv = 0, lsb = 0, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(&f->info[face], gi, &adv, &lsb);
    stbtt_GetGlyphBitmapBox(&f->info[face], gi, sc, sc, &x0, &y0, &x1, &y1);
    memset(g, 0, sizeof *g);
    g->used = 1;
    g->face = face;
    g->px = px;
    g->cp = cp;
    g->adv = (float)adv * sc;
    g->w = x1 - x0;
    g->h = y1 - y0;
    g->xoff = x0;
    g->yoff = y0;
    if (g->w > 0 && g->h > 0) {
        g->bmp = (unsigned char *)calloc((size_t)g->w * (size_t)g->h, 1);
        if (g->bmp != NULL)
            stbtt_MakeGlyphBitmap(&f->info[face], g->bmp, g->w, g->h, g->w, sc, sc, gi);
    }
    return g;
}

static uint32_t next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = p[0];
    int n = 1;
    if (c >= 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        c = ((c & 7u) << 18) | ((p[1] & 63u) << 12) | ((p[2] & 63u) << 6) | (p[3] & 63u);
        n = 4;
    } else if (c >= 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        c = ((c & 15u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u);
        n = 3;
    } else if (c >= 0xC0 && (p[1] & 0xC0) == 0x80) {
        c = ((c & 31u) << 6) | (p[1] & 63u);
        n = 2;
    } else if (c >= 0x80) {
        c = '?';
    }
    *s += n;
    return c;
}

int od_ttf_width(od_ttf *f, int face, int px, const char *s)
{
    float w = 0;
    while (f != NULL && s != NULL && *s)
        w += get_glyph(f, face, px, next_cp(&s))->adv;
    return (int)ceilf(w);
}

int od_ttf_ascent(od_ttf *f, int face, int px)
{
    int a, d, g;
    stbtt_GetFontVMetrics(&f->info[face], &a, &d, &g);
    return (int)lroundf((float)a * scale_of(f, face, px));
}

int od_ttf_line_height(od_ttf *f, int face, int px)
{
    int a, d, g;
    stbtt_GetFontVMetrics(&f->info[face], &a, &d, &g);
    return (int)lroundf((float)(a - d + g) * scale_of(f, face, px));
}

void od_ttf_draw(od_ttf *f, int face, int px, uint32_t *buf, int bw, int bh,
                 int x, int by, const char *s, uint32_t argb)
{
    float pen = (float)x;
    unsigned sa = argb >> 24, sr = (argb >> 16) & 255, sg = (argb >> 8) & 255,
             sb = argb & 255;
    while (f != NULL && s != NULL && *s) {
        glyph *g = get_glyph(f, face, px, next_cp(&s));
        int gx = (int)lroundf(pen) + g->xoff, gy = by + g->yoff;
        for (int j = 0; g->bmp != NULL && j < g->h; j++) {
            int yy = gy + j;
            if (yy < 0 || yy >= bh)
                continue;
            for (int i = 0; i < g->w; i++) {
                int xx = gx + i;
                unsigned a = (g->bmp[j * g->w + i] * sa + 127) / 255;
                if (xx < 0 || xx >= bw || a == 0)
                    continue;
                uint32_t d = buf[yy * bw + xx];
                unsigned dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
                dr = (sr * a + dr * (255 - a) + 127) / 255;
                dg = (sg * a + dg * (255 - a) + 127) / 255;
                db = (sb * a + db * (255 - a) + 127) / 255;
                buf[yy * bw + xx] = 0xFF000000u | (dr << 16) | (dg << 8) | db;
            }
        }
        pen += g->adv;
    }
}

const char *od_ttf_fit(od_ttf *f, int face, int px, const char *s, int max_w,
                       char *out, size_t cap)
{
    char src[800];
    snprintf(src, sizeof src, "%s", s);
    snprintf(out, cap, "%s", src);
    if (od_ttf_width(f, face, px, out) <= max_w)
        return out;
    size_t n = strlen(src);
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
        char tmp[800];
        snprintf(tmp, sizeof tmp, "%.*s...", (int)n, src);
        if (od_ttf_width(f, face, px, tmp) <= max_w || n == 0) {
            snprintf(out, cap, "%s", tmp);
            return out;
        }
    }
    return out;
}

int od_ttf_wrap(od_ttf *f, int face, int px, const char *s, int max_w,
                char *out, size_t line_cap, int max_lines)
{
    int lines = 0;
    char cur[800] = "";
    const char *p = s;
    int left = 0;
    while (*p) {
        const char *e = p;
        while (*e && *e != ' ')
            e++;
        char word[256];
        snprintf(word, sizeof word, "%.*s", (int)(e - p), p);
        char cand[1100];
        snprintf(cand, sizeof cand, "%s%s%s", cur, cur[0] ? " " : "", word);
        if (cur[0] && od_ttf_width(f, face, px, cand) > max_w) {
            if (lines == max_lines - 1) {
                left = 1; /* no room for another line */
                break;
            }
            snprintf(out + (size_t)lines * line_cap, line_cap, "%s", cur);
            lines++;
            snprintf(cur, sizeof cur, "%s", word);
        } else {
            snprintf(cur, sizeof cur, "%.790s", cand);
        }
        p = *e ? e + 1 : e;
    }
    if (left) {
        char tmp[800];
        snprintf(tmp, sizeof tmp, "%.790s ...", cur);
        snprintf(cur, sizeof cur, "%s", tmp);
    }
    if (cur[0] || lines == 0) {
        snprintf(out + (size_t)lines * line_cap, line_cap, "%s", cur);
        lines++;
    }
    for (int i = 0; i < lines; i++) {
        char tmp[800];
        snprintf(tmp, sizeof tmp, "%s", out + (size_t)i * line_cap);
        od_ttf_fit(f, face, px, tmp, max_w, out + (size_t)i * line_cap, line_cap);
    }
    return lines;
}
