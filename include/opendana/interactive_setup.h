/* ROM-derived static interactive artwork; no phase/tick/recorded display data.
 * C5.7 owns scheduling, scene readiness, CHR mapping and OAM publication. */
#ifndef OD_INTERACTIVE_SETUP_H
#define OD_INTERACTIVE_SETUP_H

#include "opendana/platform.h"
#include "opendana/scene_whatnext.h"

typedef struct {
    uint8_t password_chr[6], password_palette_buffer[32];
    od_menu_record password_field_frame[4], password_choice_panel[8];
    uint8_t password_field_cursor_x[24], password_palette_cursor_oam_yx[35][2];
    uint8_t editor_chr[6], editor_palette_buffer[32];
    od_menu_record editor_initial_layout[27], editor_toolbar_layout[8];
    unsigned editor_entry_cinematic;
} od_interactive_setup;

typedef struct od_interactive_setup_stage od_interactive_setup_stage;
struct cJSON;

/* Validate a complete document. On error *out and public state are untouched;
 * on success *out receives a private stage; caller discards any previous one.
 * Number strings/fractions/extra keys/duplicate keys are not accepted. */
od_status od_interactive_setup_prepare_doc(const struct cJSON *doc,
    od_interactive_setup_stage **out);
/* No IO, allocation, lazy getters or failure. Consumes the stage; NULL no-op.
 * Getter identity remains stable across commits; publication is serial. */
void od_interactive_setup_commit(od_interactive_setup_stage *stage);
void od_interactive_setup_discard(od_interactive_setup_stage *stage);
const od_interactive_setup *od_interactive_setup_get(void);
void od_interactive_setup_reset(void);

#endif
