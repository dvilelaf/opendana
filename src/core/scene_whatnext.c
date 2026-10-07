/* scene_whatnext.c - the "What next?" menu as logic (C3.3).
 *
 * The extracted screen is frozen as a maqueta (assets/screens/whatnext.track,
 * reference f11800); this module owns the dynamic part: the three-option cursor
 * and the press-edge navigation. The layout, the text and the sprite order come
 * from the reference, never from C. */
#include "opendana/scene_whatnext.h"
#include "opendana/game.h"

/* Option rows in the extracted nametable (the first menu uses the same rows,
 * see `OD_SCREEN_MENU` in screens.c). The cursor is the arrow tile at
 * column 17, one column left of the option text. */
#define WHATNEXT_COL 17u
#define WHATNEXT_ROW0 19u
#define WHATNEXT_ROW_STEP 2u
#define WHATNEXT_CURSOR_TILE 0xfeu

/* Layout offsets inside the OD_SCENE_WORDS display vector: 32 palette bytes,
 * then the 32x30 nametable. */
#define WHATNEXT_NT_BASE 32u

void od_scene_whatnext_reset(od_scene_whatnext *w)
{
    w->selection = w->visible_selection = 0;
    w->previous_buttons = 0;
    w->age = 0;
    w->leaving = 0;
    w->pending = OD_WHATNEXT_NONE;
}

void od_scene_whatnext_begin(od_scene_whatnext *w)
{
    od_scene_whatnext_reset(w);
}

od_whatnext_event od_scene_whatnext_tick(od_scene_whatnext *w,
                                           unsigned buttons)
{
    /* First tick after the scene was entered: a button that was already held
     * when the previous screen left (the password's END is held for several
     * ticks in the scripted routes) is not a fresh press. The original is not
     * even showing the menu yet at that point, so no event is consumed. */
    if (w->age == 0) {
        w->previous_buttons = buttons;
        w->visible_selection = w->selection;
        ++w->age;
        return OD_WHATNEXT_NONE;
    }
    /* Menu-close hold: the confirm is latched and delivered a measured number
     * of ticks later, so the settled screen stays for the same window as the
     * reference (the A is read at f12200; the map transition starts at f12275). */
    if (w->leaving) {
        w->visible_selection = w->selection;
        if (--w->leaving == 0)
            return w->pending;
        return OD_WHATNEXT_NONE;
    }
    /* Publish the previous selection first: the original's PPU write shows one
     * tick late, exactly like the first menu (screens.c) and the retry menu. */
    w->visible_selection = w->selection;
    unsigned pressed = buttons & ~w->previous_buttons;
    w->previous_buttons = buttons;
    ++w->age;
    /* DOWN then UP, wrap modulo 3; START is not confirm. */
    if (pressed & OD_BTN_DOWN)
        w->selection = (w->selection + 1) % OD_WHATNEXT_OPTIONS;
    if (pressed & OD_BTN_UP)
        w->selection = (w->selection + OD_WHATNEXT_OPTIONS - 1) %
                       OD_WHATNEXT_OPTIONS;
    if (pressed & OD_BTN_A) {
        switch (w->selection) {
        case 0:  w->pending = OD_WHATNEXT_CONT_STORY; break;
        case 1:  w->pending = OD_WHATNEXT_QUIT; break;
        default: w->pending = OD_WHATNEXT_EDIT_MODE; break;
        }
        w->leaving = OD_WHATNEXT_EXIT_TICKS;
    }
    return OD_WHATNEXT_NONE;
}

unsigned od_scene_whatnext_row(unsigned option)
{
    return WHATNEXT_ROW0 + WHATNEXT_ROW_STEP * (option % OD_WHATNEXT_OPTIONS);
}

/* ---- the hearth illustration (WHATNEXT) --------------------------------- */

/* Display vector layout (screens.c compose_display): background banks at
 * 2080..2083 (four bank values x 64), sprite count at 2124, then x, row, tile, attr. */
#define HEARTH_BG_BANK_BASE 2080u
#define HEARTH_SPRITE_BASE  2124u
#define HEARTH_OAM_SLOTS    64u

/* The draw loop runs 17 frames from the sixth frame of the menu mode; the menu
 * text is then uploaded without calling the fire animation. */
#define HEARTH_FIRST_FRAME 6
#define HEARTH_DRAW_FRAMES 17

void od_hearth_reset(od_hearth *h)
{
    if (h)
        *h = (od_hearth){0};
}

void od_hearth_call(od_hearth *h, const od_hearth_art *art, int confirmed)
{
    if (!h || !art || !art->loaded)
        return;
    /* Fire animation, object 3: a running frame counts down and the next one
     * starts when it expires (a zero duration restarts the list); a zero timer
     * (the first call after the clear) loads the current frame. */
    const od_map_object *o = &art->fire;
    if (o->frame_count) {
        unsigned idx = h->fire_frame;
        if (h->fire_timer && --h->fire_timer == 0)
            ++idx;
        if (!h->fire_timer) {
            if (idx >= o->frame_count)
                idx = 0;
            h->fire_frame = (uint8_t)idx;
            h->fire_timer = o->frames[idx].duration;
        }
    }
    /* The fire's value counter counts 16 frames; a zero counter loads 16 and the
     * first value, an expiring one reloads 16 and swaps the value. */
    if (!h->bank_timer) {
        h->bank_timer = art->bank_period;
        h->bank_value = art->bank_values[0];
    } else if (--h->bank_timer == 0) {
        h->bank_timer = art->bank_period;
        h->bank_value = h->bank_value == art->bank_values[0] ? art->bank_values[1]
                                                             : art->bank_values[0];
    }
    /* Slot writes use the current fill mode, which then advances. */
    h->drawn_rotation = h->rotation;
    h->rotation = (uint8_t)((h->rotation + 1u) & 3u);
    h->drawn_confirmed = confirmed ? 1u : 0u;
    h->calls = 1;
}

/* With no reserved slots: mode 0 fills slots 0.. forward, mode 1 backward from
 * 63, mode 2 forward from 32 (wrapping), mode 3 backward from 31 (wrapping).
 * Unused slots keep a Y below the screen. */
static void hearth_put(uint16_t slots[HEARTH_OAM_SLOTS][4], unsigned *cursor,
                       unsigned mode, unsigned y, unsigned tile, unsigned attr,
                       unsigned x)
{
    unsigned slot;
    if (mode & 1u) {
        if (*cursor == 0)
            *cursor = HEARTH_OAM_SLOTS;
        slot = --*cursor;
    } else {
        slot = (*cursor)++;
        if (*cursor == HEARTH_OAM_SLOTS)
            *cursor = 0;
    }
    slots[slot][0] = (uint16_t)(x & 0xffu);
    slots[slot][1] = (uint16_t)((y & 0xffu) + 1u); /* screen row = OAM y + 1 */
    slots[slot][2] = (uint16_t)tile;
    slots[slot][3] = (uint16_t)attr;
}

void od_hearth_patch(const od_hearth *h, const od_hearth_art *art,
                      uint16_t *display, unsigned display_words)
{
    if (!h || !h->calls || !art || !art->loaded || !display ||
        display_words < HEARTH_SPRITE_BASE + 1u + HEARTH_OAM_SLOTS * 4u)
        return;
    uint16_t slots[HEARTH_OAM_SLOTS][4];
    for (unsigned i = 0; i < HEARTH_OAM_SLOTS; ++i) {
        slots[i][0] = 0;
        slots[i][1] = 0xf0u + 1u;
        slots[i][2] = slots[i][3] = 0;
    }
    unsigned mode = h->drawn_rotation & 3u;
    unsigned cursor = (mode & 2u) ? HEARTH_OAM_SLOTS / 2u : 0u;
    /* the groups of the list, in order. */
    unsigned list = h->drawn_confirmed ? 1u : 0u;
    for (unsigned g = 0; g < art->group_count[list]; ++g) {
        const od_hearth_group *gr = &art->groups[list][g];
        for (unsigned i = 0; i < gr->count; ++i) {
            const od_map_sprite *s = &gr->sprites[i];
            hearth_put(slots, &cursor, mode, gr->y + s->dy, s->tile, s->attr,
                       gr->x + s->dx);
        }
    }
    /* the fire's current frame. */
    if (art->fire.frame_count) {
        const od_map_frame *fr = &art->fire.frames[h->fire_frame % art->fire.frame_count];
        for (unsigned i = 0; i < fr->count; ++i)
            hearth_put(slots, &cursor, mode, art->fire.y + fr->sprites[i].dy,
                       fr->sprites[i].tile, fr->sprites[i].attr,
                       art->fire.x + fr->sprites[i].dx);
    }
    display[HEARTH_SPRITE_BASE] = HEARTH_OAM_SLOTS;
    for (unsigned i = 0; i < HEARTH_OAM_SLOTS; ++i)
        for (unsigned k = 0; k < 4; ++k)
            display[HEARTH_SPRITE_BASE + 1u + i * 4u + k] = slots[i][k];
    /* the fire publishes its value; the next frame's display picks it up. */
    if (h->bank_value && art->bank_slot < 4)
        display[HEARTH_BG_BANK_BASE + art->bank_slot] = (uint16_t)(h->bank_value * 64u);
}

int od_hearth_menu_frame(int frame, unsigned text_frames)
{
    return frame >= HEARTH_FIRST_FRAME &&
           (frame < HEARTH_FIRST_FRAME + HEARTH_DRAW_FRAMES ||
            frame >= HEARTH_FIRST_FRAME + HEARTH_DRAW_FRAMES + (int)text_frames);
}

void od_scene_whatnext_panel(const od_scene_whatnext *w, uint16_t *display,
                              unsigned display_words)
{
    if (!display)
        return;
    for (unsigned opt = 0; opt < OD_WHATNEXT_OPTIONS; ++opt) {
        unsigned idx = WHATNEXT_NT_BASE + od_scene_whatnext_row(opt) * 32u +
                       WHATNEXT_COL;
        if (idx >= display_words)
            continue;
        display[idx] = opt == (w->visible_selection % OD_WHATNEXT_OPTIONS)
                     ? WHATNEXT_CURSOR_TILE : 0u;
    }
}

/* ---- the menu's entry (fade in) --------------------------------------- */

int od_menu_entry_brightness(int frame)
{
    /* The first call (frame 6) zeroes the brightness and the driver raises it the
     * same frame; the screen shows the brightness of the frame before. */
    if (frame <= OD_MENU_FADE_FIRST)
        return 0;
    if (frame - OD_MENU_FADE_FIRST >= 15)
        return 15;
    return frame - OD_MENU_FADE_FIRST;
}

/* A record enters while its byte count plus the queue length stays below
 * `capacity`; the queue grows by the 3-byte header and the bytes, and each CPU
 * frame starts with the queue the previous display pass emptied. Returns the
 * records taken in the first `cpu_frames` CPU frames (all of them when
 * negative) and stores how many CPU frames the whole layout takes. */
static unsigned layout_taken(const od_menu_layout *l, unsigned capacity,
                             int cpu_frames, int *frames_needed)
{
    unsigned taken = 0, queued = 0;
    int frame = 0;
    while (taken < l->record_count) {
        unsigned n = l->records[taken].count & 0x3Fu;
        if (n + queued >= capacity) {
            if (queued == 0)
                break; /* a record the queue can never take */
            ++frame;   /* wait for the next frame */
            queued = 0;
            continue;
        }
        if (cpu_frames >= 0 && frame >= cpu_frames)
            break;
        queued += 3u + n;
        ++taken;
    }
    if (frames_needed)
        *frames_needed = frame + 1;
    return taken;
}

unsigned od_menu_layout_visible(const od_menu_layout *layout,
                                 unsigned capacity, int frame)
{
    if (!layout || !capacity || frame <= OD_MENU_LAYOUT_FRAME)
        return 0;
    return layout_taken(layout, capacity, frame - OD_MENU_LAYOUT_FRAME, NULL);
}

static void menu_record(uint16_t *display, unsigned display_words,
                        const od_menu_record *r, int blank)
{
    unsigned step = (r->count & 0x80u) ? 32u : 1u;
    unsigned addr = r->addr;
    for (unsigned i = 0; i < (r->count & 0x3Fu); ++i, addr += step) {
        if (addr < 0x2000u || addr >= 0x2400u)
            continue;
        /* The attribute bytes are already the layout's when it starts (the reference
         * draws the box edges of f11783 in their final palette before the attribute
         * record lands on f11785): only the tiles are blank. */
        if (blank && addr >= 0x23C0u)
            continue;
        unsigned idx = WHATNEXT_NT_BASE + (addr - 0x2000u);
        if (idx < display_words)
            display[idx] = blank ? 0u : r->data[i];
    }
}

int od_menu_layout_draw(const od_menu_layout *layout, unsigned capacity,
                         int frame, uint16_t *display, unsigned display_words)
{
    if (!layout || !capacity || !display || !layout->record_count)
        return 0;
    int needed = 0;
    (void)layout_taken(layout, capacity, -1, &needed);
    /* The option cursor is written by the loop's first frame after the
     * layout, so the frame that shows the last records still has bare rows. */
    if (frame > OD_MENU_LAYOUT_FRAME + needed)
        return 0;
    unsigned shown = od_menu_layout_visible(layout, capacity, frame);
    for (unsigned i = 0; i < layout->record_count; ++i)
        menu_record(display, display_words, &layout->records[i], i >= shown);
    return 1;
}

int od_hearth_screen(const od_hearth_art *art, uint16_t *display,
                      unsigned display_words)
{
    if (!art || !art->has_screen || !display ||
        display_words < WHATNEXT_NT_BASE + 2048u)
        return 0;
    for (unsigned i = 0; i < 32u; ++i)
        display[i] = art->palette[i];
    for (unsigned r = 0; r < 16u; ++r)
        for (unsigned c = 0; c < 16u; ++c) {
            unsigned y = art->illus_y + r, x = art->illus_x + c;
            if (y < 30u && x < 32u)
                display[WHATNEXT_NT_BASE + y * 32u + x] = art->illus[r][c];
        }
    /* Both nametables' attribute tables start filled; the illustration's
     * own 4x4 block goes into the first one. */
    for (unsigned nt = 0; nt < 2u; ++nt)
        for (unsigned i = 0; i < 64u; ++i)
            display[WHATNEXT_NT_BASE + nt * 1024u + 960u + i] = art->attr_fill;
    for (unsigned r = 0; r < 4u; ++r)
        for (unsigned c = 0; c < 4u; ++c) {
            unsigned y = art->attr_y + r, x = art->attr_x + c;
            if (y < 8u && x < 8u)
                display[WHATNEXT_NT_BASE + 960u + y * 8u + x] = art->attr[r][c];
        }
    for (unsigned i = 0; i < art->ppu_count; ++i)
        if (art->ppu_field[i] < display_words)
            display[art->ppu_field[i]] = art->ppu_value[i];
    /* The settled panel: every record of the layout written. */
    const od_menu_layout *l = &art->layouts[OD_MENU_LAYOUT_WHATNEXT];
    for (unsigned i = 0; i < l->record_count; ++i)
        menu_record(display, display_words, &l->records[i], 0);
    return 1;
}

void od_menu_record_write(const od_menu_record *r, uint16_t *display,
                           unsigned display_words)
{
    if (r && display)
        menu_record(display, display_words, r, 0);
}
