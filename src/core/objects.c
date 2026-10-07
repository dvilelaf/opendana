#include "opendana/objects.h"

#include "opendana/content_tables.h"
#include "opendana/rom_data.h"

#include <string.h>

/* Boss combat tables (attack script pointers and corridor transforms), loaded
 * from the content package. */

/* Last byte of the extracted bank-6 metasprite blob. */
#define BOSS_POSE_BASE OD_BOSS_POSE_BASE
#define BOSS_POSE_END (BOSS_POSE_BASE + (unsigned)OD_BOSS_POSE_DATA - 1u)

/* Last byte of the imported metasprite blob. */
static const od_object_info catalogue[16] = {
    {"empty", OD_OBJECT_EMPTY, 0, 0},
    {"wall", OD_OBJECT_WALL, 0, 0},
    {"ice-middle", OD_OBJECT_ICE, 1, 0},
    {"ice-left", OD_OBJECT_ICE, 1, 0},
    {"ice-right", OD_OBJECT_ICE, 1, 0},
    {"ice-single", OD_OBJECT_ICE, 1, 0},
    {"pipe-vertical", OD_OBJECT_PIPE, 0, 12},
    {"pipe-horizontal", OD_OBJECT_PIPE, 0, 3},
    {"pipe-08", OD_OBJECT_PIPE, 0, 6},
    {"pipe-09", OD_OBJECT_PIPE, 0, 5},
    {"pipe-0a", OD_OBJECT_PIPE, 0, 10},
    {"pipe-0b", OD_OBJECT_PIPE, 0, 9},
    {"stone", OD_OBJECT_STONE, 0, 0},
    {"reactive-0d", OD_OBJECT_REACTIVE, 0, 0},
    {"fire", OD_OBJECT_FIRE, 0, 0},
    {"hazard-0f", OD_OBJECT_HAZARD, 0, 0}
};
static const od_object_info scroll = {"scroll-command", OD_OBJECT_SCROLL, 0, 0};
static const od_object_info flames[] = {
    {"horizontal-flame", OD_OBJECT_FLAME, 0, 0},
    {"vertical-flame", OD_OBJECT_FLAME, 0, 0},
    {"resistant-vertical-flame", OD_OBJECT_FLAME, 0, 0}
};

const od_object_info *od_object_type(uint8_t type)
{
    if (type < 16) return &catalogue[type];
    if (type == 0x80) return &scroll;
    if (type >= 0x81 && type <= 0x83) return &flames[type - 0x81];
    return NULL;
}

od_status od_object_scroll_tick(od_object_scroll *s, uint8_t game_flags,
                                 uint8_t *sound)
{
    if (!s || !sound) return OD_ERR_FORMAT;
    *sound = 0;
    if (s->enabled & 0x80) ++s->frame_counter;
    if (game_flags & 0x0a) return OD_OK;
    switch (s->state[0] & 3) {
    case 0:
        if (!(s->enabled & 0x80)) return OD_OK;
        s->state[0] ^= 0x80;
        if (s->state[0] & 0x80) return OD_OK;
        if (s->state[1] && --s->state[1]) return OD_OK;
        s->state[0] = 1;
        *sound = 0x46;
        return OD_OK;
    case 1:
        if (s->state[1] && --s->state[1]) return OD_OK;
        s->state[3] = (uint8_t)(s->state[3] - 1);
        if (s->state[3] >= 0xf0) s->state[3] -= 0x10;
        s->scroll_y = s->state[3];
        if (!(s->scroll_y & 15)) {
            s->state[1] = s->interval;
            s->state[0] = 0;
        } else {
            s->state[1] = 1;
        }
        return OD_OK;
    case 2:
    case 3:
        return OD_OK; /* original explicit RTS dispatch entries */
    }
    return OD_ERR_FORMAT;
}

/* Boss sequence tags. The object state stores the running sequence as a
 * two-byte tag (low, high); the data for each sequence comes from the imported
 * engine data. The tag values are part of the boss object state that is compared
 * per tick, so they are kept as opaque constants. */
enum { BOSS_SEQ_IDLE, BOSS_SEQ_HIT, BOSS_SEQ_FINAL, BOSS_SEQ_COUNT };
static const uint8_t boss_seq_tag[BOSS_SEQ_COUNT][2] = {
    {0xad, 0x9c}, {0xdf, 0x9c}, {0xea, 0x9c}
};

static void boss_sequence_start(od_boss *b, unsigned base, unsigned sequence)
{
    b->state[base] = 0;
    b->state[base + 1] = boss_seq_tag[sequence][0];
    b->state[base + 2] = boss_seq_tag[sequence][1];
    b->state[base + 3] = 0;
}

/* Run one step of a boss sequence, restricted to the three main-controller
 * sequences above. Returns 1 at the end marker, 0 while waiting, -1 for
 * unsupported state or data. */
static int boss_sequence_tick(od_boss *b, unsigned base)
{
    const od_rom_data *tables = od_rom_data_get();
    if (!tables) return -1;
    uint8_t *s = b->state + base;
    if (s[0] && --s[0]) return 0;
    const uint8_t *data = NULL;
    unsigned size = 0;
    for (unsigned q = 0; q < BOSS_SEQ_COUNT; ++q) {
        if (s[1] != boss_seq_tag[q][0] || s[2] != boss_seq_tag[q][1]) continue;
        if (q == BOSS_SEQ_IDLE) { data = tables->boss_idle; size = sizeof tables->boss_idle; }
        else if (q == BOSS_SEQ_HIT) { data = tables->boss_hit; size = sizeof tables->boss_hit; }
        else { data = tables->boss_defeat; size = sizeof tables->boss_defeat; }
    }
    if (!data) return -1;
    unsigned cursor = s[3];
    for (unsigned budget = 0; budget < 64 && cursor < size; ++budget) {
        unsigned op = data[cursor++];
        if (op < 0x80) {
            s[0] = (uint8_t)op;
            s[3] = (uint8_t)cursor;
            return 0;
        }
        if (op < 0xf0) {
            if (cursor + 2 > size || data[cursor + 1] >= 8) return -1;
            unsigned low = data[cursor++], slot = data[cursor++];
            b->poses[slot] = (uint16_t)((op << 8) | low);
        } else if (op == 0xff) {
            for (unsigned i = 0; i < 4; ++i) s[i] = 0;
            return 1;
        } else if (op == 0xfe) {
            if (cursor + 2 > size || data[cursor] != boss_seq_tag[BOSS_SEQ_IDLE][0] ||
                data[cursor + 1] != boss_seq_tag[BOSS_SEQ_IDLE][1])
                return -1;
            data = tables->boss_idle;
            size = sizeof tables->boss_idle;
            s[1] = boss_seq_tag[BOSS_SEQ_IDLE][0];
            s[2] = boss_seq_tag[BOSS_SEQ_IDLE][1];
            cursor = 0;
        } else if (op == 0xfb || op == 0xfa) {
            if (cursor >= size) return -1;
            if (op == 0xfb) b->spawn_attack = data[cursor++];
            else b->sound = data[cursor++];
        } else return -1;
    }
    return -1;
}

static void boss_accept_hit(od_boss *b)
{
    b->state[0] = 2;
    b->state[1] = 0;
    boss_sequence_start(b, 6, 1);
    if ((unsigned)b->state[10] + 1 >= 5) {
        b->state[10] = 0; /* the counter resets to zero instead of storing a fifth hit */
        b->state[0] = 3;
        boss_sequence_start(b, 6, 2);
    } else {
        ++b->state[10];
        b->sound = 0x74;
    }
}

od_status od_boss_tick(od_boss *b, uint8_t game_flags, uint8_t attack_active)
{
    if (!b) return OD_ERR_FORMAT;
    if (!od_rom_data_get()) return OD_ERR_IO;
    b->spawn_attack = b->sound = b->defeated = 0;
    if (game_flags & 0x1e) return OD_OK;
    uint8_t hit = b->state[1] & 0x80;
    int done;
    switch (b->state[0] & 7) {
    case 0:
        boss_sequence_start(b, 2, 0);
        done = boss_sequence_tick(b, 2);
        b->state[0] = 1;
        break;
    case 1:
        if (hit) { boss_accept_hit(b); return OD_OK; }
        if (attack_active) return OD_OK;
        done = boss_sequence_tick(b, 2);
        break;
    case 2:
        done = boss_sequence_tick(b, 6);
        if (done == 1) {
            if (b->state[10] >= 5) {
                b->state[10] = 0;
                b->state[0] = 3;
                boss_sequence_start(b, 6, 2);
            } else b->state[0] = 1;
        } else if (done == 0 && hit) boss_accept_hit(b);
        break;
    case 3:
        done = boss_sequence_tick(b, 6);
        if (done == 1) {
            b->state[0] = 0;
            b->defeated = 1;
        }
        break;
    default:
        return OD_ERR_RANGE;
    }
    return done < 0 ? OD_ERR_FORMAT : OD_OK;
}

/* Walk the eight pose words (little-endian) and interpret the metasprite behind
 * each non-zero pointer. A metasprite starts with an anchor x/y plus one unused
 * byte, followed by a stream of control bytes. A byte of 128 or more is an 8x8
 * tile entry (tile = byte, attribute = next byte, position = current x/y, then
 * y advances by 8). A byte below 32 sets y = anchor_y - byte; 32..63 sets
 * x = anchor_x + sign_extended(byte - 32); 64..127 is a command by its low two
 * bits: 0 ends the pose, 1 jumps to a sub-pose pointer, 2 subtracts a following
 * byte from y and 3 adds a following byte to x. The arena has no scroll, so the
 * camera term is a no-op. */
static uint8_t boss_pose_byte(const od_engine_tables *t, unsigned offset)
{
    return offset < OD_BOSS_POSE_DATA ? t->boss_pose_data[offset] : (uint8_t)0;
}

static int boss_pose_walk(const od_boss *b, unsigned slot,
                          od_boss_sprite *out, unsigned capacity,
                          unsigned *written)
{
    unsigned address = b->poses[slot];
    if (address < BOSS_POSE_BASE || address > BOSS_POSE_END) return 0;
    const od_engine_tables *t = od_engine_tables_get();
    if (t == NULL) return 0;
    unsigned base_x = (unsigned)(uint8_t)(t->boss_pose_data[address - BOSS_POSE_BASE]);
    unsigned base_y = boss_pose_byte(t, address - BOSS_POSE_BASE + 1);
    unsigned x = base_x, y = base_y;
    unsigned cursor = 3; /* bytes 0..2 are x, y and an unused byte */
    for (unsigned budget = 0; budget < 1024; ++budget) {
        unsigned offset = address - BOSS_POSE_BASE;
        if (offset + cursor >= OD_BOSS_POSE_DATA) return -1;
        uint8_t control = t->boss_pose_data[offset + cursor++];
        if (control < 0x20) {
            y = (unsigned)(uint8_t)(base_y - control);
            continue;
        }
        if (control < 0x40) {
            int delta = (int)(control - 0x20);
            if (delta & 0x10) delta -= 0x20; /* sign-extend the 5-bit offset */
            x = (unsigned)(uint8_t)(base_x + delta);
            continue;
        }
        if (control < 0x80) {
            switch (control & 3) {
            case 0: /* end of this pose */
                return 1;
            case 1: { /* command 1: switch to a sub-metasprite pointer */
                uint16_t lo = boss_pose_byte(t, offset + cursor++);
                uint16_t hi = boss_pose_byte(t, offset + cursor++);
                address = (unsigned)(lo | (hi << 8));
                if (address < BOSS_POSE_BASE || address > BOSS_POSE_END) return -1;
                cursor = 0;
                break;
            }
            case 2: /* command 2: y = anchor_y - next byte */
                y = (unsigned)(uint8_t)(base_y -
                                        boss_pose_byte(t, offset + cursor++));
                break;
            default: /* command 3: x = anchor_x + next byte */
                x = (unsigned)(uint8_t)(base_x + boss_pose_byte(t, offset + cursor++));
                break;
            }
            continue;
        }
        /* tile entry: emit one 8x8 sprite, then advance a row. */
        if (*written < capacity) {
            uint8_t attr = boss_pose_byte(t, offset + cursor++);
            out[*written].x = (int)x;
            out[*written].y = (int)y;
            out[*written].tile = control;
            out[*written].attr = attr;
            ++*written;
        } else {
            ++cursor;
        }
        y = (y + 8) & 0xFF;
    }
    return 0;
}

unsigned od_boss_pose(od_boss_sprite *out, unsigned capacity,
                       const od_boss *boss)
{
    if (!out || !boss || capacity == 0) return 0;
    unsigned written = 0;
    for (unsigned slot = 0; slot < 8; ++slot)
        boss_pose_walk(boss, slot, out, capacity, &written);
    return written;
}


/* ------------------------------------------------------------------------- */
/* Boss attack controller (object slot 1) */
/* ------------------------------------------------------------------------- */

void od_boss_attack_spawn(od_boss_attack *attack)
{
    if (!attack) return;
    memset(attack, 0, sizeof(*attack));
    attack->alive = 1; /* marks the attack object as registered */
}

void od_boss_attack_graphics(uint8_t cells[256], const od_boss_attack *attack)
{
    memset(cells, 0xff, 256);
    if (!attack || !attack->alive) return;
    const od_engine_tables *t = od_engine_tables_get();
    if (t == NULL) return;
    unsigned count = attack->state[3];
    if (count > 31) count = 31;
    for (unsigned i = 0; i < count; i++) {
        unsigned cursor = t->boss_attack_offset[i];
        while (t->boss_attack_data[cursor] != 0xff) {
            unsigned cell = t->boss_attack_data[cursor++];
            cells[cell] = t->boss_attack_data[cursor++];
        }
    }
}

od_status od_boss_attack_tick(od_boss_attack *a, uint8_t grid[256],
                               uint8_t hero_x, uint8_t hero_y,
                               uint8_t *hero_flags, uint8_t *sound)
{
    if (!a || !grid || !hero_flags || !sound) return OD_ERR_FORMAT;
    *sound = 0;
    if (!a->alive) return OD_OK;
    const od_engine_tables *t = od_engine_tables_get();
    if (t == NULL) return OD_ERR_FORMAT;

    uint8_t *s = a->state;
    if (s[0] == 0) { /* one-shot init: sets phase 1 and requests the attack sound */
        s[0] = 1;
        s[2] = 0;
        s[3] = 0;
        *sound = 0x72;
        return OD_OK;
    }

    /* One script every six ticks. A stored count of 1 falls through, so the period
     * is exactly 6. */
    if (s[2] && --s[2]) return OD_OK;
    s[2] = 6;

    unsigned index = s[3]++;
    /* The last table entry is null: it ends the object. Its support spans
     * (od_boss_attack_retire_spans) are queued by the combat scene. */
    if (index >= 31) {
        memset(s, 0, 4);
        a->alive = 0;
        return OD_OK;
    }
    unsigned cursor = t->boss_attack_offset[index];
    for (;;) {
        uint8_t byte = t->boss_attack_data[cursor++];
        if (byte == 0xFF) { /* is the hero standing on a hazard cell? */
            uint8_t cell = (uint8_t)((hero_y & 0xf0) | (hero_x >> 4));
            if (grid[cell] == 0x10) *hero_flags |= 0x10;
            return OD_OK;
        }
        uint8_t cell = byte;
        uint8_t type = t->boss_attack_data[cursor++];
        if (type == 0) {
            grid[cell] = 0; /* explicit clear */
        } else {
            /* Join the neighbours through the corridor tables, then stamp the hazard.
             * Neighbours already holding a hazard are skipped. */
            uint8_t x = (uint8_t)(cell + 1);
            if (grid[x] != 0x10) grid[x] = t->boss_corridor_right[grid[x] & 0x1f];
            x = (uint8_t)(cell - 1);
            if (grid[x] != 0x10) grid[x] = t->boss_corridor_left[grid[x] & 0x1f];
            grid[cell] = 0x10;
        }
    }
}

void od_boss_attack_retire_spans(uint8_t cells[OD_BOSS_RETIRE_SPANS],
                                  uint8_t widths[OD_BOSS_RETIRE_SPANS])
{
    const od_engine_tables *t = od_engine_tables_get();
    if (t == NULL) return;
    for (unsigned i = 0; i < OD_BOSS_RETIRE_SPANS; ++i) {
        cells[i] = t->boss_retire_spans[2 * i];
        widths[i] = t->boss_retire_spans[2 * i + 1];
    }
}
