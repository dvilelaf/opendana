/* extract_palettes.c - palettes/master.json.
 *
 * The NES master palettes are hardware signal tables, not game content:
 *   - ntsc: the standard 2C02 table;
 *   - pal:  the NESdev 2C07 table (pally v0.24.1, MIT-0, emphasis 0), the one
 *           the EU profile renders with;
 *   - reference: a signal-model table, reproduced by a closed-form
 *           computation.
 * None of these come from ROM bytes.
 */
#include "fsutil.h"
#include "jsonw.h"
#include "rom.h"

#include <math.h>
#include <stdio.h>

static const uint32_t NTSC[64] = {
    0x666666, 0x002A88, 0x1412A7, 0x3B00A4, 0x5C007E, 0x6E0040, 0x6C0600, 0x561D00,
    0x333500, 0x0B4800, 0x005200, 0x004F08, 0x00404D, 0x000000, 0x000000, 0x000000,
    0xADADAD, 0x155FD9, 0x4240FF, 0x7527FE, 0xA01ACC, 0xB71E7B, 0xB53120, 0x994E00,
    0x6B6D00, 0x388700, 0x0C9300, 0x008F32, 0x007C8D, 0x000000, 0x000000, 0x000000,
    0xFFFEFF, 0x64B0FF, 0x9290FF, 0xC676FF, 0xF36AFF, 0xFE6ECC, 0xFE8170, 0xEA9E22,
    0xBCBE00, 0x88D800, 0x5CE430, 0x45E082, 0x48CDDE, 0x4F4F4F, 0x000000, 0x000000,
    0xFFFEFF, 0xC0DFFF, 0xD3D2FF, 0xE8C8FF, 0xFBC2FF, 0xFEC4EA, 0xFECCC5, 0xF7D8A5,
    0xE4E594, 0xCFEF96, 0xBDF4AB, 0xB3F3CC, 0xB5EBF2, 0xB8B8B8, 0x000000, 0x000000,
};

static const uint32_t PAL[64] = {
    0x626262, 0x002365, 0x0D107F, 0x2B017F, 0x450065, 0x540037, 0x540501, 0x451500,
    0x2B2800, 0x0D3700, 0x003F00, 0x003E01, 0x003337, 0x000000, 0x000000, 0x000000,
    0xABABAB, 0x1151AA, 0x3438CE, 0x5C23CE, 0x7F19AA, 0x931A6C, 0x932824, 0x7F3F00,
    0x5C5800, 0x346C00, 0x117700, 0x007524, 0x00676C, 0x000000, 0x000000, 0x000000,
    0xFFFFFF, 0x61A1FB, 0x8489FF, 0xAD74FF, 0xD069FB, 0xE46BBC, 0xE47974, 0xD08F36,
    0xADA812, 0x84BD12, 0x61C836, 0x4DC674, 0x4DB8BC, 0x4E4E4E, 0x000000, 0x000000,
    0xFFFFFF, 0xC4DDFF, 0xD1D3FF, 0xE1CBFF, 0xEFC7FF, 0xF7C8E7, 0xF7CDCB, 0xEFD6B3,
    0xE1DFA5, 0xD1E7A5, 0xC4ECB3, 0xBCEBCB, 0xBCE5E7, 0xB8B8B8, 0x000000, 0x000000,
};

static void reference_palette(uint32_t out[64])
{
    static const double low_tab[4] = {0.350, 0.518, 0.962, 1.550};
    static const double high_tab[4] = {1.094, 1.506, 1.962, 1.962};
    const double pi = 3.14159265358979323846;
    for (int index = 0; index < 64; index++) {
        int hue = index & 15;
        int level = (index >> 4) & 3;
        if (hue > 13)
            level = 1;
        double low = low_tab[level];
        double high = high_tab[level];
        if (hue == 0)
            low = high;
        if (hue > 12)
            high = low;
        double y = 0.0, u = 0.0, v = 0.0;
        for (int phase = 0; phase < 12; phase++) {
            double signal = ((hue + phase) % 12 < 6) ? high : low;
            signal = (signal - 0.518) / (1.962 - 0.518);
            double angle = pi * (phase - 0.5) / 6.0;
            y += signal;
            u += signal * cos(angle);
            v -= signal * sin(angle);
        }
        y /= 12.0;
        u *= 2.0 / 12.0;
        v *= 2.0 / 12.0;
        double colors[3] = {y + 1.140 * v, y - 0.395 * u - 0.581 * v, y + 2.032 * u};
        uint32_t rgb = 0;
        for (int k = 0; k < 3; k++) {
            int c = (int)(colors[k] * 255.0 + 0.5);
            if (c < 0)
                c = 0;
            if (c > 255)
                c = 255;
            rgb = (rgb << 8) | (uint32_t)c;
        }
        out[index] = rgb;
    }
}

static od_jw *colors_node(const uint32_t colors[64])
{
    od_jw *arr = od_jw_array();
    for (int i = 0; i < 64; i++)
        od_jw_push(arr, od_jw_integer((long long)colors[i]));
    return arr;
}

od_status od_import_palettes(const od_rom *rom, const char *dir, char *err, size_t err_cap)
{
    (void)rom;
    uint32_t reference[64];
    reference_palette(reference);

    od_jw *doc = od_jw_object();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    od_jw_add(doc, "id", od_jw_string("master"));
    od_jw *master = od_jw_array();
    struct {
        const char *id;
        const char *name;
        const uint32_t *colors;
    } entries[3] = {
        {"ntsc", "NTSC 2C02", NTSC},
        {"pal", "EU PAL 2C07 reference, emphasis 0", PAL},
        {"reference", "EU console reference, emphasis 0", reference},
    };
    for (int i = 0; i < 3; i++) {
        od_jw *e = od_jw_object();
        od_jw_add(e, "id", od_jw_string(entries[i].id));
        od_jw_add(e, "name", od_jw_string(entries[i].name));
        od_jw_add(e, "colors", colors_node(entries[i].colors));
        od_jw_push(master, e);
    }
    od_jw_add(doc, "master", master);

    char path[4096];
    od_fs_join(path, sizeof(path), dir, "palettes/master.json");
    od_fs_mkdir_parent(path);
    FILE *f = od_fs_fopen(path, "wb");
    if (f == NULL) {
        od_jw_free(doc);
        if (err != NULL && err_cap > 0)
            snprintf(err, err_cap, "cannot write palettes/master.json");
        return OD_ERR_IO;
    }
    od_jw_write(f, doc, 1);
    fputc('\n', f);
    fclose(f);
    od_jw_free(doc);
    return OD_OK;
}
