#ifndef OD_EXTRACT_CINEMATICS_H
#define OD_EXTRACT_CINEMATICS_H
#include "rom.h"
/* Writes castle-appears, bootstrap, editor-entry (scene29), castle-round10
 * (worldZERO0..8 -> scene8+2*world), world-clear
 * (worldZERO0..8 -> scene9+2*world, then scene42), goodnight-hidden
 * (ordered scene31/5; minimal17 animation rows), boss-ending
 * (ordered scene26/27;46 animations, palette/shake tables), post-ending
 * (scene7;46 animations, operand-bounded image pairs, unused slots empty) families
 * under screens/cinematics/ in an import staging dir. No editor track/timeline.
 * Also writes version2 static layouts/map bases and magicword/post-goodnight
 * artwork, not captured agendas. Input is the pinned ROM already validated
 * by od_rom_load. No track. */
od_status od_import_cinematics(const od_rom *rom, const char *directory,
                                 char *err, size_t err_cap);
#endif
