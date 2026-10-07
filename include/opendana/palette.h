/* palette.h - NES master palettes and the per-screen palette RAM.
 *
 * The master palette maps the PPU's 6-bit colour indices to RGB. The game only
 * stores colour indices; the RGB table is presentation. Two tables are planned
 * (NTSC 2C02 and PAL 2C07).
 */
#ifndef OD_PALETTE_H
#define OD_PALETTE_H

#include "opendana/common.h"

typedef struct {
    const uint32_t *colors; /* 64 entries, 0x00RRGGBB */
    const char *name;
} od_master_palette;

extern const od_master_palette od_master_palette_ntsc;
extern const od_master_palette od_master_palette_pal;
extern const od_master_palette od_master_palette_reference;

/* Fill the three master palettes from a package `palettes/master.json`
 * document (C2.2). Returns OD_ERR_FORMAT unless all three are present and
 * each carries 64 colour entries. */
od_status od_palette_master_load_json(const char *json, size_t len);

/* Same transaction contract as worlds: validate every complete document,
 * last wins. Commit preserves public master objects and colors pointers. */
typedef struct od_palette_content_stage od_palette_content_stage;
od_status od_palette_content_prepare(od_palette_content_stage **out,
    const char *const *json, const size_t *sizes, size_t count);
void od_palette_content_commit(od_palette_content_stage *stage);
void od_palette_content_discard(od_palette_content_stage *stage);

/* PPU palette RAM: 32 NES colour indices. Entries 0..15 are the
 * four background palettes, 16..31 the four sprite palettes. Entry 0 is the
 * universal background colour. */
typedef struct {
    uint8_t ram[32];
} od_palette_ram;

od_status od_palette_ram_load(od_palette_ram *out, const char *path);

/* Resolve one NES colour index to RGB. Indices 0..63 are defined; the mirror
 * region 0x40..0x7F (emphasis) falls back to the base colour. */
void od_master_lookup(const od_master_palette *pal, uint8_t index, uint8_t rgb[3]);

#endif /* OD_PALETTE_H */
