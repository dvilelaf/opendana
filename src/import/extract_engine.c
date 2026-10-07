/* extract_engine.c - engine JSON documents.
 *
 * Reads the engine data from the ROM image:
 *   - the enemy animation and flame animation tables;
 *   - Hero's action programs;
 *   - the Boss combat tables;
 *   - the password, editor, scene and presentation tables.
 * Reading only.
 */
#include "fsutil.h"
#include "jsonw.h"
#include "rom.h"
#include "audio_layout.h"
#include "opendana/rom_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0)
        snprintf(err, cap, "%s", msg);
}

static od_jw *bytes_node(const uint8_t *data, size_t n)
{
    od_jw *arr = od_jw_array();
    for (size_t i = 0; i < n; i++)
        od_jw_push(arr, od_jw_integer(data[i]));
    return arr;
}

static int write_doc(const char *dir, const char *rel, od_jw *doc, char *err, size_t cap)
{
    char path[4096];
    od_fs_join(path, sizeof(path), dir, rel);
    if (od_fs_mkdir_parent(path) != 0) {
        od_jw_free(doc);
        set_err(err, cap, "cannot create an engine table directory");
        return -1;
    }
    FILE *f = od_fs_fopen(path, "wb");
    if (f == NULL) {
        od_jw_free(doc);
        set_err(err, cap, "cannot write an engine table");
        return -1;
    }
    od_jw_write(f, doc, 1);
    fputc('\n', f);
    int failed = ferror(f);
    if (fclose(f) != 0)
        failed = 1;
    od_jw_free(doc);
    if (failed)
        set_err(err, cap, "cannot finish writing an engine table");
    return failed ? -1 : 0;
}

static od_status enemy_tables(const uint8_t *prg, const char *dir, char *err, size_t cap)
{
    const uint8_t *bank3 = prg + 0x6000;
    od_jw *doc = od_jw_object();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    od_jw_add(doc, "horizontal_animations", bytes_node(bank3 + 0x1600, 34));
    od_jw_add(doc, "vertical_animations", bytes_node(bank3 + 0x1622, 68));
    od_jw_add(doc, "resistant_animations", bytes_node(bank3 + 0x1666, 68));

    od_jw *flames = od_jw_array();
    for (int index = 0; index < 48; index++) {
        int address = bank3[0x16AA + 2 * index] | (bank3[0x16AB + 2 * index] << 8);
        int waits[4], poses[4], count = 0;
        while (bank3[address - 0xA000] != 0) {
            if (count >= 4)
                break;
            waits[count] = bank3[address - 0xA000];
            poses[count] = bank3[address - 0xA000 + 1] | (bank3[address - 0xA000 + 2] << 8);
            count++;
            address += 3;
        }
        od_jw *rec = od_jw_object();
        od_jw_add(rec, "count", od_jw_integer(count));
        od_jw_add(rec, "wait", od_jw_integer(count ? waits[0] : 0));
        od_jw *pose = od_jw_array();
        for (int k = 0; k < 4; k++)
            od_jw_push(pose, od_jw_integer(k < count ? poses[k] : 0));
        od_jw_add(rec, "pose", pose);
        od_jw_push(flames, rec);
    }
    od_jw_add(doc, "flame_animation", flames);
    return write_doc(dir, "engine/enemy-tables.json", doc, err, cap) == 0 ? OD_OK : OD_ERR_IO;
}

static od_status hero_programs(const uint8_t *prg, const char *dir, char *err, size_t cap)
{
    od_jw *doc = od_jw_object();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    od_jw_add(doc, "programs_clear", bytes_node(prg + 4 * 8192, 0x9CC));
    od_jw_add(doc, "programs_covered", bytes_node(prg + 5 * 8192, 0x9CC));
    return write_doc(dir, "engine/hero-programs.json", doc, err, cap) == 0 ? OD_OK : OD_ERR_IO;
}

static od_status boss_tables(const uint8_t *prg, const char *dir, char *err, size_t cap)
{
    int ptr_base = 0x4000 + (0xB800 - 0xA000);
    uint16_t words[32];
    for (int i = 0; i < 32; i++)
        words[i] = (uint16_t)(prg[ptr_base + 2 * i] | (prg[ptr_base + 2 * i + 1] << 8));

    uint8_t packed[4096];
    size_t packed_n = 0;
    uint16_t offsets[33];
    int nscripts = 0;
    for (int i = 0; i < 32; i++) {
        uint16_t w = words[i];
        if (w == 0)
            break;
        int off = 0x4000 + (w - 0xA000);
        offsets[nscripts++] = (uint16_t)packed_n;
        int terminated = 0;
        for (int j = 0; j < 256; j++) {
            uint8_t b = prg[off + j];
            packed[packed_n++] = b;
            if (b == 0xFF) {
                terminated = 1;
                break;
            }
        }
        if (!terminated) {
            set_err(err, cap, "boss script without terminator");
            return OD_ERR_FORMAT;
        }
    }
    offsets[nscripts] = 0xFFFF;

    od_jw *doc = od_jw_object();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    od_jw_add(doc, "attack_data", bytes_node(packed, packed_n));
    od_jw *off_node = od_jw_array();
    for (int i = 0; i <= nscripts; i++)
        od_jw_push(off_node, od_jw_integer(offsets[i]));
    od_jw_add(doc, "attack_offset", off_node);
    int r7 = 0x4000 + (0xB4E9 - 0xA000);
    int r7l = 0x4000 + (0xB4F9 - 0xA000);
    od_jw_add(doc, "corridor_right", bytes_node(prg + r7, 32));
    od_jw_add(doc, "corridor_left", bytes_node(prg + r7l, 32));
    int pose_base = 0xC000 + (0x9D13 - 0x8000);
    int pose_end = 0xC000 + (0x9F63 - 0x8000);
    od_jw_add(doc, "pose_data", bytes_node(prg + pose_base, (size_t)(pose_end - pose_base + 1)));
    int spans = 0xC000 + (0x99B0 - 0x8000);
    od_jw_add(doc, "retire_spans", bytes_node(prg + spans, 10));
    return write_doc(dir, "engine/boss-tables.json", doc, err, cap) == 0 ? OD_OK : OD_ERR_IO;
}

/* Offsets below are PRG offsets (file offsets minus the iNES header). All
 * five byte arrays come from the verified ROM; the textual alphabet is a
 * transcription rule, not a compiled glyph table. */
static od_status rom_data(const od_rom *rom, const char *dir, char *err, size_t cap)
{
    od_jw *doc = od_jw_object();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    od_jw_add(doc, "boss_idle", bytes_node(rom->prg + 0xDCAD, 50));
    od_jw_add(doc, "boss_hit", bytes_node(rom->prg + 0xDCDF, 11));
    od_jw_add(doc, "boss_defeat", bytes_node(rom->prg + 0xDCEA, 41));
    od_jw_add(doc, "password_code", bytes_node(rom->prg + 0x5C42, 32));
    od_jw_add(doc, "password_glyph", bytes_node(rom->prg + 0x5C21, 32));
    char display[33];
    unsigned n = 0;
    for (char c = 'B'; c <= 'Z'; c++) {
        if (c == 'E' || c == 'I' || c == 'O' || c == 'U' || c == 'Y')
            continue;
        display[n++] = c;
        if (c == 'X')
            display[n++] = '?';
    }
    for (char c = '1'; c <= '9'; c++)
        display[n++] = c;
    display[n++] = '.';
    display[n++] = '!';
    display[n] = 0;
    od_jw_add(doc, "password_display", od_jw_string(display));
    return write_doc(dir, "engine/rom-data.json", doc, err, cap) ? OD_ERR_IO : OD_OK;
}

/* Where the sound data sits in the sparse image audio/sequence.json fills, and
 * the music requested per world. The positions are plain numbers; only the
 * per-world list is read from the ROM. The result is the `audio_layout` object
 * of pack.json. NULL when the image is too short. */
od_jw *od_import_audio_layout(const od_rom *rom)
{
    static const struct { const char *key; long long at; } tables[] = {
        {"fade_steps", 1199}, {"song_slot", 2401}, {"song_start", 2161},
        {"periods", 2023}, {"durations", 5768}, {"effects", 2047},
        {"volume_patterns", 2521}, {"pitch_steps", 5866},
    };
    static const struct { long long first, slot; } windows[] = {
        {32768, 10}, {40960, -1}, {49152, 14}, {57344, 15},
    };
    if (rom->prg_size < 0x199B + OD_AUDIO_WORLD_MUSIC)
        return NULL;
    od_jw *doc = od_jw_object();
    od_jw_add(doc, "slot_size", od_jw_integer(8192));
    od_jw_add(doc, "first_slot", od_jw_integer(10));
    od_jw_add(doc, "slot_count", od_jw_integer(6));
    od_jw *win = od_jw_array();
    for (size_t i = 0; i < sizeof windows / sizeof windows[0]; ++i) {
        od_jw *w = od_jw_object();
        od_jw_add(w, "first", od_jw_integer(windows[i].first));
        od_jw_add(w, "slot", od_jw_integer(windows[i].slot));
        od_jw_push(win, w);
    }
    od_jw_add(doc, "windows", win);
    od_jw *tab = od_jw_object();
    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; ++i)
        od_jw_add(tab, tables[i].key, od_jw_integer(tables[i].at));
    od_jw_add(doc, "tables", tab);
    od_jw_add(doc, "sample_base", od_jw_integer(49152));
    od_jw_add(doc, "world_music", bytes_node(rom->prg + 0x199B, OD_AUDIO_WORLD_MUSIC));
    return doc;
}

static od_status editor_data(const od_rom *rom, const char *dir, char *err, size_t cap)
{
    /* File offsets minus the 16-byte iNES header. Coordinates are
     * interleaved records, not compiled arrays or two contiguous slices. */
    static const struct { const char *key; size_t offset, count, step; } fields[] = {
        {"ice_connections", 0x54E9, 64, 1}, {"palette", 0x1BA06, 36, 1},
        {"cell_tile", 0x1BABE, 16, 1}, {"toolbar_x", 0x1BA2A, 3, 1},
        {"grid_y", 0x1BA2E, 14, 1}, {"grid_x", 0x1BA3E, 15, 1},
        {"pal_y", 0x1BA54, 36, 2}, {"pal_x", 0x1BA55, 36, 2},
        {"marker_addr", 0x1BA9C, 34, 1}
    };
    od_jw *doc = od_jw_object();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    for (size_t f = 0; f < sizeof fields / sizeof fields[0]; ++f) {
        if (fields[f].offset + (fields[f].count - 1) * fields[f].step >= rom->prg_size) {
            od_jw_free(doc);
            set_err(err, cap, "editor table outside PRG");
            return OD_ERR_FORMAT;
        }
        od_jw *a = od_jw_array();
        for (size_t i = 0; i < fields[f].count; ++i)
            od_jw_push(a, od_jw_integer(rom->prg[fields[f].offset + i * fields[f].step]));
        od_jw_add(doc, fields[f].key, a);
    }
    return write_doc(dir, "engine/editor-data.json", doc, err, cap) ? OD_ERR_IO : OD_OK;
}

static od_status scene_data(const od_rom *rom, const char *dir, char *err, size_t cap)
{
    /* File offsets minus the iNES header. cycle_bg has no contiguous table:
     * its four destinations are relocated from operands. */
    static const struct { const char *key; size_t offset, count; } fields[] = {
        {"cycle_block", 0x25E7, 19}, {"map_world_up", 0xC80B, 10},
        {"map_world_down", 0xC815, 10}, {"cursor_body", 0xCBCB, 8},
        {"cursor_skin", 0xCBC3, 8}
    };
    if (rom->prg_size <= 0xCBD2 || rom->prg[0x258B] != 0xA0 ||
        rom->prg[0x258D] != 0xA2 || rom->prg[0x2595] != 0xA0 ||
        rom->prg[0x259F] != 0xA0 || rom->prg[0x25A3] != 0xA0 ||
        rom->prg[0x259C] != 0xC8) {
        set_err(err, cap, "invalid EU scene destination instructions");
        return OD_ERR_FORMAT;
    }
    unsigned ptr = rom->prg[0x258C] | ((unsigned)rom->prg[0x258E] << 8);
    if (ptr < 0x360 || ptr > 0x36F) {
        set_err(err, cap, "invalid EU scene palette pointer");
        return OD_ERR_FORMAT;
    }
    unsigned base = ptr - 0x360, y = rom->prg[0x2596];
    unsigned destinations[] = {base + y, base + y + 1,
                              base + rom->prg[0x25A0], base + rom->prg[0x25A4]};
    od_jw *doc = od_jw_object(), *cycle = od_jw_array();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    for (size_t f = 0; f < sizeof fields / sizeof fields[0]; ++f)
        od_jw_add(doc, fields[f].key, bytes_node(rom->prg + fields[f].offset, fields[f].count));
    for (unsigned i = 0; i < 4; ++i) {
        if (destinations[i] > 15) {
            od_jw_free(cycle);
            od_jw_free(doc);
            set_err(err, cap, "EU scene destination outside background palette");
            return OD_ERR_FORMAT;
        }
        od_jw_push(cycle, od_jw_integer(destinations[i]));
    }
    od_jw_add(doc, "cycle_bg", cycle);
    return write_doc(dir, "engine/scene-data.json", doc, err, cap) ? OD_ERR_IO : OD_OK;
}

static od_status presentation_data(const od_rom *rom, const char *dir, char *err, size_t cap)
{
    /* All offsets are PRG offsets (not file offsets). The data programs
     * encode pose geometry. */
    const uint8_t *p = rom->prg;
    if (rom->prg_size <= 0x1ED01) goto invalid;
    unsigned phase = (p[0xD04A] | ((unsigned)p[0xD04B] << 8)) + 0x4000;
    if (phase >= rom->prg_size) goto invalid;
    unsigned banks[3] = {p[phase], p[0x55B0], p[0x1ED01]};
    static const unsigned cases[6][3] = {{0,0,0},{1,1,1},{1,0,1},{1,1,0},{5,0,0},{14,0,0}};
    unsigned cells[24], palettes[6], player[12] = {0}, seen = 0;
    for (unsigned row = 0; row < 6; ++row) {
        unsigned type = cases[row][0], bases[2];
        for (unsigned side = 0; side < 2; ++side) {
            unsigned lookup = p[(side ? 0x53C0 : 0x52C0) + type * 16 + cases[row][side + 1]];
            if (lookup >= 25) goto invalid;
            bases[side] = p[0x54C0 + lookup];
            if (bases[side] == 0x80) bases[side] = p[0x5529];
        }
        for (unsigned q = 0; q < 4; ++q) {
            unsigned pattern = bases[q & 1] + (q & 1) + 2 * (q >> 1);
            if ((pattern >> 6) >= 3) goto invalid;
            cells[row * 4 + q] = banks[pattern >> 6] * 64 + (pattern & 63);
            if (cells[row * 4 + q] > 8191) goto invalid;
        }
        palettes[row] = p[0x54D9 + type];
        if (palettes[row] > 3) goto invalid;
    }
    unsigned root = p[0x8000] | ((unsigned)p[0x8001] << 8);
    if (root < 0xA000 || root >= 0xC000) goto invalid;
    unsigned pc = root - 0xA000;
    const uint8_t *bank = p + 0x8000;
    while (pc < 0x2000 && bank[pc] < 0x40) ++pc;
    if (pc + 1 >= 0x2000 || bank[pc] < 0x80 || bank[pc] >= 0xF0) goto invalid;
    unsigned address = ((unsigned)bank[pc] << 8) | bank[pc + 1];
    if (address < 0xA000 || address >= 0xC000) goto invalid;
    pc = address - 0xA000;
    unsigned chr = bank[pc++], ended = 0;
    int x = 0, y = 0;
    for (unsigned steps = 0; steps < 256 && pc < 0x2000; ++steps) {
        unsigned op = bank[pc++];
        if (op < 0x80) {
            if (pc >= 0x2000) goto invalid;
            unsigned attr = bank[pc++];
            if ((x != -8 && x != 0) || (y != -16 && y != -8 && y != 0) || attr > 1) goto invalid;
            unsigned index = attr * 6 + (unsigned)((y + 16) / 8) * 2 + (unsigned)((x + 8) / 8);
            if (seen & (1u << index)) goto invalid;
            seen |= 1u << index;
            player[index] = (chr & 0xFE) * 64 + op;
            if (player[index] > 8191) goto invalid;
            y += 8;
        } else if (op < 0xA0) y = -(int)(op - 0x80);
        else if (op < 0xC0) {
            int v = (int)op - 0xA0;
            x = (v & 15) - (v & 16);
        } else if (((op ^ 0xFF) & 3) == 0) { ended = 1; break; }
        else goto invalid;
    }
    if (!ended || seen != 0xFFF) goto invalid;
    od_jw *doc = od_jw_object(), *cell = od_jw_array(), *pal = od_jw_array(), *pose = od_jw_array();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    for (unsigned i = 0; i < 24; ++i) od_jw_push(cell, od_jw_integer(cells[i]));
    for (unsigned i = 0; i < 6; ++i) od_jw_push(pal, od_jw_integer(palettes[i]));
    for (unsigned i = 0; i < 12; ++i) od_jw_push(pose, od_jw_integer(player[i]));
    od_jw_add(doc, "cell_tiles", cell);
    od_jw_add(doc, "cell_palettes", pal);
    od_jw_add(doc, "player_tiles", pose);
    return write_doc(dir, "engine/presentation-data.json", doc, err, cap) ? OD_ERR_IO : OD_OK;
invalid:
    set_err(err, cap, "invalid EU terrain or idle pose data");
    return OD_ERR_FORMAT;
}

od_status od_import_engine(const od_rom *rom, const char *dir, char *err, size_t err_cap)
{
    od_status st = enemy_tables(rom->prg, dir, err, err_cap);
    if (st == OD_OK)
        st = hero_programs(rom->prg, dir, err, err_cap);
    if (st == OD_OK)
        st = boss_tables(rom->prg, dir, err, err_cap);
    if (st == OD_OK)
        st = rom_data(rom, dir, err, err_cap);
    if (st == OD_OK)
        st = editor_data(rom, dir, err, err_cap);
    if (st == OD_OK)
        st = scene_data(rom, dir, err, err_cap);
    if (st == OD_OK)
        st = presentation_data(rom, dir, err, err_cap);
    return st;
}
