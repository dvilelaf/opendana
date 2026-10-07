/* hud.c - the ROUND/TIME status bar. See opendana/hud.h for the model. */
#include "opendana/hud.h"

#include "opendana/tile.h"

#include <stdio.h>

/* Decimal increment done by hand, digit by digit. */
static uint8_t decimal_next(uint8_t value, unsigned limit)
{
    unsigned next = value + 1u;
    if ((next & 15) >= 10) {
        next = (next & 0xF0) + 0x10;
        if (next >= limit)
            next = 0;
    }
    return (uint8_t)next;
}

int od_hud_clock_tick(od_hud_clock *clock, uint8_t hero_flags, uint8_t scene_flags)
{
    /* The gate covers the entire operation, not just the decimal increment. */
    if ((hero_flags & 1) || scene_flags)
        return 0;
    if (clock->countdown & 0x80)
        return 1;
    clock->countdown--;
    if (clock->countdown)
        return 1;
    clock->countdown = 60;
    clock->seconds = decimal_next(clock->seconds, 0x60);
    if (clock->seconds)
        return 1;
    clock->minutes = decimal_next(clock->minutes, 0x60);
    if (clock->minutes)
        return 1;
    clock->hours = decimal_next(clock->hours, 0xA0);
    if (!clock->hours) {
        clock->countdown = 0x80;
        clock->hours = 0x99;
        clock->minutes = clock->seconds = 0x59;
        return 0; /* returns before the queue; next call writes it. */
    }
    return 1;
}

void od_hud_clock_text(char *out, size_t cap, const od_hud_clock *clock)
{
    if (!cap)
        return;
    snprintf(out, cap, "%02X:%02X:%02X", (unsigned)clock->hours,
             (unsigned)clock->minutes, (unsigned)clock->seconds);
}

/* Draw one glyph, optionally enlarged by `scale`. The CHR tile index is the
 * ASCII code itself; out-of-range characters fall back to a space. */
static void putc_scaled(od_index_frame *fb, int px, int py, char c, int scale,
                        uint8_t bg_pal, const uint8_t *chr, const od_palette_ram *pal)
{
    unsigned ch = (unsigned char)c;
    /* The CHR font has no blank tile at ASCII 32 (tile 32 is a box glyph), so
     * spaces and control characters fall back to tile 0, which is empty. */
    if (ch < 33 || ch > 126)
        ch = 0;
    const uint8_t *t = chr + (size_t)ch * OD_TILE_BYTES;
    const uint8_t base = (uint8_t)(bg_pal * 4);
    if (scale < 1)
        scale = 1;
    for (int sy = 0; sy < 8; sy++) {
        for (int sx = 0; sx < 8; sx++) {
            uint8_t v = od_tile_pixel(t, sx, sy);
            uint8_t col = pal->ram[base + v];
            for (int oy = 0; oy < scale; oy++) {
                int fy = py + sy * scale + oy;
                if (fy < 0 || fy >= OD_SCREEN_H)
                    continue;
                for (int ox = 0; ox < scale; ox++) {
                    int fx = px + sx * scale + ox;
                    if (fx < 0 || fx >= OD_SCREEN_W)
                        continue;
                    fb->index[fy * OD_SCREEN_W + fx] = col;
                }
            }
        }
    }
}

void od_font_putc(od_index_frame *fb, int px, int py, char c,
                   uint8_t bg_pal, const uint8_t *chr, const od_palette_ram *pal)
{
    putc_scaled(fb, px, py, c, 1, bg_pal, chr, pal);
}

void od_font_text_scaled(od_index_frame *fb, int px, int py, const char *s,
                          int scale, uint8_t bg_pal,
                          const uint8_t *chr, const od_palette_ram *pal)
{
    if (scale < 1)
        scale = 1;
    for (int i = 0; s[i] != '\0'; i++)
        putc_scaled(fb, px + i * 8 * scale, py, s[i], scale, bg_pal, chr, pal);
}

void od_hud_round_text(char *out, size_t cap, int level_index)
{
    if (cap == 0)
        return;
    if (level_index < 0)
        level_index = 0;
    /* Levels 100..149 are the hidden rounds (worlds 11..15): the original keeps
     * showing "ROUND w-r" there, not a flat index. Same label as
     * od_hidden_round_text()/worldmap.h. */
    if (level_index < 100)
        snprintf(out, cap, "%d-%d", level_index / 10 + 1, level_index % 10 + 1);
    else
        snprintf(out, cap, "%d-%d", (level_index - 100) / 10 + 11,
                 (level_index - 100) % 10 + 1);
}

void od_hud_time_text(char *out, size_t cap, long frames)
{
    if (cap == 0)
        return;
    if (frames < 0)
        frames = 0;
    long secs = frames / 60;          /* caller supplies enabled simulation ticks */
    if (secs > 359999)
        secs = 359999;               /* saturate at 99:59:59 */
    long hh = secs / 3600;
    long mm = (secs / 60) % 60;
    long ss = secs % 60;
    snprintf(out, cap, "%02ld:%02ld:%02ld", hh, mm, ss);
}

void od_hud_begin_frame(od_hud_latch *latch)
{
    if (latch->pending) {
        latch->visible = latch->queued;
        latch->pending = 0;
    }
}

void od_hud_queue(od_hud_latch *latch, const od_hud_clock *clock)
{
    latch->queued = *clock;
    latch->pending = 1;
}

static void draw_text_value(od_index_frame *fb, const char *value, int val_col,
                            const char *time, const uint8_t *chr,
                            const od_palette_ram *pal)
{
    /* Opaque black band over the bottom 24 rows (the level's last 1.5 cells). */
    uint8_t black = pal->ram[OD_HUD_BG_PAL * 4 + 0];
    for (int y = OD_HUD_TOP; y < OD_SCREEN_H; y++)
        for (int x = 0; x < OD_SCREEN_W; x++)
            fb->index[y * OD_SCREEN_W + x] = black;

    od_font_text(fb, OD_HUD_ROUND_COL * 8, OD_HUD_TEXT_Y, "ROUND", chr, pal);
    od_font_text(fb, val_col * 8, OD_HUD_TEXT_Y, value, chr, pal);
    od_font_text(fb, OD_HUD_TIME_COL * 8, OD_HUD_TEXT_Y, "TIME", chr, pal);
    od_font_text(fb, OD_HUD_TIME_VAL_COL * 8, OD_HUD_TEXT_Y, time, chr, pal);
}

static void draw_text(od_index_frame *fb, int level_index, const char *time,
                       const uint8_t *chr, const od_palette_ram *pal)
{
    char buf[16];
    od_hud_round_text(buf, sizeof(buf), level_index);
    /* The template keeps its '-' in column 12: the world's digits end in column
     * 11 and the round's start in column 13, so a two-digit world (10-x and the
     * hidden 11-1..15-10) starts one tile to the left: reference 10-10 ink in tiles
     * 10..14 (R4 f21362), 11-1 first ink x=82 -> tile 10. */
    int val_col = OD_HUD_ROUND_VAL_COL - (level_index >= 90 ? 1 : 0);
    draw_text_value(fb, buf, val_col, time, chr, pal);
}

void od_hud_draw_clock_user(od_index_frame *fb, int user_level,
                             const od_hud_clock *clock,
                             const uint8_t *chr, const od_palette_ram *pal)
{
    /* The level number plus 1 as decimal digits in tile columns 11 and 12; a single
     * digit leaves column 12 blank, erasing the template's '-' (reference VRAM
     * queue f2906: column 12 <- 00, column 11 <- '1'). */
    char time[16], value[8];
    if (user_level < 0)
        user_level = 0;
    snprintf(value, sizeof value, "%d", user_level + 1);
    od_hud_clock_text(time, sizeof(time), clock);
    draw_text_value(fb, value, OD_HUD_ROUND_VAL_COL, time, chr, pal);
}

void od_hud_draw_clock(od_index_frame *fb, int level_index,
                        const od_hud_clock *clock,
                        const uint8_t *chr, const od_palette_ram *pal)
{
    char time[16];
    od_hud_clock_text(time, sizeof(time), clock);
    draw_text(fb, level_index, time, chr, pal);
}

void od_hud_draw_row(od_index_frame *fb, const uint8_t row[32],
                      const od_hud_clock *clock,
                      const uint8_t *chr, const od_palette_ram *pal)
{
    uint8_t black = pal->ram[OD_HUD_BG_PAL * 4 + 0];
    for (int y = OD_HUD_TOP; y < OD_SCREEN_H; y++)
        for (int x = 0; x < OD_SCREEN_W; x++)
            fb->index[y * OD_SCREEN_W + x] = black;
    char line[33], time[16];
    for (int i = 0; i < 32; i++)
        line[i] = row[i] ? (char)row[i] : ' ';
    line[32] = '\0';
    od_hud_clock_text(time, sizeof(time), clock);
    for (int i = 0; i < OD_HUD_TIME_CHARS && time[i]; i++)
        line[OD_HUD_TIME_VAL_COL + i] = time[i];
    od_font_text(fb, 0, OD_HUD_TEXT_Y, line, chr, pal);
}

static void draw_template_cells(od_index_frame *fb, const char *cells,
                                const uint8_t *chr, const od_palette_ram *pal)
{
    for (int i = 0; i < OD_HUD_TIME_CHARS && cells[i]; i++) {
        int px = (OD_HUD_TIME_VAL_COL + i) * 8;
        if (cells[i] == ':') {
            od_font_text(fb, px, OD_HUD_TEXT_Y, ":", chr, pal);
            continue;
        }
        /* putc_scaled maps ASCII 32 to the empty tile 0; the template really
         * writes tile 32, so it is drawn by index here. */
        const uint8_t *t = chr + (size_t)0x20 * OD_TILE_BYTES;
        for (int sy = 0; sy < 8; sy++)
            for (int sx = 0; sx < 8; sx++)
                fb->index[(OD_HUD_TEXT_Y + sy) * OD_SCREEN_W + px + sx] =
                    pal->ram[OD_HUD_BG_PAL * 4 + od_tile_pixel(t, sx, sy)];
    }
}

void od_hud_draw_template(od_index_frame *fb, const char *value, int val_col,
                           const od_hud_clock *clock,
                           const uint8_t *chr, const od_palette_ram *pal)
{
    /* The template is written by the VRAM record writer: 00 00 "ROUND" 00 00 00 '-'
     * 00 00 00 00 "TIME" 00 20 20 ':' 20 20 ':' 20 20 00. The template's time cells
     * hold tile 32 (the font's box glyph) until the clock writer next writes the
     * digits; the round value is blank until the round writer puts it back. */
    char time[16];
    draw_text_value(fb, value ? value : "", val_col, "", chr, pal);
    if (!value)
        od_font_text(fb, (OD_HUD_ROUND_VAL_COL + 1) * 8, OD_HUD_TEXT_Y, "-",
                      chr, pal);
    if (clock) {
        od_hud_clock_text(time, sizeof(time), clock);
        od_font_text(fb, OD_HUD_TIME_VAL_COL * 8, OD_HUD_TEXT_Y, time, chr, pal);
        return;
    }
    draw_template_cells(fb, "\x20\x20:\x20\x20:\x20\x20", chr, pal);
}

void od_hud_draw_level_template(od_index_frame *fb, const char *value,
                                 int val_col, const uint8_t *chr,
                                 const od_palette_ram *pal)
{
    /* The template ends after the minutes' ':' (24 bytes), so the seconds cells
     * stay empty until the clock writer. */
    draw_text_value(fb, value ? value : "", val_col, "", chr, pal);
    draw_template_cells(fb, "\x20\x20:\x20\x20:", chr, pal);
}

void od_hud_draw(od_index_frame *fb, int level_index, long frames,
                  const uint8_t *chr, const od_palette_ram *pal)
{
    char time[16];
    od_hud_time_text(time, sizeof(time), frames);
    draw_text(fb, level_index, time, chr, pal);
}
