/* scene_map.c - the WORLD map as logic (C3.2).
 *
 * The map is no longer a recorded clip: the scene keeps a frozen layout (the
 * extracted reference map, `screens.c`), and this module supplies the dynamic
 * panel (WORLD n number, progress marks and the Hero cursor) from the session
 * progress. The world step is bounded only by the world limit (9, or 10 with
 * world 9's castle), whatever the progress; the level index stays validated
 * (A5). The round moves on the press edge (A7). Pure C11, no SDL. */
#include "opendana/scene_map.h"
#include "opendana/scene_data.h"
#include "opendana/content_gfx.h"

#include "opendana/game.h"
#include "opendana/tile.h"

#include <string.h>

/* Advance / retreat tables: the direction bit that walks the world pointer one
 * step, indexed by the current world. Direction bits use the original's
 * remapped order bit0=RIGHT, bit1=LEFT, bit2=DOWN, bit3=UP. The measured
 * WORLD 1->10 sequence is RIGHT, RIGHT, DOWN, DOWN, DOWN, LEFT, LEFT, UP, UP. */

/* Layout offsets inside the OD_SCENE_WORDS display vector (screens.c). */
#define MAP_ATTR_BASE   992u   /* 32 (palette) + 960 (nametable) */
#define MAP_SPRITE_BASE 2124u  /* count, then 4 words per sprite */
#define MAP_SPRITE_MAX  64u
#define MAP_BG_BANK_BASE 2080u /* background banks (x 64) */

/* Marker geometry of the extracted panel: round i (1..9) sits at tile column
 * 4+2(i-1), rows 24 (top oval) and 25 (bottom oval), inside attribute block row
 * 6. The 2-bit palette field of that marker is at byte
 * `MAP_ATTR_BASE + 6*8 + col/4`, selector `col & 2`. */
#define MAP_MARKER_ROW0 24u
#define MAP_MARKER_ROW1 25u
#define MAP_MARKER_COL0 4u

/* Marker palettes in the extracted map (0xFF = palette 3 everywhere): blue
 * (0x21) is a cleared round, red (0x16) an uncleared one. */
#define MAP_PAL_CLEARED   1u
#define MAP_PAL_UNCLEARED 3u

static unsigned map_direction(unsigned buttons)
{
    unsigned d = 0;
    if (buttons & OD_BTN_RIGHT) d |= 0x01;
    if (buttons & OD_BTN_LEFT)  d |= 0x02;
    if (buttons & OD_BTN_DOWN)  d |= 0x04;
    if (buttons & OD_BTN_UP)    d |= 0x08;
    return d;
}

void od_scene_map_reset(od_scene_map *m)
{
    memset(m, 0, sizeof *m);
    od_scene_round_begin(&m->sel, 0);
}
od_status od_scene_map_static_base(const od_scene_map *m,uint16_t *display,unsigned words)
{
    const od_static_layouts *l=od_gfx_static_layouts();
    if(!m||!display||words<2382||!l)return OD_ERR_FORMAT;
    const od_static_map_base *b=&l->map_bases[(m->words[8]&0x8000u)!=0];
    for(unsigned i=0;i<2048;++i)display[32+i]=b->ciram[i];
    for(unsigned i=0;i<4;++i)display[2080+i]=display[2084+i]=64u*b->background_banks[i];
    display[2381]=(display[2381]&1u)|(b->chr_mode?2u:0u);
    return OD_OK;
}

void od_scene_map_open(od_scene_map *m, const uint16_t words[OD_MAP_WORLD_COUNT],
                        int world, unsigned ready)
{
    od_scene_map_reset(m);
    if (world < 0)
        world = 0;
    if (world > OD_MAP_WORLD_COUNT - 1)
        world = OD_MAP_WORLD_COUNT - 1;
    m->world = world;
    if (words)
        memcpy(m->words, words, sizeof m->words);
    m->ready = ready;
}

void od_scene_map_focus_world(od_scene_map *m)
{
    m->focus = 0;
    m->round = m->sel.round;
}

/* The castle latch is set when rounds 1-9 are cleared (low byte full and bit 0
 * of the high byte) and, past world 1, the previous world's castle (bit 15). */
static int map_castle_latch(const od_scene_map *m)
{
    unsigned word = m->words[m->world];
    return (word & 0x01ffu) == 0x01ffu &&
           (m->world == 0 || (m->words[m->world - 1] & 0x8000u));
}

/* WORLD focus `A`: the round cursor starts on the first uncleared round; with
 * all ten cleared the count is reset to 0, and round 10 without the castle
 * latch falls back to round 1. */
static int map_round_on_entry(const od_scene_map *m)
{
    unsigned word = m->words[m->world] & 0x03ffu;
    int r = 0;
    while (r < OD_MAP_ROUND_COUNT && (word & (1u << r)))
        r++;
    if (r == OD_MAP_ROUND_COUNT)
        return 0;
    if (r == OD_MAP_ROUND_COUNT - 1 && !map_castle_latch(m))
        return 0;
    return r;
}

void od_scene_map_resume_round(od_scene_map *m)
{
    int round = od_scene_round_first(m->words, m->world);
    if (round == OD_MAP_ROUND_COUNT - 1 && !map_castle_latch(m))
        return; /* back to the WORLD focus */
    m->sel.round = round;
    m->round = round;
    m->focus = 1;
}

int od_scene_map_world_limit(const od_scene_map *m)
{
    return od_scene_map_castle_open(m) ? OD_MAP_WORLD_COUNT
                                        : OD_MAP_WORLD_COUNT - 1;
}

/* With the pressed directions: an advance bit moves one world on, kept below
 * the world limit (else clamped to limit - 1, which then still runs the retreat
 * test: RIGHT on world 9 is world 8, reference f1960); a retreat bit moves one
 * world back, never below world 1. Without a match the world is left alone. */
static int map_world_step(const od_scene_map *m, unsigned dir, const od_scene_data *data)
{
    int x = m->world;
    int limit = od_scene_map_world_limit(m);
    if (dir & data->map_world_up[x]) {
        ++x;
        if (x < limit)
            return x;
        x = limit - 1;
    }
    if (dir & data->map_world_down[x])
        return x > 0 ? x - 1 : 0;
    return m->world;
}

od_map_event od_scene_map_tick(od_scene_map *m, unsigned buttons)
{
    const od_scene_data *data = od_scene_data_require();
    if (!data) return OD_MAP_EVENT_DATA_ERROR;
    unsigned pressed = buttons & ~m->prev_buttons;
    m->prev_buttons = buttons;

    if (m->age < m->ready) {
        ++m->age;
        return OD_MAP_EVENT_NONE;
    }
    ++m->age;

    if (m->focus == 0) {
        /* WORLD focus: `A` selects the round the world resumes at and moves to the
         * round focus; `B` leaves the map for the menu (reference f1800 -> "Where to
         * begin from?"); otherwise the pressed directions walk the world. A5: the step
         * is a press edge, bounded by the world limit, and the entered level is always
         * validated (od_scene_round_level). */
        if (pressed & OD_BTN_A) {
            m->sel.round = map_round_on_entry(m);
            m->round = m->sel.round;
            m->focus = 1;
            return OD_MAP_EVENT_NONE;
        }
        if (pressed & OD_BTN_B)
            return OD_MAP_EVENT_MENU;
        unsigned dir = map_direction(pressed);
        if (dir)
            m->world = map_world_step(m, dir, data);
    } else {
        /* ROUND focus: `A` enters the selected round, `B` returns to the world,
         * LEFT/RIGHT move the round on the press edge (A7). */
        if (pressed & OD_BTN_A) {
            if (od_scene_map_level(m) >= 0)
                return OD_MAP_EVENT_ENTER;
        } else if (pressed & OD_BTN_B) {
            od_scene_map_focus_world(m);
        } else {
            /* RIGHT stops at the last round (9 rounds, or 10 with the castle latch);
             * LEFT stops at round 1. */
            unsigned dir = map_direction(pressed);
            int last = map_castle_latch(m) ? OD_MAP_ROUND_COUNT - 1
                                           : OD_MAP_ROUND_COUNT - 2;
            if ((dir & 0x01u) && m->sel.round >= last)
                dir &= ~0x01u;
            if (od_scene_round_move(&m->sel, dir))
                m->round = m->sel.round;
        }
    }
    return OD_MAP_EVENT_NONE;
}

int od_scene_map_round_cleared(const od_scene_map *m, int world, int round)
{
    if (world < 0 || world >= OD_MAP_WORLD_COUNT)
        return 0;
    if (round < 0 || round >= OD_MAP_ROUND_COUNT)
        return 0;
    return (m->words[world] >> round) & 1u;
}

int od_scene_map_level(const od_scene_map *m)
{
    return od_scene_round_level(m->world, m->sel.round);
}

int od_scene_map_cursor_round(const od_scene_map *m)
{
    if (m->focus == 1)
        return m->sel.round;
    return od_scene_round_first(m->words, m->world);
}

/* ---- panel overlay ------------------------------------------------------ */

/* Attribute byte and 2-bit field of round `i` (1..9) in the panel. */
static void marker_field(int i, unsigned *byte, unsigned *shift)
{
    unsigned col = MAP_MARKER_COL0 + 2u * (unsigned)(i - 1);
    *byte = MAP_ATTR_BASE + (MAP_MARKER_ROW0 / 4u) * 8u + col / 4u;
    *shift = col & 2u;
}

void od_scene_map_panel(const od_scene_map *m, uint16_t *display,
                         unsigned display_words)
{
    if (!display || display_words < MAP_SPRITE_BASE + MAP_SPRITE_MAX * 4u)
        return;

    /* Progress marks: a cleared round is blue (palette 1), an uncleared one red
     * (palette 3), the same colours the reference paints in the panel. */
    for (int i = 1; i <= OD_MAP_ROUND_COUNT - 1; ++i) {
        unsigned byte, shift;
        marker_field(i, &byte, &shift);
        unsigned pal = od_scene_map_round_cleared(m, m->world, i - 1)
                     ? MAP_PAL_CLEARED : MAP_PAL_UNCLEARED;
        display[byte] = (uint16_t)((display[byte] & ~(3u << shift)) |
                                   (pal << shift));
    }

    /* WORLD n: the number is a panel sprite; place one glyph per digit. The
     * extracted layout already carries the world-1 sprite, which is reused. */
    int number = m->world + 1;
    char text[4];
    int len = 0;
    if (number >= 10)
        text[len++] = (char)('0' + number / 10);
    text[len++] = (char)('0' + number % 10);
    text[len] = '\0';

    unsigned count = display[MAP_SPRITE_BASE];
    if (count > MAP_SPRITE_MAX)
        count = MAP_SPRITE_MAX;
    int slot = -1;
    for (unsigned i = 0; i < count; ++i) {
        uint16_t *sp = &display[MAP_SPRITE_BASE + 1u + i * 4u];
        if (sp[0] == OD_SCENE_MAP_WORLD_NUM_X && sp[1] == OD_SCENE_MAP_WORLD_NUM_Y) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0 && count < MAP_SPRITE_MAX) {
        slot = (int)count++;
        display[MAP_SPRITE_BASE] = (uint16_t)count;
        uint16_t *sp = &display[MAP_SPRITE_BASE + 1u + (unsigned)slot * 4u];
        sp[0] = OD_SCENE_MAP_WORLD_NUM_X;
        sp[1] = OD_SCENE_MAP_WORLD_NUM_Y;
        sp[3] = 1;
    }
    if (slot < 0)
        return;

    for (int d = 0; d < len; ++d) {
        int s = slot + d;
        if (s >= (int)count) {
            if (count >= MAP_SPRITE_MAX)
                break;
            s = (int)count++;
            display[MAP_SPRITE_BASE] = (uint16_t)count;
            uint16_t *sp = &display[MAP_SPRITE_BASE + 1u + (unsigned)s * 4u];
            sp[0] = (uint16_t)(OD_SCENE_MAP_WORLD_NUM_X + d * 8);
            sp[1] = OD_SCENE_MAP_WORLD_NUM_Y;
            sp[3] = 1;
        }
        uint16_t *sp = &display[MAP_SPRITE_BASE + 1u + (unsigned)s * 4u];
        sp[0] = (uint16_t)(OD_SCENE_MAP_WORLD_NUM_X + d * 8);
        sp[1] = OD_SCENE_MAP_WORLD_NUM_Y;
        sp[2] = (uint16_t)(OD_SCENE_MAP_DIGIT_TILE + (unsigned)(text[d] - '0'));
        sp[3] = 1;
    }
}

/* ---- Hero cursor -------------------------------------------------------- */

/* The cursor Hero is the original's two 2x2 sprite layers (measured from the
 * reference OAM at f2200: tiles 0x1e/0x1f/0x34/0x35 on sprite palette 1 and
 * 0x4a/0x4b/0x60/0x63 on sprite palette 2, at x=28+16*round, screen y=185).
 * The panel band resolves its sprite tiles through tile bank 5504 (the high
 * word of the extracted map layout's band field). The green body is drawn
 * first, the outline/skin layer on top, as the OAM priority does. */
#define MAP_SPRITE_BANK 5504u

static void draw_layer(od_index_frame *fb, int dx, int dy,
                        const uint8_t tiles[4][2], const uint8_t *chr,
                        const od_palette_ram *pal)
{
    for (int k = 0; k < 4; ++k) {
        const uint8_t *t = chr + (size_t)(MAP_SPRITE_BANK + tiles[k][0]) * OD_TILE_BYTES;
        unsigned attr = tiles[k][1], base = 16u + (attr & 3u) * 4u;
        int cx = k & 1, cy = k >> 1;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                uint8_t v = od_tile_pixel(t, attr & 0x40 ? 7 - x : x,
                                            attr & 0x80 ? 7 - y : y);
                if (!v)
                    continue;
                int px = dx + cx * 8 + x, py = dy + cy * 8 + y;
                if (px < 0 || px >= OD_SCREEN_W || py < 0 || py >= OD_SCREEN_H)
                    continue;
                fb->index[py * OD_SCREEN_W + px] = pal->ram[base + v];
            }
    }
}

void od_scene_map_draw_cursor(const od_scene_map *m, od_index_frame *fb,
                               const uint8_t *chr, const od_palette_ram *pal)
{
    const od_scene_data *data = od_scene_data_require();
    if (!data || !m || !fb || !chr || !pal) return;
    int cursor = od_scene_map_cursor_round(m);
    if (cursor < 0 || cursor >= OD_MAP_ROUND_COUNT)
        return;
    int dx = 28 + 16 * cursor;   /* the reference OAM x of round 1 is 28 */
    int dy = 185;
    draw_layer(fb, dx, dy, data->cursor_body, chr, pal);
    draw_layer(fb, dx, dy, data->cursor_skin, chr, pal);
}

/* ---- castle overlay and landmark sprites ------------------------------- */

int od_scene_map_castle_open(const od_scene_map *m)
{
    /* The castle opening runs one script while the high byte of world 9's word
     * has bit 7 clear; otherwise the world limit becomes 10 and the other script
     * runs. */
    return (m->words[8] & 0x8000u) != 0;
}

void od_scene_map_castle(const od_scene_map *m, const od_map_art *art,
                          uint16_t *display, unsigned display_words)
{
    if (!art || !art->loaded || !display || !od_scene_map_castle_open(m))
        return;
    for (unsigned i = 0; i < art->overlay_count; ++i) {
        unsigned field = 32u + art->overlay_index[i];   /* palette, then CIRAM */
        if (field < display_words)
            display[field] = art->overlay_value[i];
    }
    /* The background banks follow the CIRAM in the display (screens.c
     * compose_display: words 2080..2083 = four bank values * 64). */
    if (display_words > MAP_BG_BANK_BASE + 3u)
        for (unsigned k = 0; k < 4; ++k)
            display[MAP_BG_BANK_BASE + k] = (uint16_t)(art->castle_banks[k] * 64u);
}

/* Which animation objects this frame draws: object 0 only while WORLD 10 is
 * shut, object 1 always, object 2 while the world's castle is open but not
 * cleared (bit 6 set, bit 7 clear). */
static int map_object_drawn(const od_scene_map *m, int k)
{
    unsigned word = m->words[m->world];
    if (k == 0)
        return !od_scene_map_castle_open(m);
    if (k == 2)
        return (word & 0x4000u) && !(word & 0x8000u);
    return 1;
}

/* Show the current frame for its duration, then the next one; a zero duration
 * ends the list and restarts it. The first call after the init (timer 0) loads
 * frame 0. */
static void map_object_step(uint8_t *timer, uint8_t *frame, const od_map_object *o)
{
    if (!o->frame_count)
        return;
    unsigned idx = *frame;
    if (*timer) {
        if (--*timer)
            return;
        ++idx;
    }
    if (idx >= o->frame_count)
        idx = 0;
    *frame = (uint8_t)idx;
    *timer = o->frames[idx].duration;
}

int od_scene_map_step_sprites(od_scene_map *m, const od_map_art *art,
                               unsigned frame_tick)
{
    int colour_step = 0;
    if (!art || !art->loaded)
        return 0;
    ++m->loop_calls;
    /* Hero's pose advances on the frames whose animation counter is a multiple
     * of 8 (the counter is incremented before the frame's main loop runs). R1:
     * counter 192 at f4042, 200 at f4050; the menu route 240 at f1527; the 10-10
     * password route 96 at f11623. */
    if ((frame_tick & 7u) == 0) {
        m->hero_pose = (m->hero_pose + 1u) & 3u;
        /* The colour step runs on the same frames: the frame's colour (its palette entry)
         * takes the next table entry (R1: first colour at the palette load f4036,
         * second at f4042, third at f4050...). */
        ++m->frame_pal_steps;
        colour_step = 1;
    }
    for (int k = 0; k < OD_MAP_OBJECTS; ++k)
        if (map_object_drawn(m, k))
            map_object_step(&m->obj_timer[k], &m->obj_frame[k], &art->objects[k]);
    return colour_step;
}

typedef struct {
    unsigned count;
    uint16_t sprite[64][4];      /* x, screen row (OAM y + 1), tile, attr */
} map_oam;

/* Each sprite is stored in turn; the OAM fills downwards, so the last sprite
 * the frame writes ends up first in OAM order. */
static void map_put(map_oam *o, unsigned y, unsigned tile, unsigned attr, unsigned x)
{
    if (o->count >= 64)
        return;
    uint16_t *s = o->sprite[o->count++];
    s[0] = (uint16_t)(x & 0xffu);
    s[1] = (uint16_t)((y & 0xffu) + 1u);
    s[2] = (uint16_t)tile;
    s[3] = (uint16_t)attr;
}

static void map_put_list(map_oam *o, unsigned by, unsigned bx,
                         const od_map_sprite *list, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        map_put(o, by + list[i].dy, list[i].tile, list[i].attr, bx + list[i].dx);
}

static void map_put_object(map_oam *o, const od_scene_map *m,
                           const od_map_art *art, int k)
{
    const od_map_object *ob = &art->objects[k];
    if (!ob->frame_count)
        return;
    const od_map_frame *fr = &ob->frames[m->obj_frame[k] % ob->frame_count];
    map_put_list(o, ob->y, ob->x, fr->sprites, fr->count);
}

void od_scene_map_sprites(const od_scene_map *m, const od_map_art *art,
                           unsigned frame_cycle, uint16_t *display,
                           unsigned display_words)
{
    if (!art || !art->loaded || !display ||
        display_words < MAP_SPRITE_BASE + 1u + MAP_SPRITE_MAX * 4u)
        return;
    map_oam o;
    o.count = 0;
    /* The frame's colour: the palette's own entry (the table's third colour) until this visit's first
     * colour step; each step advances the colour counter and takes table[(counter
     * + 2) % 3] (1 -> first, 2 -> second, 0 -> third). The map does not reset the
     * counter: the menu route's first visit starts from the title's 2 (reference
     * 02 -> 00 at f1527, third colour at f1528), R1's second visit from 0 (01 at
     * f4042, first colour). */
    display[art->frame_pal_entry] =
        m->frame_pal_steps ? art->frame_pal_colors[(frame_cycle + 2u) % 3u]
                           : art->frame_pal_colors[2];
    if (m->loop_calls) {
        int w = m->world;
        /* Hero's eight sprites at the cursor position: the world landmark in WORLD
         * focus, the panel round in ROUND focus. */
        unsigned dx = m->focus == 1 ? art->panel_x[m->sel.round % OD_MAP_ROUND_COUNT]
                                    : art->world_x[w];
        unsigned dy = m->focus == 1 ? art->panel_y : art->world_y[w];
        const uint8_t (*pose)[2] = art->poses[m->hero_pose & 3u];
        for (unsigned i = 0; i < 8; ++i)
            map_put(&o, dy - ((i & 2u) ? 0u : 8u), pose[i][0], pose[i][1],
                    dx - ((i & 1u) ? 0u : 8u));
        /* The frame around the landmark: four corners of one tile, mirrored. */
        unsigned fx = art->world_x[w], fy = art->world_y[w];
        unsigned a = art->frame_attr;
        map_put(&o, fy - 16u, art->frame_tile, a, fx - 16u);
        map_put(&o, fy - 16u, art->frame_tile, a ^ 0x40u, fx + 8u);
        map_put(&o, fy + 8u, art->frame_tile, a ^ 0x80u, fx - 16u);
        map_put(&o, fy + 8u, art->frame_tile, a ^ 0xc0u, fx + 8u);
        /* The WORLD number, two slots on palette 1 (a zero tile leaves its slot out). */
        int number = w + 1;
        unsigned d0 = OD_SCENE_MAP_DIGIT_TILE +
                      (unsigned)(number >= 10 ? number / 10 : number);
        map_put(&o, art->digit_y, d0, 1u, art->digit_x);
        if (number >= 10)
            map_put(&o, art->digit_y, OD_SCENE_MAP_DIGIT_TILE + (unsigned)(number % 10),
                    1u, art->digit_x + 8u);
        /* the objects and the panel castle. */
        for (int k = 0; k < 2; ++k)
            if (map_object_drawn(m, k))
                map_put_object(&o, m, art, k);
        if (m->words[w] & 0x4000u) {
            map_put_list(&o, art->castle_y, art->castle_x, art->castle,
                         art->castle_count);
            if (map_object_drawn(m, 2))
                map_put_object(&o, m, art, 2);
        }
    }
    display[MAP_SPRITE_BASE] = (uint16_t)o.count;
    for (unsigned i = 0; i < o.count; ++i) {
        const uint16_t *s = o.sprite[o.count - 1u - i];
        for (unsigned k = 0; k < 4; ++k)
            display[MAP_SPRITE_BASE + 1u + i * 4u + k] = s[k];
    }
}
