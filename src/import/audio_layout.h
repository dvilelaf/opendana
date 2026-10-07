/* audio_layout.h - the sound layout the importer writes into pack.json. */
#ifndef OD_IMPORT_AUDIO_LAYOUT_H
#define OD_IMPORT_AUDIO_LAYOUT_H

#include "jsonw.h"
#include "rom.h"

/* The `audio_layout` object, or NULL when the image is too short. */
od_jw *od_import_audio_layout(const od_rom *rom);

#endif
