/* worldmap.c - WORLD n level select + progression. See opendana/worldmap.h for the
 * model. */
#include "opendana/worldmap.h"

#include "opendana/hud.h"

#include <stdio.h>
#include <string.h>

/* Palette indices of the hidden menu: black background and the blue cursor
 * triangle (PAL RGB 17,81,170). */
#define C_BLACK  0x0F
#define C_HIDDEN_CURSOR 0x11

/* ---- progression model ------------------------------------------------- */

static void bit_set(uint8_t *bits, int i)
{
    bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static int bit_get(const uint8_t *bits, int i)
{
    return (bits[i >> 3] >> (i & 7)) & 1;
}

void od_progress_reset(od_progress *p)
{
    for (size_t i = 0; i < sizeof(p->cleared); i++)
        p->cleared[i] = 0;
    p->world = 0;
    p->cursor = 0;
}

int od_progress_cleared(const od_progress *p, int level)
{
    if (level < 0 || level >= OD_LEVEL_COUNT)
        return 0;
    return bit_get(p->cleared, level);
}

void od_progress_set_cleared(od_progress *p, int level, int on)
{
    if (level < 0 || level >= OD_LEVEL_COUNT)
        return;
    if (on)
        bit_set(p->cleared, level);
    else
        p->cleared[level >> 3] &= (uint8_t)~(1u << (level & 7));
}

int od_progress_clear_count(const od_progress *p)
{
    int n = 0;
    for (int i = 0; i < OD_LEVEL_COUNT; i++)
        n += od_progress_cleared(p, i);
    return n;
}

int od_progress_unlocked(const od_progress *p, int level)
{
    if (level < 0 || level >= OD_LEVEL_COUNT)
        return 0;
    if (level == 0)
        return 1;
    return od_progress_cleared(p, level - 1);
}

int od_progress_marker_state(const od_progress *p, int round)
{
    if (round < 0 || round >= OD_MAP_SLOTS)
        return 0;
    int level = p->world * OD_WORLD_ROUNDS + round;
    if (od_progress_cleared(p, level))
        return 2;
    if (od_progress_unlocked(p, level))
        return 1;
    return 0;
}

int od_progress_move(od_progress *p, int dx, int dy)
{
    if (dy != 0) {
        int nw = p->world + dy;
        if (nw < 0 || nw >= OD_WORLD_COUNT)
            return 0;
        if (!od_progress_unlocked(p, nw * OD_WORLD_ROUNDS))
            return 0;
        p->world = nw;
        return 1;
    }
    if (dx != 0) {
        int nc = p->cursor + dx;
        if (nc < 0 || nc >= OD_MAP_SLOTS)
            return 0; /* the reference stops at the ends, it does not wrap */
        p->cursor = nc;
        return 1;
    }
    return 0;
}

int od_progress_enter(const od_progress *p, int *out_level)
{
    int level = p->world * OD_WORLD_ROUNDS + p->cursor;
    if (!od_progress_unlocked(p, level))
        return -1;
    if (out_level)
        *out_level = level;
    return 0;
}

void od_progress_complete(od_progress *p, int level)
{
    if (level < 0 || level >= OD_LEVEL_COUNT)
        return;
    od_progress_set_cleared(p, level, 1);
    int next = level + 1;
    /* The map only has levels 0..99. Completing level 99 (or any hidden
     * level) must not move the cursor onto / past the hidden rounds. */
    if (next < OD_HIDDEN_FIRST) {
        p->world = next / OD_WORLD_ROUNDS;
        p->cursor = next % OD_WORLD_ROUNDS;
    }
}

/* One world's low ten bits: bit j = round j+1 cleared. */
static unsigned progress_low_word(const od_progress *p, int world)
{
    unsigned word = 0;
    for (int r = 0; r < OD_WORLD_ROUNDS; ++r)
        if (od_progress_cleared(p, world * OD_WORLD_ROUNDS + r))
            word |= 1u << r;
    return word;
}

/* The magicword validator's progress writer. It packs the ten decoded world
 * words and then stamps the high bits in one pass, with a latch that is kept
 * between worlds. Reproduced branch by branch:
 *   low byte not full          -> latch
 *   high & 3 == 0              -> latch
 *   high & 3 == 3              -> complete-world high byte
 *   latch already set          -> keep
 *   otherwise                  -> frontier-world high byte, then latch
 * Only one frontier world can ever be marked, because the latch is set on the
 * first world that is not a complete ten-round world. */
void od_progress_map_words(const od_progress *p, uint16_t out[OD_WORLD_COUNT])
{
    int latched = 0; /* the latch kept between worlds */
    for (int w = 0; w < OD_WORLD_COUNT; ++w) {
        unsigned word = progress_low_word(p, w);
        unsigned low = word & 0xffu;
        unsigned high = (word >> 8) & 3u;
        if (low == 0xffu) {
            if (high == 3u)
                word |= 0xc000u;          /* complete world -> FF C3 */
            else if (high != 0u && !latched) {
                word |= 0x4000u;          /* first frontier -> FF 41 */
                latched = 1;
            } else
                latched = 1;              /* latch */
        } else
            latched = 1;                  /* latch */
        out[w] = (uint16_t)word;
    }
}

od_clear_cinematic od_progress_clear_cinematic(const od_progress *before, int level,
                                                 int *world, int *castle_open)
{
    if (world)
        *world = -1;
    if (castle_open)
        *castle_open = 0;
    if (!before || level < 0 || level >= OD_HIDDEN_FIRST)
        return OD_CLEAR_NONE;
    uint16_t words[OD_WORLD_COUNT];
    od_progress_map_words(before, words);
    /* The cleared round's bit, low byte for rounds 1..8. */
    int b6 = level / OD_WORLD_ROUNDS, b5 = level % OD_WORLD_ROUNDS;
    words[b6] |= (uint16_t)(1u << b5);
    od_clear_cinematic result = OD_CLEAR_NONE;
    /* Walk the worlds. */
    for (int w = 0; w < OD_WORLD_COUNT; ++w) {
        unsigned low = words[w] & 0xffu, high = words[w] >> 8;
        if (low != 0xffu || high == 0u)
            return result;                    /* no world left: done */
        if (high & 0x80u)
            continue;                         /* world done */
        if (!(high & 0x40u)) {
            /* bit 6, cinematic sequence 0x20 + world, return. */
            if (world)
                *world = w;
            return OD_CLEAR_CASTLE_APPEARS;
        }
        if ((high & 3u) != 3u)
            return result;                    /* return */
        if (b6 == OD_WORLD_COUNT - 1)
            return result;                    /* world 10 -> final-castle mode */
        /* bit 7, end-of-world sequence, world counter += 1, selection flag set. */
        words[w] |= 0x8000u;
        if (result == OD_CLEAR_NONE && world)
            *world = b6;
        result = OD_CLEAR_WORLD_END;
        ++b6;
        /* Index 2w+2; before the last index the walk goes on, at it (world 9 just celebrated)
         * the castle opening plays and the world limit becomes 10. */
        if (w == OD_WORLD_COUNT - 2) {
            if (castle_open)
                *castle_open = 1;
            return result;
        }
        if (w >= OD_WORLD_COUNT - 1)
            return result;
    }
    return result;
}

uint16_t od_progress_map_word(const od_progress *p, int world)
{
    if (world < 0 || world >= OD_WORLD_COUNT)
        return 0;
    uint16_t words[OD_WORLD_COUNT];
    od_progress_map_words(p, words);
    return words[world];
}

void od_level_world_round(int level, int *world, int *round)
{
    if (level < 0 || level >= OD_LEVEL_COUNT)
        level = 0; /* keep the label well-defined for out-of-range input */
    if (world)
        *world = level / OD_WORLD_ROUNDS + 1;
    if (round)
        *round = level % OD_WORLD_ROUNDS + 1;
}

/* ---- hidden menu (levels 100..149) ------------------------------------- */

void od_hidden_reset(od_hidden_menu *h)
{
    h->active = 0;
    h->counting = 0;
    h->presses = 0;
    h->level = OD_HIDDEN_FIRST;
    h->sound = 0;
}

int od_hidden_title_tick(od_hidden_menu *h, unsigned buttons, unsigned pressed)
{
    if (h->active)
        return 0;
    if (!(buttons & OD_BTN_SELECT)) {
        /* SELECT not held: the counter is disarmed. */
        h->counting = 0;
        h->presses = 0;
        return 0;
    }
    if (!h->counting) {
        /* first SELECT frame latches the arm flag, still not counting. */
        h->counting = 1;
        h->presses = 0;
        return 0;
    }
    if (pressed & OD_BTN_B) {
        h->presses++;
        if (h->presses >= OD_HIDDEN_B_PRESSES) {
            /* ten B presses with SELECT -> hidden menu, level 100. */
            h->active = 1;
            h->counting = 0;
            h->presses = 0;
            h->level = OD_HIDDEN_FIRST;
            h->sound = 0;
            return 1;
        }
    }
    return 0;
}

static int hidden_step(int level, int delta)
{
    int rel = (level - OD_HIDDEN_FIRST + delta) % OD_HIDDEN_COUNT;
    if (rel < 0)
        rel += OD_HIDDEN_COUNT;
    return OD_HIDDEN_FIRST + rel;
}

int od_hidden_menu_tick(od_hidden_menu *h, unsigned buttons, unsigned pressed)
{
    if (!h->active)
        return -1;
    /* Without SELECT: A/B step the level by one across the whole 100..149 range
     * (wrap). With SELECT held the original jumps a whole world (measured:
     * SELECT+A = +10, SELECT+B = -10). */
    int step = (buttons & OD_BTN_SELECT) ? OD_WORLD_ROUNDS : 1;
    if (pressed & OD_BTN_A)
        h->level = hidden_step(h->level, +step);
    else if (pressed & OD_BTN_B)
        h->level = hidden_step(h->level, -step);
    if (pressed & OD_BTN_START)
        return h->level;
    return -1;
}

void od_hidden_round_text(char *out, size_t cap, int level)
{
    if (!cap)
        return;
    int world, round;
    od_level_world_round(level, &world, &round);
    snprintf(out, cap, "%d-%d", world, round);
}

/* ---- drawing primitives ------------------------------------------------ */

static void fill_rect(od_index_frame *fb, int x0, int y0, int x1, int y1, uint8_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > OD_SCREEN_W - 1) x1 = OD_SCREEN_W - 1;
    if (y1 > OD_SCREEN_H - 1) y1 = OD_SCREEN_H - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            fb->index[y * OD_SCREEN_W + x] = c;
}

/* Right-pointing cursor measured from the reference screen: seven rows of
 * widths 1..4..1 anchored at the left column, first pixel at y+1. */
static void draw_hidden_cursor(od_index_frame *fb, int x, int y)
{
    static const int widths[7] = { 1, 2, 3, 4, 3, 2, 1 };
    for (int i = 0; i < 7; i++)
        fill_rect(fb, x, y + 1 + i, x + widths[i] - 1, y + 1 + i,
                  C_HIDDEN_CURSOR);
}

void od_hidden_menu_draw(od_index_frame *fb, const od_hidden_menu *h,
                          const uint8_t *chr, const od_palette_ram *pal)
{
    fill_rect(fb, 0, 0, OD_SCREEN_W - 1, OD_SCREEN_H - 1, C_BLACK);

    char round[8], sound[8];
    od_hidden_round_text(round, sizeof(round), h ? h->level : OD_HIDDEN_FIRST);
    snprintf(sound, sizeof(sound), "%0*d", OD_HIDDEN_SOUND_DIGITS,
             h ? h->sound : 0);

    /* Labels start at the measured column; the ROUND value starts at tile
     * column 14 (a five-character "12-10" runs one tile further right) and
     * the SOUND value is right-aligned, exactly as in the reference capture. */
    od_font_text(fb, OD_HIDDEN_LABEL_X, OD_HIDDEN_ROW0_Y, "ROUND", chr, pal);
    od_font_text(fb, OD_HIDDEN_ROUND_X, OD_HIDDEN_ROW0_Y, round, chr, pal);

    od_font_text(fb, OD_HIDDEN_LABEL_X, OD_HIDDEN_ROW1_Y, "SOUND", chr, pal);
    od_font_text(fb, OD_HIDDEN_VALUE_RIGHT - OD_HIDDEN_SOUND_DIGITS * 8,
                  OD_HIDDEN_ROW1_Y, sound, chr, pal);

    draw_hidden_cursor(fb, OD_HIDDEN_CURSOR_X, OD_HIDDEN_CURSOR_Y);
}
