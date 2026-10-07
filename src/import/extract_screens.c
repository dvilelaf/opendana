/* extract_screens.c - ROM-derived interactive artwork and canonical boss room.
 * Each decoder reads its tables at fixed offsets of the EU image. No captured
 * displays/tracks and no dummy room/retry data. Static setup descriptors are
 * decoded directly; the cinematic programs are handled elsewhere.
 */
#include "fsutil.h"
#include "jsonw.h"
#include "rom.h"
#include "sha256.h"
#include "audio_layout.h"

#include <stdio.h>
#include <string.h>

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0)
        snprintf(err, cap, "%s", msg);
}

static int write_doc(const char *dir, const char *rel, od_jw *doc, int indent, char *err, size_t cap)
{
    char path[4096];
    od_fs_join(path, sizeof(path), dir, rel);
    if (od_fs_mkdir_parent(path) != 0) {
        od_jw_free(doc);
        set_err(err, cap, "cannot create a screen document directory");
        return -1;
    }
    FILE *f = od_fs_fopen(path, "wb");
    if (f == NULL) {
        od_jw_free(doc);
        set_err(err, cap, "cannot write a screen document");
        return -1;
    }
    od_jw_write(f, doc, indent);
    fputc('\n', f);
    int failed = ferror(f);
    if (fclose(f) != 0)
        failed = 1;
    od_jw_free(doc);
    if (failed)
        set_err(err, cap, "cannot finish writing a screen document");
    return failed ? -1 : 0;
}

static od_jw *bytes(const uint8_t *data, size_t n)
{
    od_jw *arr = od_jw_array();
    for (size_t i = 0; i < n; i++)
        od_jw_push(arr, od_jw_integer(data[i]));
    return arr;
}

/* Menu tables are addressed through three windows of the image: the first at
 * 0x8000 (bank 6), the second at 0xA000 (bank 2) and the fixed banks at
 * 0xC000. Only table and layout decoding. */
static uint8_t menu_rd(const od_rom *rom, unsigned addr)
{
    size_t off = addr < 0xA000 ? 6 * 0x2000 + addr - 0x8000 :
                 addr < 0xC000 ? 2 * 0x2000 + addr - 0xA000 :
                 rom->prg_size - 0x4000 + addr - 0xC000;
    return rom->prg[off];
}

static unsigned menu_word(const od_rom *rom, unsigned addr)
{
    return menu_rd(rom, addr) | ((unsigned)menu_rd(rom, addr + 1) << 8);
}

/* Setup tables and block artwork. */
static uint8_t setup_rd(const od_rom *rom, unsigned addr)
{
    return rom->prg[0x10000 + addr - 0x8000];
}

static unsigned setup_word(const od_rom *rom, unsigned addr)
{
    return setup_rd(rom, addr) | ((unsigned)setup_rd(rom, addr + 1) << 8);
}

static unsigned art_word(const od_rom *rom, unsigned addr)
{
    const uint8_t *p = rom->prg + 0xE000 + addr - 0xA000;
    return p[0] | ((unsigned)p[1] << 8);
}

static od_jw *pair(unsigned a, unsigned b)
{
    od_jw *p = od_jw_array();
    od_jw_push(p, od_jw_integer(a));
    od_jw_push(p, od_jw_integer(b));
    return p;
}

static od_jw *palette_sets(const od_rom *rom, unsigned bg, unsigned sp, int ppu)
{
    od_jw *a = od_jw_array();
    for (unsigned i = 0; i < 32; i++) {
        unsigned ptr = menu_word(rom, i < 16 ? 0xE71E : 0xE720);
        unsigned v = menu_rd(rom, ptr + (i < 16 ? bg : sp) * 16 + i % 16);
        od_jw_push(a, od_jw_integer(ppu && i >= 16 && i % 4 == 0 ? 0 : v));
    }
    return a;
}

/* The picture table points to [tileset, width/height, block IDs]. The tileset
 * selects 4x4 tile blocks and their attribute bytes. A bounded rectangular
 * table transformation. */
static int block_picture(const od_rom *rom, unsigned id, unsigned dest,
                         uint8_t nt[2048], unsigned *width, unsigned *height)
{
    unsigned ptr = art_word(rom, 0xA008 + 2 * id);
    if (ptr < 0xA000 || ptr + 2 >= 0xC000 || dest < 0x2000 || dest >= 0x3000)
        return 0;
    const uint8_t *d = rom->prg + 0xE000 + ptr - 0xA000;
    unsigned tilebase = art_word(rom, 0xA000 + 2 * d[0]);
    unsigned attrbase = art_word(rom, 0xA004 + 2 * d[0]);
    unsigned w = ((d[1] >> 4) & 7) + 1, h = (d[1] & 7) + 1;
    unsigned x = dest & 31, y = (dest & 0x3FF) / 32;
    /* Dimensions are three-bit values; valid layouts do not cross a nametable
     * boundary. Vertical mirroring selects the nametable. */
    unsigned base = ((dest >> 10) & 1) * 1024;
    if (ptr + 2 + w * h > 0xC000 || x + 4 * w > 32 || y + 4 * h > 30)
        return 0;
    for (unsigned by = 0; by < h; by++)
        for (unsigned bx = 0; bx < w; bx++) {
            unsigned block = d[2 + by * w + bx];
            if (tilebase < 0xA000 || tilebase + block * 16 + 16 > 0xC000 ||
                attrbase < 0xA000 || attrbase + block >= 0xC000)
                return 0;
            const uint8_t *tiles = rom->prg + 0xE000 + tilebase - 0xA000 + block * 16;
            for (unsigned row = 0; row < 4; row++)
                memcpy(nt + base + (y + by * 4 + row) * 32 + x + bx * 4,
                       tiles + row * 4, 4);
            nt[base + 960 + (y / 4 + by) * 8 + x / 4 + bx] =
                rom->prg[0xE000 + attrbase - 0xA000 + block];
        }
    if (width) *width = w * 4;
    if (height) *height = h * 4;
    return 1;
}

static int hearth_screen(const od_rom *rom, od_jw **out)
{
    /* The hearth's setup script has a single rectangle followed by the
     * band/object setup entry. Only that descriptor is extracted; its actors
     * are logic elsewhere. */
    unsigned script = setup_word(rom, 0x9D14 + 6 * 2);
    const uint8_t *s = rom->prg + 0x12000 + script - 0xA000;
    if (script < 0xA000 || script > 0xBFF2 || s[1] != 8 || s[3] != 0x41 ||
        s[11] != 0x16 || s[13] != 0xFF)
        return 0;
    unsigned sub = setup_word(rom, 0x9CF4 + s[12] * 2);
    unsigned band = setup_word(rom, 0x955A + (setup_rd(rom, sub + 1) & 63) * 2);
    if (setup_rd(rom, sub) != 5 || setup_rd(rom, sub + 2) != 0x87 ||
        setup_rd(rom, band + 2) != 1)
        return 0;
    unsigned dest = setup_word(rom, 0x9CDC + s[9] * 2), w, h;
    uint8_t nt[2048] = {0};
    memset(nt + 960, setup_rd(rom, 0x821B), 64);
    if (!block_picture(rom, s[10], dest, nt, &w, &h))
        return 0;
    od_jw *sc = od_jw_object();
    od_jw_add(sc, "palette", palette_sets(rom, menu_rd(rom, 0x89C0),
                                                    menu_rd(rom, 0x89C4), 1));
    od_jw *pic = od_jw_object(), *rows = od_jw_array();
    unsigned x = dest & 31, y = (dest & 0x3FF) / 32;
    od_jw_add(pic, "x", od_jw_integer(x));
    od_jw_add(pic, "y", od_jw_integer(y));
    for (unsigned r = 0; r < h; r++)
        od_jw_push(rows, bytes(nt + (y + r) * 32 + x, w));
    od_jw_add(pic, "rows", rows);
    od_jw_add(sc, "illustration", pic);
    od_jw *attr = od_jw_object();
    rows = od_jw_array();
    od_jw_add(attr, "fill", od_jw_integer(setup_rd(rom, 0x821B)));
    od_jw_add(attr, "x", od_jw_integer(x / 4));
    od_jw_add(attr, "y", od_jw_integer(y / 4));
    for (unsigned r = 0; r < h / 4; r++)
        od_jw_push(rows, bytes(nt + 960 + (y / 4 + r) * 8 + x / 4, w / 4));
    od_jw_add(attr, "rows", rows);
    od_jw_add(sc, "attributes", attr);
    /* The upper banks come from the band table and the menu's first live
     * talking bank from its swap pair. The split sits at row 0x7F+1, the lower
     * background set is restored and sprite bank 0x36 is selected. Nonzero
     * native display words only. */
    od_jw *ppu = od_jw_array();
    for (unsigned i = 0; i < 4; i++) {
        unsigned v = setup_rd(rom, 0x946A + ((s[4] * 4) & 255) + i);
        if ((int)i == menu_rd(rom, 0x8E6A) - 0x38) v = menu_rd(rom, 0x8E49);
        od_jw_push(ppu, pair(2080 + i, v * 64));
    }
    unsigned lower = setup_rd(rom, band + 6);
    for (unsigned i = 0; i < 4; i++) {
        unsigned v = setup_rd(rom, 0x946A + ((lower * 4) & 255) + i);
        if (v) od_jw_push(ppu, pair(2084 + i, v * 64));
    }
    unsigned spr = setup_rd(rom, sub + 7);
    for (unsigned i = 0; i < 2; i++)
        od_jw_push(ppu, pair(2088 + i,
                    (setup_rd(rom, 0x952A + ((spr * 2) & 255) + i) & 254) * 64));
    for (unsigned i = 0; i < 2; i++)
        od_jw_push(ppu, pair(2090 + i, menu_rd(rom, 0xEB06) * 64));
    od_jw_push(ppu, pair(2092, setup_rd(rom, 0x80E3) & 0x1E));
    od_jw_push(ppu, pair(2093, 2));
    od_jw_push(ppu, pair(2100, menu_rd(rom, 0xF35A) + 1));
    od_jw_push(ppu, pair(2104, 1));
    od_jw_push(ppu, pair(2105, 1));
    od_jw_add(sc, "ppu", ppu);
    *out = sc;
    return 1;
}

static od_jw *sprites(const od_rom *rom, unsigned ptr, unsigned count)
{
    od_jw *a = od_jw_array();
    for (unsigned i = 0; i < count; i++) {
        od_jw *s = od_jw_array();
        for (unsigned k = 0; k < 4; k++)
            od_jw_push(s, od_jw_integer(menu_rd(rom, ptr + i * 4 + k)));
        od_jw_push(a, s);
    }
    return a;
}

static od_jw *groups(const od_rom *rom, unsigned ptr)
{
    od_jw *a = od_jw_array();
    while (menu_word(rom, ptr)) {
        unsigned g = menu_word(rom, ptr);
        od_jw *o = od_jw_object();
        od_jw_add(o, "y", od_jw_integer(menu_rd(rom, g)));
        od_jw_add(o, "x", od_jw_integer(menu_rd(rom, g + 1)));
        od_jw_add(o, "sprites", sprites(rom, g + 3, menu_rd(rom, g + 2)));
        od_jw_push(a, o);
        ptr += 2;
    }
    return a;
}

static od_jw *object_frames(const od_rom *rom, unsigned object)
{
    od_jw *a = od_jw_array();
    unsigned anim = menu_word(rom, 0x8C10 + 2 * object);
    while (menu_rd(rom, anim)) {
        unsigned ptr = menu_word(rom, anim + 1);
        od_jw *f = od_jw_object();
        od_jw_add(f, "duration", od_jw_integer(menu_rd(rom, anim)));
        od_jw_add(f, "sprites", sprites(rom, ptr + 1, menu_rd(rom, ptr)));
        od_jw_push(a, f);
        anim += 3;
    }
    return a;
}

static od_status import_map(const od_rom *rom, const char *dir, char *err, size_t cap)
{
    /* Two map scripts (scene index 2 and 43) each call their static
     * background subroutine (9 or 10). The actors that follow are live state,
     * not extracted. */
    uint8_t nt[2][2048], banks[4];
    unsigned map_sub = 0;
    for (unsigned k = 0; k < 2; k++) {
        unsigned index = k ? 43 : 2;
        unsigned script = setup_word(rom, 0x9D14 + index * 2);
        const uint8_t *s = rom->prg + (k ? 13 : 9) * 0x2000 + (script & 0x1FFF);
        if (s[1] != 0x16 || s[3] != 0xFF) {
            set_err(err, cap, "invalid EU map setup call");
            return OD_ERR_FORMAT;
        }
        unsigned sub = setup_word(rom, 0x9CF4 + s[2] * 2);
        if (setup_rd(rom, sub + 2) != 0x41) {
            set_err(err, cap, "invalid EU map background descriptor");
            return OD_ERR_FORMAT;
        }
        if (!k) map_sub = sub; /* both map scripts share the static PPU setup */
        memset(nt[k], 0, sizeof nt[k]); /* initial clear */
        memset(nt[k] + 960, setup_rd(rom, 0x821B), 64);
        memset(nt[k] + 1984, setup_rd(rom, 0x821B), 64);
        unsigned dest = setup_word(rom, 0x9CDC + setup_rd(rom, sub + 8) * 2);
        if (!block_picture(rom, setup_rd(rom, sub + 9), dest, nt[k], NULL, NULL)) {
            set_err(err, cap, "invalid EU map block picture");
            return OD_ERR_FORMAT;
        }
        if (k) {
            unsigned selector = setup_rd(rom, sub + 3);
            for (unsigned b = 0; b < 4; b++)
                banks[b] = selector & 128 ?
                    setup_rd(rom, 0x946A + ((selector * 4) & 255) + b) : selector + b;
        }
    }
    od_jw *doc = od_jw_object(), *worlds = od_jw_array();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    od_jw_add(doc, "id", od_jw_string("map"));
    for (unsigned w = 0; w < 10; w++) {
        od_jw *o = od_jw_object();
        od_jw_add(o, "x", od_jw_integer(menu_rd(rom, 0x881F + w)));
        od_jw_add(o, "y", od_jw_integer(menu_rd(rom, 0x8829 + w)));
        od_jw_push(worlds, o);
    }
    od_jw_add(doc, "worlds", worlds);
    od_jw *panel = od_jw_object(), *a = od_jw_array();
    for (unsigned r = 0; r < 10; r++) od_jw_push(a, od_jw_integer(menu_rd(rom, 0x8833 + r)));
    od_jw_add(panel, "x", a);
    od_jw_add(panel, "y", od_jw_integer(menu_rd(rom, 0x883D)));
    od_jw_add(doc, "panel", panel);
    a = od_jw_array();
    for (unsigned p = 0; p < 4; p++) {
        od_jw *pose = od_jw_array();
        for (unsigned i = 0; i < 8; i++)
            od_jw_push(pose, pair(menu_rd(rom, 0x8BB3 + p * 16 + i * 2),
                                  menu_rd(rom, 0x8BB4 + p * 16 + i * 2)));
        od_jw_push(a, pose);
    }
    od_jw_add(doc, "hero_poses", a);
    od_jw *frame = od_jw_object();
    od_jw_add(frame, "tile", od_jw_integer(menu_rd(rom, 0x8A02)));
    od_jw_add(frame, "attr", od_jw_integer(menu_rd(rom, 0x8A06)));
    od_jw_add(doc, "frame", frame);
    od_jw *fpal = od_jw_object();
    od_jw_add(fpal, "entry", od_jw_integer(menu_rd(rom, 0x8D6B) - 0x60 + menu_rd(rom, 0x8D75)));
    od_jw_add(fpal, "colors", bytes(rom->prg + 0xCD88, 3));
    od_jw_add(doc, "frame_palette", fpal);
    /* The map's palette and PPU setup the v1 document carried in
     * layouts.json:map, decoded from the image:
     *  - palette: background set 0x0B and sprite set 0x07 from the palette
     *    pointer tables; the last entry is the frame colour the animation
     *    rewrites every eighth frame, left at its settled value.
     *  - mask 0x1E.
     *  - split banks: from the descriptor's band entry (4 per lower index).
     *  - sprite banks: 2 per map descriptor index, so both sprite slots hold
     *    the same pair.
     *  - bands: 2; the panel split is at 0xA0 plus one row. */
    {
        unsigned pbg = menu_word(rom, 0xE71E), psp = menu_word(rom, 0xE720);
        unsigned entry = menu_rd(rom, 0x8D6B) - 0x60 + menu_rd(rom, 0x8D75);
        unsigned settled = menu_rd(rom, 0x8D88); /* frame colour at its first step */
        od_jw *pal = od_jw_array();
        for (unsigned i = 0; i < 32; i++) {
            unsigned v = i < 16 ? menu_rd(rom, pbg + 11 * 16 + i)
                                : menu_rd(rom, psp + 7 * 16 + (i & 15));
            if (i >= 16 && i % 4 == 0) v = 0; /* PPU universal-colour mirror */
            if (i == entry) v = settled;      /* the animation owns the last entry */
            od_jw_push(pal, od_jw_integer(v));
        }
        unsigned band = setup_word(rom, 0x955A + (setup_rd(rom, map_sub + 1) & 63) * 2);
        unsigned lower = setup_rd(rom, band + 6);
        od_jw *split = od_jw_array();
        for (unsigned i = 0; i < 4; i++)
            od_jw_push(split, od_jw_integer(
                setup_rd(rom, 0x946A + ((lower * 4) & 255) + i) * 64));
        unsigned index = (2 * setup_rd(rom, map_sub + 5)) & 255;
        unsigned s0 = setup_rd(rom, 0x952A + index);
        unsigned s1 = setup_rd(rom, 0x952A + index + 1);
        od_jw *sprites = od_jw_array();
        for (unsigned i = 0; i < 4; i++)
            od_jw_push(sprites, od_jw_integer((i & 1 ? s1 : s0) * 64));
        od_jw *bands = od_jw_array();
        for (unsigned i = 0; i < 6; i++) od_jw_push(bands, od_jw_integer(0));
        od_jw_push(bands, od_jw_integer(menu_rd(rom, 0xF371) + 1));
        for (unsigned i = 0; i < 3; i++) od_jw_push(bands, od_jw_integer(0));
        od_jw_push(bands, od_jw_integer(1));
        od_jw_push(bands, od_jw_integer(1));
        od_jw *st = od_jw_object();
        od_jw_add(st, "palette", pal);
        od_jw_add(st, "mask", od_jw_integer(setup_rd(rom, 0x80E3) & 0x1E));
        od_jw_add(st, "bands", od_jw_integer(2));
        od_jw_add(st, "band_table", bands);
        od_jw_add(st, "split_banks", split);
        od_jw_add(st, "sprite_banks", sprites);
        od_jw_add(doc, "static", st);
    }
    od_jw *digits = od_jw_object();
    od_jw_add(digits, "y", od_jw_integer(menu_rd(rom, 0x8C06)));
    od_jw_add(digits, "x", od_jw_integer(menu_rd(rom, 0x8C07)));
    od_jw_add(doc, "digits", digits);
    a = od_jw_array();
    for (unsigned k = 0; k < 3; k++) {
        od_jw *o = od_jw_object();
        od_jw_add(o, "y", od_jw_integer(menu_rd(rom, 0x8C08 + k * 2)));
        od_jw_add(o, "x", od_jw_integer(menu_rd(rom, 0x8C09 + k * 2)));
        od_jw_add(o, "frames", object_frames(rom, k));
        od_jw_push(a, o);
    }
    od_jw_add(doc, "objects", a);
    od_jw *castle = od_jw_object();
    od_jw_add(castle, "y", od_jw_integer(menu_rd(rom, 0x8BF3)));
    od_jw_add(castle, "x", od_jw_integer(menu_rd(rom, 0x8BF4)));
    od_jw_add(castle, "sprites", sprites(rom, 0x8BF6, menu_rd(rom, 0x8BF5)));
    od_jw_add(doc, "castle_open", castle);
    a = od_jw_array();
    for (unsigned i = 0; i < 2048; i++)
        if (nt[0][i] != nt[1][i] && !(i >= 1000 && i <= 1015))
            od_jw_push(a, pair(i, nt[1][i]));
    od_jw_add(doc, "castle_overlay", a);
    od_jw_add(doc, "castle_banks", bytes(banks, 4));
    return write_doc(dir, "screens/map.json", doc, 1, err, cap) ? OD_ERR_IO : OD_OK;
}

static od_jw *record(const uint8_t *prg, size_t ptr)
{
    unsigned n = prg[ptr];
    od_jw *a = od_jw_array();
    od_jw_push(a, od_jw_integer(n));
    od_jw_push(a, od_jw_integer(prg[ptr + 1] | (prg[ptr + 2] << 8)));
    od_jw_push(a, bytes(prg + ptr + 3, n & 0x3F));
    return a;
}

/* A list of video records; the pointers live in the second window. */
static od_jw *layout(const od_rom *rom, unsigned ptr)
{
    od_jw *a = od_jw_array();
    while (menu_word(rom, ptr)) {
        unsigned p = menu_word(rom, ptr);
        while (menu_rd(rom, p)) {
            od_jw_push(a, record(rom->prg, 0x4000 + p - 0xA000));
            p += 3 + (menu_rd(rom, p) & 0x3F);
        }
        ptr += 2;
    }
    return a;
}

/* Interactive setup: decoded data only. Physical 8-KiB bank numbers are
 * explicit: password bank6/2, editor bank1/13; palettes temporarily bank2.
 * Offsets below omit the iNES header. No recorded display or timeline. */
typedef struct {
    const od_rom *rom;
    int bad;
} interactive_reader;

static unsigned interactive_byte(interactive_reader *r, unsigned bank, unsigned addr)
{
    size_t offset = bank * 8192u + (addr & 8191u);
    if (addr < 0x8000 || addr > 0xffff || offset >= r->rom->prg_size) {
        r->bad = 1;
        return 0;
    }
    return r->rom->prg[offset];
}

static unsigned interactive_word(interactive_reader *r, unsigned bank, unsigned addr)
{
    return interactive_byte(r, bank, addr) | (interactive_byte(r, bank, addr + 1) << 8);
}

static od_jw *interactive_bytes(interactive_reader *r, unsigned bank, unsigned addr, unsigned n)
{
    od_jw *out = od_jw_array();
    if ((addr & 8191u) + n > 8192u) r->bad = 1;
    for (unsigned i = 0; i < n; ++i)
        od_jw_push(out, od_jw_integer(interactive_byte(r, bank, addr + i)));
    return out;
}

static unsigned interactive_records(interactive_reader *r, od_jw *out,
    unsigned bank, unsigned ptr, unsigned relative)
{
    unsigned count = 0;
    for (; count < 128 && !r->bad; ++count) {
        unsigned ctl = interactive_byte(r, bank, ptr), n = ctl & 63u;
        if (!ctl) return count;
        unsigned addr = interactive_word(r, bank, ptr + 1) + relative;
        unsigned stride = ctl & 128u ? 32u : 1u;
        if (!n || n >= 60 || (ctl & 64u) || addr < 0x2000 ||
            addr + (n - 1) * stride >= 0x2800 || (ptr & 8191u) + 3 + n >= 8192u) {
            r->bad = 1;
            break;
        }
        od_jw *record = od_jw_array();
        od_jw_push(record, od_jw_integer(ctl));
        od_jw_push(record, od_jw_integer(addr));
        od_jw_push(record, interactive_bytes(r, bank, ptr + 3, n));
        od_jw_push(out, record);
        ptr += 3 + n;
    }
    r->bad = 1;
    return count;
}

static od_jw *interactive_layout(interactive_reader *r, unsigned ptr, unsigned expected)
{
    od_jw *out = od_jw_array();
    unsigned count = 0;
    for (unsigned i = 0; i < 32 && !r->bad; ++i, ptr += 2) {
        unsigned records = interactive_word(r, 13, ptr);
        if (!records) {
            if (count != expected) r->bad = 1;
            return out;
        }
        /* Keep duplicate pointer BCAE: queue order is semantically observable. */
        count += interactive_records(r, out, 13, records, 0);
    }
    r->bad = 1;
    return out;
}

static od_jw *interactive_palette(interactive_reader *r, unsigned bg, unsigned spr)
{
    od_jw *out = od_jw_array();
    for (unsigned i = 0; i < 32; ++i) {
        unsigned value = interactive_byte(r, 2, (i < 16 ? bg : spr) + (i & 15u));
        if (value > 63) r->bad = 1;
        od_jw_push(out, od_jw_integer(value));
    }
    return out;
}

static od_status import_interactive_setup(const od_rom *rom, const char *dir, char *err, size_t cap)
{
    interactive_reader r = {rom, 0};
    od_jw *doc = od_jw_object(), *pw = od_jw_object(), *ed = od_jw_object();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    od_jw_add(doc, "id", od_jw_string("interactive-setup"));
    od_jw_add(doc, "password", pw);
    od_jw_add(doc, "editor", ed);
    od_jw_add(pw, "chr", interactive_bytes(&r, 15, 0xec52, 6));
    od_jw_add(pw, "palette_buffer", interactive_palette(&r, 0xbec0, 0xbfa0));
    od_jw *frame = od_jw_array(), *panel = od_jw_array();
    /* The frame records use relative addresses from 0x2041. */
    if (interactive_records(&r, frame, 2, 0xbc62, 0x2041) != 4) r.bad = 1;
    if (interactive_records(&r, panel, 2, 0xbcb1, 0) != 8) r.bad = 1;
    od_jw_add(pw, "field_frame", frame);
    od_jw_add(pw, "choice_panel", panel);
    od_jw_add(pw, "field_cursor_x", interactive_bytes(&r, 6, 0x8346, 24));
    od_jw *xy = od_jw_array();
    for (unsigned i = 0; i < 35; ++i)
        od_jw_push(xy, interactive_bytes(&r, 6, 0x835e + 2 * i, 2));
    od_jw_add(pw, "palette_cursor_oam_yx", xy);
    od_jw_add(ed, "chr", interactive_bytes(&r, 15, 0xee23, 6));
    od_jw_add(ed, "palette_buffer", interactive_palette(&r, 0xbed0, 0xbfb0));
    od_jw_add(ed, "initial_layout", interactive_layout(&r, 0xbacf, 27));
    od_jw_add(ed, "toolbar_layout", interactive_layout(&r, 0xbadd, 8));
    /* The entry points at a cinematic scene index, not copied bytes. */
    unsigned entry = interactive_byte(&r, 15, 0xee06);
    if (entry != 29) r.bad = 1;
    od_jw_add(ed, "entry_cinematic", od_jw_integer(entry));
    if (r.bad) {
        od_jw_free(doc);
        set_err(err, cap, "invalid EU interactive setup tables");
        return OD_ERR_FORMAT;
    }
    return write_doc(dir, "screens/interactive-setup.json", doc, 1, err, cap) ? OD_ERR_IO : OD_OK;
}

static od_status import_menu_tables(const od_rom *rom, const char *dir,
                                      char *err, size_t cap)
{
    /* Password prompt and error messages. */
    od_jw *doc = od_jw_object();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    od_jw_add(doc, "id", od_jw_string("password"));
    od_jw_add(doc, "prompt", record(rom->prg, 0x5D54));
    od_jw_add(doc, "error", record(rom->prg, 0x5D6E));
    if (write_doc(dir, "screens/password.json", doc, 1, err, cap))
        return OD_ERR_IO;

    /* Menu hearth groups, animation, uploads and resolved static setup. */
    doc = od_jw_object();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    od_jw_add(doc, "id", od_jw_string("hearth"));
    od_jw_add(doc, "groups", groups(rom, 0x8EBA));
    od_jw_add(doc, "groups_confirmed", groups(rom, 0x8EC8));
    od_jw *fire = od_jw_object(), *frames = od_jw_array();
    od_jw_add(fire, "y", od_jw_integer(menu_rd(rom, 0x8C0E)));
    od_jw_add(fire, "x", od_jw_integer(menu_rd(rom, 0x8C0F)));
    unsigned anim = menu_word(rom, 0x8C16);
    while (menu_rd(rom, anim)) {
        unsigned ptr = menu_word(rom, anim + 1);
        od_jw *f = od_jw_object();
        od_jw_add(f, "duration", od_jw_integer(menu_rd(rom, anim)));
        od_jw_add(f, "sprites", sprites(rom, ptr + 1, menu_rd(rom, ptr)));
        od_jw_push(frames, f);
        anim += 3;
    }
    od_jw_add(fire, "frames", frames);
    od_jw_add(doc, "fire", fire);
    od_jw *bank = od_jw_object(), *values = od_jw_array();
    od_jw_add(bank, "slot", od_jw_integer(menu_rd(rom, 0x8E6A) - 0x38));
    od_jw_add(bank, "period", od_jw_integer(menu_rd(rom, 0x8E43)));
    od_jw_push(values, od_jw_integer(menu_rd(rom, 0x8E49)));
    od_jw_push(values, od_jw_integer(menu_rd(rom, 0x8E49) ^ menu_rd(rom, 0x8E60)));
    od_jw_add(bank, "values", values);
    od_jw_add(doc, "bank", bank);
    od_jw_add(doc, "queue_capacity", od_jw_integer(menu_rd(rom, 0xE19B)));
    od_jw *layouts = od_jw_object();
    od_jw_add(layouts, "begin", layout(rom, 0xB963));
    od_jw_add(layouts, "whatnext", layout(rom, 0xB96B));
    od_jw_add(doc, "layouts", layouts);
    od_jw *screen = NULL;
    if (!hearth_screen(rom, &screen)) {
        od_jw_free(doc);
        set_err(err, cap, "invalid EU hearth setup descriptor");
        return OD_ERR_FORMAT;
    }
    od_jw_add(doc, "screen", screen);
    return write_doc(dir, "screens/hearth.json", doc, 1, err, cap) ? OD_ERR_IO : OD_OK;
}

/* Semantic background transfer inputs. Geometry and atlas data are decoded
 * once, storage phases preserve page boundaries only. No timings, captured
 * CIRAM or duplicate encoded image stream. */
static od_status import_transfer_structure(const od_rom *rom, const char *dir, char *err, size_t cap)
{
    interactive_reader r = {rom, 0};
    unsigned tile[2], attr[2], count[2];
    for (unsigned i = 0; i < 2; ++i) {
        tile[i] = interactive_word(&r, 7, 0xa000 + 2*i);
        attr[i] = interactive_word(&r, 7, 0xa004 + 2*i);
    }
    count[0] = tile[1] >= tile[0] ? (tile[1]-tile[0])/16 : 0;
    count[1] = attr[0] >= tile[1] ? (attr[0]-tile[1])/16 : 0;
    unsigned first_image = interactive_word(&r, 7, 0xa008);
    if (count[0] != 239 || count[1] != 132 || attr[1]-attr[0] != count[0] ||
        first_image-attr[1] != count[1] || tile[0] < 0xa000 || first_image >= 0xc000) {
        set_err(err, cap, "invalid EU transfer atlas boundaries");
        return OD_ERR_FORMAT;
    }
    od_jw *doc = od_jw_object(), *destinations = od_jw_array();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    od_jw_add(doc, "id", od_jw_string("transfer-structure"));
    for (unsigned i = 0; i < 12; ++i)
        od_jw_push(destinations, od_jw_integer(interactive_word(&r, 8, 0x9cdc+2*i)));
    od_jw_add(doc, "destinations", destinations);
    od_jw *atlases = od_jw_array();
    for (unsigned i = 0; i < 2; ++i) {
        od_jw *a = od_jw_object(), *tiles = od_jw_array();
        od_jw_add(a, "tile_storage_low", od_jw_integer(tile[i]&255));
        od_jw_add(a, "attribute_storage_low", od_jw_integer(attr[i]&255));
        for (unsigned k = 0; k < count[i]; ++k)
            od_jw_push(tiles, interactive_bytes(&r, 7, tile[i]+16*k, 16));
        od_jw_add(a, "tiles", tiles);
        od_jw_add(a, "attributes", interactive_bytes(&r, 7, attr[i], count[i]));
        od_jw_push(atlases, a);
    }
    od_jw_add(doc, "atlases", atlases);
    od_jw *images = od_jw_array();
    for (unsigned i = 0; i < 31; ++i) {
        unsigned ptr = interactive_word(&r, 7, 0xa008+2*i);
        unsigned atlas = interactive_byte(&r, 7, ptr), dims = interactive_byte(&r, 7, ptr+1);
        unsigned cols = ((dims>>4)&7)+1, rows = (dims&7)+1;
        if (ptr < 0xa000 || ptr+2+cols*rows > 0xc000 || atlas > 1) r.bad = 1;
        od_jw *m = od_jw_object();
        od_jw_add(m, "atlas_id", od_jw_integer(atlas));
        od_jw_add(m, "columns", od_jw_integer(cols));
        od_jw_add(m, "rows", od_jw_integer(rows));
        od_jw_add(m, "header_storage_low", od_jw_integer(ptr&255));
        for (unsigned k = 0; k < cols*rows && !r.bad; ++k)
            if (interactive_byte(&r, 7, ptr+2+k) >= count[atlas]) r.bad = 1;
        od_jw_add(m, "metatiles", interactive_bytes(&r, 7, ptr+2, cols*rows));
        od_jw_push(images, m);
    }
    od_jw_add(doc, "images", images);
    if (r.bad) {
        od_jw_free(doc); set_err(err, cap, "invalid EU transfer image geometry");
        return OD_ERR_FORMAT;
    }
    return write_doc(dir, "screens/transfer-structure.json", doc, 1, err, cap) ? OD_ERR_IO : OD_OK;
}

/* Reconstruct the room initialization: the mask and commands expand into the
 * grid, the cells map through the tile tables, and the 4x4 blocks are
 * overlaid from a list of records. */
/* The Boss's sprite CHR bank. Every pose of the boss starts with a 3-byte
 * header (anchor x, anchor y, bank); the third byte is the bank the boss is
 * drawn from in combat. (The room set-up table read before gave the HUD bank.)
 * Walk the poses in their stored order: header, then the control stream up to
 * its end (0x40) or sub-pose jump (0x41 + pointer); the next header follows.
 * The 0xFF padding closes the data. All headers must agree. 0 on failure. */
static unsigned boss_sprite_bank(const uint8_t *prg)
{
    const unsigned base = 0xC000 + (0x9D13 - 0x8000), end = 0xC000 + (0x9F63 - 0x8000);
    unsigned at = base, bank = 0, headers = 0;
    while (at + 3 <= end + 1) {
        if (prg[at] == 0xFF && prg[at + 1] == 0xFF && prg[at + 2] == 0xFF)
            break;                         /* padding after the last pose */
        unsigned header_bank = prg[at + 2];
        if (headers > 0 && header_bank != bank)
            return 0;
        bank = header_bank;
        headers++;
        at += 3;
        int closed = 0;
        while (!closed && at <= end) {
            uint8_t c = prg[at++];
            if (c >= 0x80)
                at++;                      /* tile + attribute */
            else if (c >= 0x40) {
                switch (c & 3) {
                case 0: closed = 1; break;              /* end of the pose */
                case 1: at += 2; closed = 1; break;     /* jump to a sub-pose */
                default: at++; break;                   /* y-/x+ operand */
                }
            }
        }
        if (!closed)
            return 0;
    }
    return headers > 0 ? bank : 0;
}

static void arena_init(const od_rom *rom, uint8_t grid[256], uint8_t nt[2048])
{
    const uint8_t *p = rom->prg;
    memset(grid, 0, 256); /* the grid starts clear */
    memset(nt, 0, 2048); /* both nametables and attributes start clear */
    for (unsigned i = 0; i < 240; i++)
        grid[i] = (p[0x5AD7 + i / 8] >> (7 - i % 8)) & 1;
    unsigned at = 0x5AF5;
    while (p[at]) {
        unsigned type = p[at++], count = p[at++];
        for (unsigned i = 0; i < count; i++)
            grid[p[at++]] = (uint8_t)type;
    }
    for (unsigned i = 0; i < 240; i++) {
        unsigned x = i % 16, y = i / 16, type = grid[i] & 15;
        /* The left neighbour is 0 at the row start; the right neighbour of
         * the last column wraps to column zero of the SAME row. */
        unsigned neighbour[2] = {x ? grid[i - 1] & 15 : 0,
                                grid[y * 16 + (x + 1) % 16] & 15};
        for (unsigned dx = 0; dx < 2; dx++) {
            unsigned index = p[0x52C0 + dx * 0x100 + type * 16 + neighbour[dx]];
            unsigned tile = p[0x54C0 + index];
            if (tile == 0x80)
                tile = p[0x5529 + 9]; /* ROUND 10-10 substitution */
            tile += dx;
            nt[y * 64 + x * 2 + dx] = (uint8_t)tile;
            nt[y * 64 + 32 + x * 2 + dx] = (uint8_t)(tile + 2);
        }
        unsigned attr = p[0x54D9 + grid[i]];
        nt[960 + (y / 2) * 8 + x / 2] |= (uint8_t)(attr << ((x & 1) * 2 + (y & 1) * 4));
    }
    at = 0xD78D;
    while (p[at] != 0xFF) {
        unsigned cell = p[at++], block = p[at++];
        unsigned addr = (cell & 0x38) * 16 + (cell & 7) * 4;
        const uint8_t *b = p + 0x5B41 + block * 17;
        for (unsigned y = 0; y < 4; y++)
            memcpy(nt + addr + y * 32, b + 1 + y * 4, 4);
        nt[960 + cell] = b[0];
    }
    /* Eight repeats of four tiles in one nametable row (horizontal mirroring
     * puts it at CIRAM 0x0740). */
    for (unsigned x = 0; x < 32; x++)
        nt[1856 + x] = p[0xBE9 + (x % 4) * 5];
    /* Restore the HUD, then the round and clock uploads. The decimal glyph
     * base is a constant of the image. */
    memcpy(nt + 1922, p + 0x27F4, p[0x27F1] & 0x3F);
    unsigned glyph = p[0x8C7];
    nt[1930] = nt[1933] = (uint8_t)(glyph + 1);
    nt[1931] = nt[1934] = (uint8_t)glyph;
    for (unsigned i = 1942; i <= 1949; i++)
        if (i != 1944 && i != 1947)
            nt[i] = (uint8_t)glyph;
}

od_status od_import_arena(const od_rom *rom, const char *dir, char *err, size_t err_cap)
{
    const uint8_t *prg = rom->prg;
    if (import_menu_tables(rom, dir, err, err_cap) != OD_OK)
        return OD_ERR_IO;
    if (import_map(rom, dir, err, err_cap) != OD_OK)
        return OD_ERR_IO;
    od_status interactive_status = import_interactive_setup(rom, dir, err, err_cap);
    if (interactive_status != OD_OK)
        return interactive_status;
    interactive_status = import_transfer_structure(rom, dir, err, err_cap);
    if (interactive_status != OD_OK) return interactive_status;
    uint8_t grid[256], nt[2048], base_palette[32], palette[32];
    arena_init(rom, grid, nt);
    /* Background set 0x0F and sprite set 0x0B from the palette pointer
     * tables. The package stores the canonical fully-lit room (brightness 15):
     * (base & 0x0F) | fade_lut[(base & 0x30) + brightness]. The runtime
     * compositor uses base_palette and its live fade clock instead. */
    unsigned bg = menu_word(rom, 0xE71E) + prg[0x1F082] * 16;
    unsigned sp = menu_word(rom, 0xE720) + prg[0x1F086] * 16;
    for (unsigned i = 0; i < 32; i++) {
        base_palette[i] = menu_rd(rom, (i < 16 ? bg : sp) + i % 16);
        palette[i] = (base_palette[i] & 15) | prg[0x1E736 + (base_palette[i] & 0x30) + 15];
        if (i >= 16 && i % 4 == 0)
            palette[i] = 0; /* PPU universal-colour mirror, display model. */
    }

    /* screens/boss-background.json */
    {
        od_jw *doc = od_jw_object();
        od_jw_add(doc, "schema_version", od_jw_integer(1));
        od_jw_add(doc, "id", od_jw_string("boss-background"));
        od_jw_add(doc, "ciram", bytes(nt, 2048));
        od_jw_add(doc, "palette", bytes(palette, 32));
        uint8_t banks[4] = {prg[0x1F02C], prg[0x1F030], prg[0x1F034], prg[0x1F038]};
        od_jw_add(doc, "banks", bytes(banks, 4));
        od_jw_add(doc, "grid", bytes(grid, 256));
        od_jw_add(doc, "base_palette", bytes(base_palette, 32));
        od_jw_add(doc, "fade_lut", bytes(prg + 0x1E736, 64));
        od_jw_add(doc, "lower_banks", bytes(prg + 0xD0A4, 3));
        unsigned boss_bank = boss_sprite_bank(prg);
        if (boss_bank == 0) {
            od_jw_free(doc);
            set_err(err, err_cap, "unrecognised Boss pose headers (sprite bank)");
            return OD_ERR_FORMAT;
        }
        uint8_t sprite_banks[2] = {prg[0x1F021], (uint8_t)boss_bank};
        od_jw_add(doc, "sprite_banks", bytes(sprite_banks, 2));
        od_jw *hashes = od_jw_array();
        for (unsigned i = 0; i < 4; i++) {
            uint8_t digest[32];
            char hex[65];
            od_sha256(rom->chr + banks[i] * 1024, 1024, digest);
            od_sha256_hex(digest, hex);
            od_jw_push(hashes, od_jw_string(hex));
        }
        od_jw_add(doc, "chr_sha256", hashes);
        od_jw_add(doc, "attack_tiles", bytes(prg + 0x5907, 92));
        if (write_doc(dir, "screens/boss-background.json", doc, 1, err, err_cap) != 0)
            return OD_ERR_IO;
    }

    /* screens/boss-arena.json */
    {
        od_jw *doc = od_jw_object();
        od_jw_add(doc, "schema_version", od_jw_integer(1));
        od_jw_add(doc, "id", od_jw_string("boss-arena"));
        od_jw_add(doc, "rows", od_jw_integer(16));
        od_jw_add(doc, "cols", od_jw_integer(16));
        od_jw_add(doc, "cells", bytes(grid, 256));
        if (write_doc(dir, "screens/boss-arena.json", doc, 1, err, err_cap) != 0)
            return OD_ERR_IO;
    }

    /* screens/retry.json */
    {
        od_jw *doc = od_jw_object();
        od_jw_add(doc, "schema_version", od_jw_integer(1));
        od_jw_add(doc, "id", od_jw_string("retry"));
        /* Retry upload: one video record written at 0x2784; the cleared
         * three-row band starts at 0x2760. The arrow is the nametable cursor
         * drawn by the retry input loop. */
        uint8_t tiles[96] = {0};
        unsigned n = prg[0x27D3] & 0x3F;
        unsigned addr = prg[0x27D4] | (prg[0x27D5] << 8);
        if (addr < 0x2760 || addr + n > 0x27C0) {
            od_jw_free(doc);
            return OD_ERR_FORMAT;
        }
        memcpy(tiles + addr - 0x2760, prg + 0x27D6, n);
        /* Two cursor records; the first uploads 0xFF to 0x2786 and clears
         * 0x2793. */
        unsigned cursor_addr = prg[0x57C5] | (prg[0x57C6] << 8);
        tiles[cursor_addr - 0x2760] = prg[0x57C7];
        od_jw_add(doc, "tiles", bytes(tiles, sizeof tiles));
        od_jw_add(doc, "palette", bytes(prg + 0x5E00, 4));
        od_jw_add(doc, "cursor_tile", od_jw_integer(prg[0x57C7]));
        uint8_t cols[2] = {(uint8_t)(prg[0x57C5] & 31), (uint8_t)(prg[0x57C9] & 31)};
        od_jw_add(doc, "cursor_cols", bytes(cols, 2));
        if (write_doc(dir, "screens/retry.json", doc, 1, err, err_cap) != 0)
            return OD_ERR_IO;
    }
    return OD_OK;
}

od_status od_import_pack(const od_rom *rom, const char *dir, char *err, size_t cap)
{
    od_jw *doc = od_jw_object();
    od_jw_add(doc, "schema_version", od_jw_integer(2));
    od_jw_add(doc, "id", od_jw_string("base"));
    od_jw_add(doc, "name", od_jw_string("OpenDana base content (EU)"));
    od_jw_add(doc, "version", od_jw_string("1.0.0"));
    od_jw_add(doc, "grid_width", od_jw_integer(16));
    od_jw_add(doc, "grid_height", od_jw_integer(15));
    od_jw_add(doc, "level_count", od_jw_integer(150));
    od_jw *levels = od_jw_array();
    for (int w = 1; w <= 15; w++) {
        char rel[64];
        snprintf(rel, sizeof(rel), "levels/world-%02d.json", w);
        od_jw_push(levels, od_jw_string(rel));
    }
    od_jw_add(doc, "levels", levels);
    od_jw *graphics = od_jw_object();
    od_jw *chr = od_jw_array();
    for (int i = 0; i < 8; i++) {
        char rel[64];
        snprintf(rel, sizeof(rel), "graphics/chr/chr-%02d.png", i);
        od_jw_push(chr, od_jw_string(rel));
    }
    od_jw_add(graphics, "chr", chr);
    od_jw *sprites = od_jw_array();
    od_jw_push(sprites, od_jw_string("graphics/sprites/poses.json"));
    od_jw_add(graphics, "sprites", sprites);
    od_jw_add(doc, "graphics", graphics);

    od_jw *worlds = od_jw_array();
    od_jw_push(worlds, od_jw_string("worlds.json"));
    od_jw_add(doc, "worlds", worlds);
    od_jw *palettes = od_jw_array();
    od_jw_push(palettes, od_jw_string("palettes/master.json"));
    od_jw_add(doc, "palettes", palettes);
    od_jw *audio = od_jw_array();
    od_jw_push(audio, od_jw_string("audio/sequence.json"));
    od_jw_add(doc, "audio", audio);
    od_jw *layout = od_import_audio_layout(rom);
    if (layout == NULL) {
        od_jw_free(doc);
        set_err(err, cap, "sound layout outside PRG");
        return OD_ERR_FORMAT;
    }
    od_jw_add(doc, "audio_layout", layout);
    od_jw *screens = od_jw_array();
    od_jw_push(screens, od_jw_string("screens/boss-arena.json"));
    od_jw_push(screens, od_jw_string("screens/retry.json"));
    od_jw_push(screens, od_jw_string("screens/password.json"));
    od_jw_push(screens, od_jw_string("screens/interactive-setup.json"));
    od_jw_push(screens, od_jw_string("screens/transfer-structure.json"));
    od_jw_push(screens, od_jw_string("screens/hearth.json"));
    od_jw_push(screens, od_jw_string("screens/map.json"));
    od_jw_push(screens, od_jw_string("screens/layouts.json"));
    od_jw_push(screens, od_jw_string("screens/magicword.json"));
    od_jw_add(doc, "screens", screens);
    od_jw *cinematics = od_jw_array();
    od_jw_push(cinematics, od_jw_string("screens/cinematics/castle-appears.json"));
    od_jw_push(cinematics, od_jw_string("screens/cinematics/bootstrap.json"));
    od_jw_push(cinematics, od_jw_string("screens/cinematics/editor-entry.json"));
    od_jw_push(cinematics, od_jw_string("screens/cinematics/castle-round10.json"));
    od_jw_push(cinematics, od_jw_string("screens/cinematics/world-clear.json"));
    od_jw_push(cinematics, od_jw_string("screens/cinematics/goodnight-hidden.json"));
    od_jw_push(cinematics, od_jw_string("screens/cinematics/boss-ending.json"));
    od_jw_push(cinematics, od_jw_string("screens/cinematics/post-ending.json"));
    od_jw_add(doc, "cinematics", cinematics);
    od_jw *engine = od_jw_array();
    od_jw_push(engine, od_jw_string("engine/enemy-tables.json"));
    od_jw_push(engine, od_jw_string("engine/hero-programs.json"));
    od_jw_push(engine, od_jw_string("engine/boss-tables.json"));
    od_jw_push(engine, od_jw_string("engine/rom-data.json"));
    od_jw_push(engine, od_jw_string("engine/editor-data.json"));
    od_jw_push(engine, od_jw_string("engine/scene-data.json"));
    od_jw_push(engine, od_jw_string("engine/presentation-data.json"));
    od_jw_add(doc, "engine", engine);

    return write_doc(dir, "pack.json", doc, 2, err, cap) == 0 ? OD_OK : OD_ERR_IO;
}
