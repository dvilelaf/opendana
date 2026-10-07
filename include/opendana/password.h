/* password.h - the "Enter your magicword." screen: codec and entry UI.
 *
 * Everything here is pure C11 with no SDL and no ROM dependency. The model is
 * derived from the game data:
 *
 *   * 24 symbol fields: 20 data, 3 checksum (15 bits), 1 key.
 *   * A 32-symbol alphabet laid out on screen in 3 rows of 10 plus the row
 *     `.  !  forth  back  END` (2 symbols + 3 controls). The 5-bit
 *     symbol->code table is a permutation of 0..31; the code->glyph table maps
 *     each code to its tile.
 *   * Data fields unpack to 10 16-bit words, one per world; each word uses its
 *     low 10 bits as a round-completion bitmap (bit j = round j+1 cleared).
 *   * checksum = sum_{i=0..19} (field[i] - key - i) & 0x1F, a 10-bit value
 *     carried by fields 20..22 as (sum>>0)&31, (sum>>5)&31, (sum>>10)&31.
 *
 * The UI state machine: palette cursor 0..34, field cursor 0..23, A writes a
 * symbol and advances (forth), forth/back move the field cursor, END validates. */
#ifndef OD_PASSWORD_H
#define OD_PASSWORD_H

#include "opendana/common.h"
#include "opendana/rom_data.h"

#define OD_PASSWORD_SYMBOLS 32
#define OD_PASSWORD_FIELD   24 /* 20 data + 3 checksum + 1 key */
#define OD_PASSWORD_WORDS   10
#define OD_PASSWORD_EMPTY   0x20 /* the field starts filled with the blank tile */

/* Controls, stored in the cursor space after the 32 symbols. */
#define OD_PASSWORD_FORTH 32
#define OD_PASSWORD_BACK  33
#define OD_PASSWORD_END   34
#define OD_PASSWORD_CURSORS 35

/* display order -> 5-bit code. A permutation of 0..31. */
const od_rom_data *od_password_data(void);
#define od_password_code (od_password_data()->password_code)
/* 5-bit code -> glyph tile index. */
#define od_password_glyph (od_password_data()->password_glyph)
/* The alphabet as drawn, index-aligned with od_password_code. */
#define od_password_display (od_password_data()->password_display)

/* Map a display character to its 5-bit code, or -1 when it is not a symbol. */
int od_password_code_of(char c);
/* Map a 5-bit code to its display character. */
char od_password_char_of(unsigned code);

/* Decode a 24-character magicword. `words` is always filled when the text has
 * exactly 24 known symbols. Returns 1 when the checksum holds, 0 otherwise
 * (including malformed text). */
int od_password_decode(const char *text, uint16_t words[OD_PASSWORD_WORDS]);

/* Validate the checksum of a 24-character magicword (1 = valid). */
int od_password_check(const char *text);

/* Encode 10 round-bitmaps with a 5-bit key (0..31) into a magicword. The
 * encoder reproduces the exact published strings for the documented cases. */
void od_password_encode(const uint16_t words[OD_PASSWORD_WORDS], unsigned key,
                         char out[OD_PASSWORD_FIELD + 1]);

/* Number of set round bits across the ten words (0..100). */
unsigned od_password_rounds(const uint16_t words[OD_PASSWORD_WORDS]);

/* ------------------------------------------------------------------ UI ----- */

typedef enum {
    OD_PASSWORD_EDITING = 0, /* still entering */
    OD_PASSWORD_ACCEPTED,    /* successful END: switch to the "What next?" menu */
    OD_PASSWORD_REJECTED     /* failed END: error, stay on the screen */
} od_password_result;

/* Password entry state. `field` holds 5-bit symbol codes, the blank code while empty. */
typedef struct {
    uint8_t field[OD_PASSWORD_FIELD];
    unsigned cursor;      /* palette cursor 0..34 */
    unsigned position;    /* field cursor 0..23 */
    unsigned previous;    /* last frame's buttons, for press edges */
    unsigned attempts;    /* number of rejected ENDs */
    od_password_result result;
} od_password_ui;

void od_password_ui_reset(od_password_ui *ui);

/* Advance one tick with the native OD_BTN_* mask. Returns the current
 * result. Once ACCEPTED no further input is consumed (the original leaves
 * the screen); REJECTED keeps the field and lets the user keep editing. */
od_password_result od_password_ui_tick(od_password_ui *ui, unsigned buttons);

/* Validate the current field exactly as the original validator does. */
int od_password_ui_validate(const od_password_ui *ui);

/* Decode the current field into the ten round-bitmaps (0 = malformed/empty). */
int od_password_ui_words(const od_password_ui *ui,
                          uint16_t words[OD_PASSWORD_WORDS]);

/* Move only the field cursor (used by forth/back and by the host). */
void od_password_ui_forth(od_password_ui *ui);
void od_password_ui_back(od_password_ui *ui);

#endif /* OD_PASSWORD_H */
