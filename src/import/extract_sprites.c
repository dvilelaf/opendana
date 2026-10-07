/* extract_sprites.c - graphics/sprites/poses.json.
 *
 * Hero's two metasprite sets, the moving-flame poses and the steam/background
 * effects, exactly as the EU data lays them out. Reading only.
 */
#include "fsutil.h"
#include "jsonw.h"
#include "rom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int x, y, tile, attr;
} part;

typedef struct {
    int bank, address, chr_bank, first, count;
} pose_entry;

typedef struct {
    part *parts;
    size_t parts_count, parts_cap;
    pose_entry *poses;
    size_t poses_count, poses_cap;
} sprite_data;

static int signed5(int value) { return (value & 15) - (value & 16); }

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0)
        snprintf(err, cap, "%s", msg);
}

static int push_part(sprite_data *d, int x, int y, int tile, int attr)
{
    if (d->parts_count == d->parts_cap) {
        size_t ncap = d->parts_cap ? d->parts_cap * 2 : 256;
        part *np = (part *)realloc(d->parts, ncap * sizeof(*np));
        if (np == NULL)
            return -1;
        d->parts = np;
        d->parts_cap = ncap;
    }
    d->parts[d->parts_count].x = x;
    d->parts[d->parts_count].y = y;
    d->parts[d->parts_count].tile = tile;
    d->parts[d->parts_count].attr = attr;
    d->parts_count++;
    return 0;
}

static int push_pose(sprite_data *d, int bank, int address, int chr_bank,
                     int first, int count)
{
    if (d->poses_count == d->poses_cap) {
        size_t ncap = d->poses_cap ? d->poses_cap * 2 : 128;
        pose_entry *np = (pose_entry *)realloc(d->poses, ncap * sizeof(*np));
        if (np == NULL)
            return -1;
        d->poses = np;
        d->poses_cap = ncap;
    }
    d->poses[d->poses_count].bank = bank;
    d->poses[d->poses_count].address = address;
    d->poses[d->poses_count].chr_bank = chr_bank;
    d->poses[d->poses_count].first = first;
    d->poses[d->poses_count].count = count;
    d->poses_count++;
    return 0;
}

static uint16_t w16(const uint8_t *data, int at) { return (uint16_t)(data[at] | (data[at + 1] << 8)); }

/* Collect the pose addresses reachable from the 40 sequence roots. Sorted. */
static int collect_poses(const uint8_t *data, uint16_t *poses, size_t cap, size_t *count)
{
    uint16_t roots[40];
    for (int i = 0; i < 40; i++)
        roots[i] = w16(data, 2 * i);
    uint16_t *pending = (uint16_t *)malloc(sizeof(uint16_t) * 4096);
    uint8_t *seen = (uint8_t *)calloc(0x2000, 1);
    if (pending == NULL || seen == NULL) {
        free(pending);
        free(seen);
        return -1;
    }
    size_t pn = 0, pc = 4096;
    for (int i = 0; i < 40; i++)
        pending[pn++] = roots[i];
    *count = 0;
    while (pn > 0) {
        uint16_t addr = pending[--pn];
        int pc_ = addr;
        if (pc_ < 0xA050 || pc_ >= 0xC000)
            continue;
        if (seen[pc_ - 0xA000])
            continue;
        seen[pc_ - 0xA000] = 1;
        uint8_t op = data[pc_ - 0xA000];
        int size = 1;
        uint16_t targets[5];
        int nt = 0;
        if (op >= 0xF0) {
            switch (op) {
            case 0xFE: size = 3; break;
            case 0xFD: size = 5; break;
            case 0xFC: size = 5; break;
            case 0xFB: size = 3; break;
            case 0xF8: size = 9; break;
            case 0xF7: size = 3; break;
            case 0xF6: size = 2; break;
            default: size = 1; break;
            }
            if (op == 0xFE)
                targets[nt++] = w16(data, pc_ + 1 - 0xA000);
            else if (op == 0xFD || op == 0xFC)
                targets[nt++] = w16(data, pc_ + 3 - 0xA000);
            else if (op == 0xF8)
                for (int k = 0; k < 4; k++)
                    targets[nt++] = w16(data, pc_ + 1 + k * 2 - 0xA000);
        } else if (op >= 0x80 && op < 0xF0) {
            size = 2;
            uint16_t target = (uint16_t)((op << 8) | data[pc_ - 0x9FFF]);
            if (*count < cap)
                poses[(*count)++] = target;
        }
        if (!(op == 0xFF || op == 0xFE || op == 0xF8))
            targets[nt++] = (uint16_t)(pc_ + size);
        for (int k = 0; k < nt; k++) {
            if (pn == pc) {
                pc *= 2;
                uint16_t *np = (uint16_t *)realloc(pending, sizeof(uint16_t) * pc);
                if (np == NULL) {
                    free(pending);
                    free(seen);
                    return -1;
                }
                pending = np;
            }
            pending[pn++] = targets[k];
        }
    }
    free(pending);
    free(seen);
    /* sort + dedup */
    for (size_t i = 1; i < *count; i++) {
        uint16_t v = poses[i];
        size_t j = i;
        while (j > 0 && poses[j - 1] > v) {
            poses[j] = poses[j - 1];
            j--;
        }
        poses[j] = v;
    }
    size_t out = 0;
    for (size_t i = 0; i < *count; i++)
        if (out == 0 || poses[i] != poses[out - 1])
            poses[out++] = poses[i];
    *count = out;
    return 0;
}

/* Flatten one bank-4/5 metaspritle at `address`. */
static int decode_pose(const uint8_t *data, int address, sprite_data *d)
{
    int pc = address - 0xA000;
    int chr_bank = data[pc++];
    int x = 0, y = 0;
    for (int guard = 0; guard < 256; guard++) {
        uint8_t op = data[pc++];
        if (op < 0x80) {
            if (push_part(d, ((x + 128) & 255) - 128, ((y + 128) & 255) - 128,
                          op, data[pc++]) != 0)
                return -1;
            y += 8;
        } else if (op < 0xA0) {
            y = -(op - 0x80);
        } else if (op < 0xC0) {
            x = signed5(op - 0xA0);
        } else if (((op ^ 0xFF) & 3) == 0) {
            return chr_bank;
        } else if (((op ^ 0xFF) & 3) == 1) {
            pc = w16(data, pc) - 0xA000;
        } else if (((op ^ 0xFF) & 3) == 2) {
            y = -data[pc++];
        } else {
            x = data[pc++];
            if (x >= 128)
                x -= 256;
        }
    }
    return -1;
}

/* The moving flames' poses, appended after Hero's. */
static int collect_flame_poses(const uint8_t *data, sprite_data *d)
{
    uint8_t *mark = (uint8_t *)calloc(0x2000, 1);
    uint16_t *addresses = (uint16_t *)malloc(sizeof(uint16_t) * 4096);
    if (mark == NULL || addresses == NULL) {
        free(mark);
        free(addresses);
        return -1;
    }
    size_t an = 0;
    for (int index = 0; index < 48; index++) {
        int at = (int)w16(data, 0x16AA + 2 * index) - 0xA000;
        while (data[at]) {
            uint16_t a = w16(data, at + 1);
            if (!mark[a - 0xA000]) {
                mark[a - 0xA000] = 1;
                addresses[an++] = a;
            }
            at += 3;
        }
    }
    /* sorted */
    for (size_t i = 1; i < an; i++) {
        uint16_t v = addresses[i];
        size_t j = i;
        while (j > 0 && addresses[j - 1] > v) {
            addresses[j] = addresses[j - 1];
            j--;
        }
        addresses[j] = v;
    }
    for (size_t i = 0; i < an; i++) {
        int pc = addresses[i] - 0xA000;
        int chr_bank = data[pc++];
        int x = 0, y = 0;
        int first = (int)d->parts_count;
        int ok = 0;
        for (int guard = 0; guard < 256; guard++) {
            uint8_t op = data[pc++];
            if (op < 0xF0) {
                if (push_part(d, x, ((y + 128) & 255) - 128, op, data[pc++]) != 0) {
                    free(mark);
                    free(addresses);
                    return -1;
                }
                y += 8;
            } else if (((op ^ 0xFF) & 3) == 0) {
                ok = 1;
                break;
            } else if (((op ^ 0xFF) & 3) == 1) {
                pc = w16(data, pc) - 0xA000;
            } else if (((op ^ 0xFF) & 3) == 2) {
                y = -data[pc++];
            } else {
                x = data[pc++];
                if (x >= 128)
                    x -= 256;
            }
        }
        if (!ok || push_pose(d, 3, addresses[i], chr_bank, first,
                             (int)d->parts_count - first) != 0) {
            free(mark);
            free(addresses);
            return -1;
        }
    }
    free(mark);
    free(addresses);
    return 0;
}

/* The pipe segments Hero is drawn as while she travels a connector: around
 * the connector cell, one metasprite per type (6 types; count, then dy, tile,
 * attribute, dx for each part, CHR bank 0x10). They are stored as poses keyed
 * by the address of their list, which is how the compositor looks them up. */
static int collect_pipe_poses(const uint8_t *prg0, sprite_data *d)
{
    for (int type = 0; type < 6; type++) {
        int at = (int)w16(prg0, 0x4B2 + 2 * type) - 0x8000;
        int address = at + 0x8000;
        int count = prg0[at++];
        int first = (int)d->parts_count;
        for (int i = 0; i < count; i++, at += 4) {
            int dy = (int8_t)prg0[at];
            int dx = (int8_t)prg0[at + 3];
            if (push_part(d, dx, dy, prg0[at + 1], prg0[at + 2]) != 0)
                return -1;
        }
        if (push_pose(d, 0, address, 0x10, first, count) != 0)
            return -1;
    }
    return 0;
}

od_status od_import_sprites(const od_rom *rom, const char *dir, char *err, size_t err_cap)
{
    const uint8_t *prg = rom->prg;
    sprite_data d;
    memset(&d, 0, sizeof(d));
    uint16_t addresses[1024];

    for (int bank = 4; bank <= 5; bank++) {
        const uint8_t *data = prg + (size_t)bank * 8192;
        size_t count = 0;
        if (collect_poses(data, addresses, sizeof(addresses) / sizeof(addresses[0]),
                          &count) != 0) {
            free(d.parts);
            free(d.poses);
            set_err(err, err_cap, "sequence table too large");
            return OD_ERR_FORMAT;
        }
        for (size_t i = 0; i < count; i++) {
            int first = (int)d.parts_count;
            int chr_bank = decode_pose(data, addresses[i], &d);
            if (chr_bank < 0) {
                free(d.parts);
                free(d.poses);
                set_err(err, err_cap, "malformed pose data");
                return OD_ERR_FORMAT;
            }
            if (push_pose(&d, bank, addresses[i], chr_bank, first,
                          (int)d.parts_count - first) != 0) {
                free(d.parts);
                free(d.poses);
                set_err(err, err_cap, "out of memory");
                return OD_ERR_NOMEM;
            }
        }
    }
    if (collect_flame_poses(prg + 3 * 8192, &d) != 0) {
        free(d.parts);
        free(d.poses);
        set_err(err, err_cap, "malformed flame pose data");
        return OD_ERR_FORMAT;
    }
    if (collect_pipe_poses(prg, &d) != 0) {
        free(d.parts);
        free(d.poses);
        set_err(err, err_cap, "malformed pipe pose data");
        return OD_ERR_FORMAT;
    }

    /* Steam and background effects. */
    part steam[256];
    int steam_phases[6][3];
    size_t steam_count = 0;
    for (int phase = 0; phase < 6; phase++) {
        int offset = 0x275 + phase * 3;
        int wait = prg[offset];
        int address = w16(prg, offset + 1);
        int pc = address - 0x8000;
        int x = 0, y = 0;
        int first = (int)steam_count;
        int done = 0;
        for (int guard = 0; guard < 256; guard++) {
            uint8_t op = prg[pc++];
            if (op >= 128) {
                steam[steam_count].x = x;
                steam[steam_count].y = y;
                steam[steam_count].tile = op;
                steam[steam_count].attr = prg[pc++];
                steam_count++;
                y += 8;
            } else if (op < 32) {
                y = -op;
            } else if (op < 64) {
                x = signed5(op - 32);
            } else if (op == 64) {
                done = 1;
                break;
            } else {
                free(d.parts);
                free(d.poses);
                set_err(err, err_cap, "unsupported steam data");
                return OD_ERR_FORMAT;
            }
        }
        if (!done) {
            free(d.parts);
            free(d.poses);
            set_err(err, err_cap, "unterminated steam pose");
            return OD_ERR_FORMAT;
        }
        steam_phases[phase][0] = wait;
        steam_phases[phase][1] = first;
        steam_phases[phase][2] = (int)steam_count - first;
    }
    uint8_t bg_banks[4][6], bg_waits[4][6];
    for (int i = 0; i < 4; i++) {
        int bp = w16(prg, 0xD04A + i * 2) + 0x4000;
        int wp = w16(prg, 0xD058 + i * 2) + 0x4000;
        for (int k = 0; k < 6; k++) {
            bg_banks[i][k] = prg[bp + k];
            bg_waits[i][k] = prg[wp + k];
        }
    }

    /* JSON. */
    od_jw *doc = od_jw_object();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    od_jw_add(doc, "id", od_jw_string("sprites"));
    od_jw *wsb = od_jw_array();
    for (int i = 0; i < 10; i++)
        od_jw_push(wsb, od_jw_integer(prg[0x55BF + i]));
    od_jw_add(doc, "world_sprite_banks", wsb);

    od_jw *parts = od_jw_array();
    for (size_t i = 0; i < d.parts_count; i++) {
        od_jw *row = od_jw_array();
        od_jw_push(row, od_jw_integer(d.parts[i].x));
        od_jw_push(row, od_jw_integer(d.parts[i].y));
        od_jw_push(row, od_jw_integer(d.parts[i].tile));
        od_jw_push(row, od_jw_integer(d.parts[i].attr));
        od_jw_push(parts, row);
    }
    od_jw_add(doc, "pose_parts", parts);

    od_jw *poses = od_jw_array();
    for (size_t i = 0; i < d.poses_count; i++) {
        od_jw *row = od_jw_array();
        od_jw_push(row, od_jw_integer(d.poses[i].bank));
        od_jw_push(row, od_jw_integer(d.poses[i].address));
        od_jw_push(row, od_jw_integer(d.poses[i].chr_bank));
        od_jw_push(row, od_jw_integer(d.poses[i].first));
        od_jw_push(row, od_jw_integer(d.poses[i].count));
        od_jw_push(poses, row);
    }
    od_jw_add(doc, "poses", poses);

    od_jw *sp = od_jw_array();
    for (size_t i = 0; i < steam_count; i++) {
        od_jw *row = od_jw_array();
        od_jw_push(row, od_jw_integer(steam[i].x));
        od_jw_push(row, od_jw_integer(steam[i].y));
        od_jw_push(row, od_jw_integer(steam[i].tile));
        od_jw_push(row, od_jw_integer(steam[i].attr));
        od_jw_push(sp, row);
    }
    od_jw_add(doc, "steam_parts", sp);
    od_jw *sph = od_jw_array();
    for (int i = 0; i < 6; i++) {
        od_jw *row = od_jw_array();
        for (int k = 0; k < 3; k++)
            od_jw_push(row, od_jw_integer(steam_phases[i][k]));
        od_jw_push(sph, row);
    }
    od_jw_add(doc, "steam_phases", sph);
    od_jw *bb = od_jw_array();
    for (int i = 0; i < 4; i++) {
        od_jw *row = od_jw_array();
        for (int k = 0; k < 6; k++)
            od_jw_push(row, od_jw_integer(bg_banks[i][k]));
        od_jw_push(bb, row);
    }
    od_jw_add(doc, "bg_banks", bb);
    od_jw *bw = od_jw_array();
    for (int i = 0; i < 4; i++) {
        od_jw *row = od_jw_array();
        for (int k = 0; k < 6; k++)
            od_jw_push(row, od_jw_integer(bg_waits[i][k]));
        od_jw_push(bw, row);
    }
    od_jw_add(doc, "bg_waits", bw);

    free(d.parts);
    free(d.poses);

    char path[4096];
    od_fs_join(path, sizeof(path), dir, "graphics/sprites/poses.json");
    od_fs_mkdir_parent(path);
    FILE *f = od_fs_fopen(path, "wb");
    if (f == NULL) {
        od_jw_free(doc);
        set_err(err, err_cap, "cannot write graphics/sprites/poses.json");
        return OD_ERR_IO;
    }
    od_jw_write(f, doc, 1);
    fputc('\n', f);
    fclose(f);
    od_jw_free(doc);
    return OD_OK;
}
