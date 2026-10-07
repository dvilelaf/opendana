/* enemy.c - object/enemy records and entities. See opendana/enemy.h. */
#include "opendana/enemy.h"

#include "opendana/content_tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Raw asset                                                                  */
/* ------------------------------------------------------------------------- */

od_status od_enemyset_parse(od_enemyset *out, const uint8_t *data, size_t size)
{
    if (out == NULL || data == NULL)
        return OD_ERR_FORMAT;
    memset(out, 0, sizeof(*out));

    if (size < OD_ENEMY_HEADER_SIZE || memcmp(data, OD_ENEMY_MAGIC, 4) != 0)
        return OD_ERR_FORMAT;
    if (data[4] != OD_ENEMY_VERSION || data[5] != OD_GRID_W || data[6] != OD_GRID_H)
        return OD_ERR_FORMAT;

    uint16_t count = od_rd16(data + 8);
    if (count > OD_LEVEL_COUNT)
        return OD_ERR_RANGE;
    out->record_fnv1a32 = od_rd32(data + 12);
    out->record_fnv1a64 = (uint64_t)od_rd32(data + 16) |
                          ((uint64_t)od_rd32(data + 20) << 32);

    uint8_t *buffer = (uint8_t *)malloc(size);
    if (buffer == NULL)
        return OD_ERR_NOMEM;
    memcpy(buffer, data, size);
    out->buffer = buffer;
    out->buffer_size = size;

    size_t pos = OD_ENEMY_HEADER_SIZE;
    for (int i = 0; i < count; i++) {
        if (pos + 3 > size) {
            od_enemyset_free(out);
            return OD_ERR_FORMAT;
        }
        uint16_t len = od_rd16(buffer + pos + 1);
        pos += 3;
        if (len == 0 || pos + len > size || buffer[pos + len - 1] != 0x00) {
            od_enemyset_free(out);
            return OD_ERR_FORMAT;
        }
        out->levels[i].len = len;
        out->levels[i].data = buffer + pos;
        pos += len;
    }
    out->count = count;
    return OD_OK;
}

od_status od_enemyset_load_file(od_enemyset *out, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return OD_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return OD_ERR_IO;
    }
    long len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return OD_ERR_IO;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)len);
    if (buf == NULL) {
        fclose(f);
        return OD_ERR_NOMEM;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(buf);
        return OD_ERR_IO;
    }
    od_status st = od_enemyset_parse(out, buf, (size_t)len);
    free(buf);
    return st;
}

void od_enemyset_free(od_enemyset *set)
{
    if (set == NULL)
        return;
    free(set->buffer);
    set->buffer = NULL;
    set->buffer_size = 0;
}

uint32_t od_enemyset_hash32(const od_enemyset *set)
{
    uint32_t h = OD_FNV1A32_INIT;
    for (int i = 0; i < set->count; i++)
        h = od_fnv1a32(set->levels[i].data, set->levels[i].len, h);
    return h;
}

uint64_t od_enemyset_hash64(const od_enemyset *set)
{
    uint64_t h = OD_FNV1A64_INIT;
    for (int i = 0; i < set->count; i++)
        h = od_fnv1a64(set->levels[i].data, set->levels[i].len, h);
    return h;
}

/* ------------------------------------------------------------------------- */
/* Record iteration                                                           */
/* ------------------------------------------------------------------------- */

void od_enemy_cursor_init(od_enemy_cursor *c, const od_enemy_record *rec)
{
    c->p = rec->data;
    c->end = rec->data + rec->len;
}

int od_enemy_cursor_next(od_enemy_cursor *c, od_enemy_command *out)
{
    if (c->p >= c->end)
        return 0;
    uint8_t kind = c->p[0];
    if (kind == 0x00)
        return 0; /* terminator */
    if (kind >= 0x80) {
        if (c->p + 2 > c->end)
            return -1;
        out->kind = kind;
        out->count = 0;
        out->argument = c->p[1];
        out->positions = NULL;
        c->p += 2;
        return 1;
    }
    if (c->p + 2 > c->end)
        return -1;
    uint8_t count = c->p[1];
    if (count == 0 || c->p + 2 + count > c->end)
        return -1;
    out->kind = kind;
    out->count = count;
    out->argument = 0;
    out->positions = c->p + 2;
    c->p += 2 + count;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Type catalogue                                                             */
/* ------------------------------------------------------------------------- */

/* Legacy entity classification, retained for the existing game ABI.
 * STATIC here means grid-owned, NOT immune to gravity/magic. The semantic
 * catalogue is od_object_type; exact flame updates use od_flame_tick below.
 * The legacy updater is not a verified simulation. */
static const od_enemy_type_info type_table[0x20] = {
    [0x00] = { "empty",         OD_EKIND_STATIC },
    [0x01] = { "solid",         OD_EKIND_STATIC },
    [0x02] = { "ice-middle",    OD_EKIND_STATIC },
    [0x03] = { "ice-left",      OD_EKIND_STATIC },
    [0x04] = { "ice-right",     OD_EKIND_STATIC },
    [0x05] = { "ice",           OD_EKIND_STATIC },
    [0x06] = { "pipe-vertical", OD_EKIND_STATIC },
    [0x07] = { "pipe-horizontal", OD_EKIND_STATIC },
    [0x08] = { "pipe-left-cap", OD_EKIND_STATIC },
    [0x09] = { "pipe-right-cap", OD_EKIND_STATIC },
    [0x0A] = { "pipe-cap-a",    OD_EKIND_STATIC },
    [0x0B] = { "pipe-cap-b",    OD_EKIND_STATIC },
    [0x0C] = { "stone",         OD_EKIND_STATIC },
    [0x0D] = { "reactive-0d",   OD_EKIND_STATIC },
    [0x0E] = { "fire",          OD_EKIND_STATIC },
    [0x0F] = { "hazard-0f",     OD_EKIND_STATIC },
};

static const od_enemy_type_info unknown_type = { "unknown", OD_EKIND_STATIC };

/* 0x80 is a level parameter (it carries a value, not a cell); 0x81..0x83
 * instantiate a moving enemy at a cell. */
static const od_enemy_type_info special_80 = { "special-80-param", OD_EKIND_STATIC };
static const od_enemy_type_info special_81 = { "special-81-flame-h", OD_EKIND_PATROL_H };
static const od_enemy_type_info special_82 = { "special-82-flame-v", OD_EKIND_PATROL_V };
static const od_enemy_type_info special_83 = { "special-83", OD_EKIND_SPECIAL };

const od_enemy_type_info *od_enemy_type(uint8_t type)
{
    if (type < 0x10)
        return &type_table[type];
    switch (type) {
    case 0x80: return &special_80;
    case 0x81: return &special_81;
    case 0x82: return &special_82;
    case 0x83: return &special_83;
    default:   return &unknown_type;
    }
}

/* ------------------------------------------------------------------------- */
/* Entities                                                                   */
/* ------------------------------------------------------------------------- */

static void spawn_one(od_enemy_list *list, uint8_t type, int pos, int dir)
{
    if (list->count >= OD_ENEMY_MAX)
        return;
    od_enemy *e = &list->items[list->count++];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->kind = (uint8_t)od_enemy_type(type)->kind;
    e->alive = 1;
    e->x = pos & 0x0F;
    e->y = (pos >> 4) & 0x0F;
    e->home_x = e->x;
    e->home_y = e->y;
    e->dir = dir;
}

static int spawn_level(od_enemy_list *list, const od_enemyset *set, int index,
                       int include_static)
{
    if (list == NULL || set == NULL || index < 0 || index >= set->count)
        return -1;
    memset(list, 0, sizeof(*list));

    od_enemy_cursor cur;
    od_enemy_cursor_init(&cur, &set->levels[index]);
    od_enemy_command cmd;
    int rc;
    while ((rc = od_enemy_cursor_next(&cur, &cmd)) == 1) {
        if (cmd.kind >= 0x80) {
            if (cmd.kind > 0x83)
                return -1; /* no unknown special treated as inert terrain */
            /* 0x80 carries a level parameter, not a cell, so it spawns no
             * entity. 0x81..0x83 instantiate a moving enemy at the argument
             * cell (y*16+x) and are always spawned, even by the mobile-only
             * entry point. */
            const od_enemy_type_info *info = od_enemy_type(cmd.kind);
            if (info->kind == OD_EKIND_STATIC)
                continue;
            if (list->count >= OD_ENEMY_MAX)
                return -1;
            od_enemy *e = &list->items[list->count++];
            memset(e, 0, sizeof(*e));
            e->type = cmd.kind;
            e->kind = (uint8_t)info->kind;
            e->alive = 1;
            e->x = cmd.argument & 0x0F;
            e->y = (cmd.argument >> 4) & 0x0F;
            e->home_x = e->x;
            e->home_y = e->y;
            e->dir = 1;
        } else {
            if (cmd.kind >= 0x10)
                return -1;
            od_enemy_kind kind = od_enemy_type(cmd.kind)->kind;
            if (!include_static && kind == OD_EKIND_STATIC)
                continue;
            if (list->count + cmd.count > OD_ENEMY_MAX)
                return -1;
            for (uint8_t k = 0; k < cmd.count; k++)
                spawn_one(list, cmd.kind, cmd.positions[k], 1);
        }
    }
    return rc < 0 ? -1 : list->count;
}

int od_enemy_spawn_level(od_enemy_list *list, const od_enemyset *set, int index)
{
    return spawn_level(list, set, index, 0);
}

int od_enemy_spawn_level_all(od_enemy_list *list, const od_enemyset *set, int index)
{
    return spawn_level(list, set, index, 1);
}

static int default_blocked(void *ctx, int x, int y)
{
    /* No grid: out-of-bounds is a wall, everything else is free. */
    (void)ctx;
    return x < 0 || x >= OD_GRID_W || y < 0 || y >= OD_GRID_H;
}

static void update_one(od_enemy *e, od_enemy_blocked_fn blocked, void *ctx)
{
    if (!e->alive)
        return;
    switch (e->kind) {
    case OD_EKIND_FALLER:
        if (!blocked(ctx, e->x, e->y + 1))
            e->y++;
        break;
    case OD_EKIND_WALKER:
        /* Turn around at a wall or before a ledge, then step one cell. */
        if (blocked(ctx, e->x + e->dir, e->y) ||
            !blocked(ctx, e->x + e->dir, e->y + 1))
            e->dir = -e->dir;
        if (!blocked(ctx, e->x + e->dir, e->y))
            e->x += e->dir;
        e->timer++;
        break;
    case OD_EKIND_PATROL_H:
        /* A floating horizontal patroller: it reverses only at a wall.
         * Evidence: the four 0x81 entities of ROUND 2-10 sweep one cell each
         * side of their spawn along a fixed row over empty cells. */
        if (blocked(ctx, e->x + e->dir, e->y))
            e->dir = -e->dir;
        if (!blocked(ctx, e->x + e->dir, e->y))
            e->x += e->dir;
        e->timer++;
        break;
    case OD_EKIND_PATROL_V:
        /* A floating vertical patroller. Evidence: the seven 0x82 entities of
         * ROUND 4-10 keep a fixed column and sweep one cell up/down. */
        if (blocked(ctx, e->x, e->y + e->dir))
            e->dir = -e->dir;
        if (!blocked(ctx, e->x, e->y + e->dir))
            e->y += e->dir;
        e->timer++;
        break;
    case OD_EKIND_SPECIAL:
        /* Not yet characterised: spawn it but leave it in place. */
        e->timer++;
        break;
    case OD_EKIND_STATIC:
    default:
        break;
    }
}

void od_enemy_tick(od_enemy_list *list, od_enemy_blocked_fn blocked, void *ctx)
{
    if (list == NULL)
        return;
    if (blocked == NULL)
        blocked = default_blocked;
    for (int i = 0; i < list->count; i++)
        update_one(&list->items[i], blocked, ctx);
}

/* The flame data are extracted and loaded from the content package; the
 * state machine below is handwritten C. */

od_status od_flame_init(od_flame *e, uint8_t type, uint8_t cell)
{
    if (!e || type < 1 || type > 3 || cell >= 240)
        return OD_ERR_RANGE;
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->cell = cell;
    e->state[2] = type == 1 ? 0x10 : 0x80;
    e->state[5] = type == 1 ? 0x80 : 0x10;
    e->state[4] = (cell & 0xf0) | 8;
    e->state[7] = (uint8_t)((cell << 4) | 8);
    return OD_OK;
}

static unsigned flame_sequence(const od_engine_tables *t, uint8_t type, unsigned world,
                               uint8_t flags)
{
    unsigned turn = (flags >> 6) & 1;
    if (type == 1)
        return t->horizontal_animations[world * 2 + turn];
    unsigned index = world * 4 + ((flags & 0x80) ? 2 : 0) + turn;
    return type == 2 ? t->vertical_animations[index] : t->resistant_animations[index];
}

static void flame_add(uint8_t *lo, uint8_t *hi, int delta)
{
    uint16_t value = (uint16_t)((unsigned)*lo | ((unsigned)*hi << 8));
    value = (uint16_t)(value + delta);
    *lo = (uint8_t)value;
    *hi = (uint8_t)(value >> 8);
}

static int signed_byte(uint8_t value)
{
    return value < 128 ? value : (int)value - 256;
}

od_status od_flame_tick(od_flame *e, unsigned world, uint8_t game_flags,
                         const uint8_t grid[256], uint8_t hero_x,
                         uint8_t hero_y, uint8_t scroll_x,
                         uint8_t *hero_flags)
{
    if (!e || !grid || !hero_flags || world >= 17 || e->type > 3)
        return OD_ERR_RANGE;
    const od_engine_tables *t = od_engine_tables_get();
    if (t == NULL)
        return OD_ERR_FORMAT;
    if (!e->type)
        return OD_OK; /* explicitly removed slot */
    uint8_t *s = e->state;
    uint8_t flags = s[0];
    unsigned sequence = flame_sequence(t, e->type, world, flags);
    if (s[1] >= t->flame_animation[sequence].count)
        return OD_ERR_FORMAT;
    unsigned wait = flags & 15;
    if (!wait || --wait == 0) {
        if ((flags & 15) && ++s[1] == t->flame_animation[sequence].count) {
            s[1] = 0;
            if (flags & 0x40) {
                flags = (flags & 0x80) ^ 0x80;
                sequence = flame_sequence(t, e->type, world, flags);
            }
        }
        wait = t->flame_animation[sequence].wait;
    }
    s[0] = (flags & 0xf0) | wait;
    e->pose = t->flame_animation[sequence].pose[s[1]];

    if (!game_flags && !(flags & 0x40)) {
        int vy = e->type == 1 ? signed_byte(s[2]) :
            ((!!(flags & 0x80) == (e->type == 2)) ? s[2] : -(int)s[2]);
        int vx = e->type == 1 ? ((flags & 0x80) ? s[5] : -(int)s[5]) :
            signed_byte(s[5]);
        flame_add(&s[3], &s[4], vy);
        flame_add(&s[6], &s[7], vx);
    }
    if (game_flags)
        return OD_OK; /* animation runs even when translation/contact stops */

    /* The vertical distance keeps a borrow from the horizontal compare, so
     * dy is HeroY - flameY - 1, not a symmetric AABB. */
    uint8_t screen_x = (uint8_t)(s[7] - scroll_x);
    int dx = (int)hero_x - screen_x;
    if (dx < 0) dx = -dx;
    int dy = (int)hero_y - s[4] - 1;
    if (dx < 12 && (dy < 0 ? (uint8_t)-dy < 14 : dy < 12))
        *hero_flags |= 0x10;
    if (flags & 0x40)
        return OD_OK;

    uint8_t target;
    if (e->type == 1) {
        int distance = (int)((e->cell & 0xf0) | 8) - s[4];
        if (abs(distance) >= 2) s[2] = (uint8_t)-s[2];
        e->cell = (s[4] & 0xf0) | (s[7] >> 4);
        target = (uint8_t)((s[4] & 0xf0) + ((uint8_t)(s[7] - 8) >> 4)
                           + ((flags & 0x80) ? 1 : 0));
    } else {
        /* The original deliberately uses cell<<4 WITHOUT +8 here. */
        int distance = (int)(uint8_t)(e->cell << 4) - s[7];
        if (abs(distance) >= 2) s[5] = (uint8_t)-s[5];
        e->cell = (s[4] & 0xf0) | (s[7] >> 4);
        int down = !!(flags & 0x80) == (e->type == 2);
        target = ((uint8_t)(s[4] + (down ? 8 : -8)) & 0xf0) | (s[7] >> 4);
    }
    if ((e->type == 1 && !(target & 15)) ||
        (grid[target] != 0 && grid[target] != 0x0e)) {
        s[0] = (s[0] & 0xb0) | 0x40;
        s[1] = 0;
    }
    return OD_OK;
}
