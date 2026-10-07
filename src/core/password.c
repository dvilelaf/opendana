/* password.c - magicword codec and entry UI.
 *
 * Pure C11. Every constant is derived from the imported game data. */
#include "opendana/password.h"
#include "opendana/game.h"

/* Source-compatible array views, including sizeof and address arithmetic.
 * With no package they expose empty storage, not invented vanilla tables.
 * Codec/UI entry points explicitly reject that missing-data state. */
const od_rom_data *od_password_data(void)
{
    static const od_rom_data empty;
    const od_rom_data *data = od_rom_data_get();
    return data ? data : &empty;
}

int od_password_code_of(char c)
{
    if (!od_rom_data_get()) return -1;
    for (int i = 0; i < OD_PASSWORD_SYMBOLS; ++i)
        if (od_password_display[i] == c)
            return od_password_code[i];
    return -1;
}

char od_password_char_of(unsigned code)
{
    if (!od_rom_data_get()) return '?';
    code &= 0x1Fu;
    for (int i = 0; i < OD_PASSWORD_SYMBOLS; ++i)
        if (od_password_code[i] == code)
            return od_password_display[i];
    return '?';
}

/* Parse a magicword into raw 5-bit field codes. Returns 1 on success. */
static int parse(const char *text, uint8_t field[OD_PASSWORD_FIELD])
{
    if (!text)
        return 0;
    for (int i = 0; i < OD_PASSWORD_FIELD; ++i) {
        int code = text[i] ? od_password_code_of(text[i]) : -1;
        if (code < 0)
            return 0;
        field[i] = (uint8_t)code;
    }
    return text[OD_PASSWORD_FIELD] == '\0';
}

static void decode_field(const uint8_t field[OD_PASSWORD_FIELD],
                         uint16_t words[OD_PASSWORD_WORDS])
{
    unsigned key = field[23];
    for (int w = 0; w < OD_PASSWORD_WORDS; ++w) {
        unsigned lo = (field[2 * w]     - key - (unsigned)(2 * w))     & 0x1Fu;
        unsigned hi = (field[2 * w + 1] - key - (unsigned)(2 * w + 1)) & 0x1Fu;
        words[w] = (uint16_t)(lo | (hi << 5));
    }
}

int od_password_check(const char *text)
{
    uint8_t field[OD_PASSWORD_FIELD];
    if (!parse(text, field))
        return 0;
    unsigned key = field[23];
    unsigned sum = 0;
    for (int i = 0; i < 20; ++i)
        sum += (field[i] - key - (unsigned)i) & 0x1Fu;
    unsigned want =
        (((field[20] - key - 20u) & 0x1Fu)      ) |
        (((field[21] - key - 21u) & 0x1Fu) <<  5) |
        (((field[22] - key - 22u) & 0x1Fu) << 10);
    return sum == want;
}

int od_password_decode(const char *text, uint16_t words[OD_PASSWORD_WORDS])
{
    uint8_t field[OD_PASSWORD_FIELD];
    if (!parse(text, field)) {
        for (int w = 0; w < OD_PASSWORD_WORDS; ++w)
            words[w] = 0;
        return 0;
    }
    decode_field(field, words);
    return od_password_check(text);
}

void od_password_encode(const uint16_t words[OD_PASSWORD_WORDS], unsigned key,
                         char out[OD_PASSWORD_FIELD + 1])
{
    if (!od_rom_data_get()) { out[0] = '\0'; return; }
    key &= 0x1Fu;
    uint8_t field[OD_PASSWORD_FIELD];
    unsigned sum = 0;
    for (int w = 0; w < OD_PASSWORD_WORDS; ++w) {
        unsigned word = words[w] & 0x3FFu;
        unsigned lo = word & 0x1Fu;
        unsigned hi = (word >> 5) & 0x1Fu;
        field[2 * w]     = (uint8_t)((lo + key + (unsigned)(2 * w))     & 0x1Fu);
        field[2 * w + 1] = (uint8_t)((hi + key + (unsigned)(2 * w + 1)) & 0x1Fu);
        sum += lo + hi;
    }
    field[20] = (uint8_t)((((sum >>  0) & 0x1Fu) + key + 20u) & 0x1Fu);
    field[21] = (uint8_t)((((sum >>  5) & 0x1Fu) + key + 21u) & 0x1Fu);
    field[22] = (uint8_t)((((sum >> 10) & 0x1Fu) + key + 22u) & 0x1Fu);
    field[23] = (uint8_t)key;
    for (int i = 0; i < OD_PASSWORD_FIELD; ++i)
        out[i] = od_password_char_of(field[i]);
    out[OD_PASSWORD_FIELD] = '\0';
}

unsigned od_password_rounds(const uint16_t words[OD_PASSWORD_WORDS])
{
    unsigned total = 0;
    for (int w = 0; w < OD_PASSWORD_WORDS; ++w) {
        uint16_t bits = words[w] & 0x3FFu;
        while (bits) {
            total += bits & 1u;
            bits >>= 1;
        }
    }
    return total;
}

/* --------------------------------------------------------------- UI ------- */

void od_password_ui_reset(od_password_ui *ui)
{
    for (int i = 0; i < OD_PASSWORD_FIELD; ++i)
        ui->field[i] = OD_PASSWORD_EMPTY;
    ui->cursor = 0;
    ui->position = 0;
    ui->previous = 0;
    ui->attempts = 0;
    ui->result = OD_PASSWORD_EDITING;
}

int od_password_ui_validate(const od_password_ui *ui)
{
    for (int i = 0; i < 20; ++i)
        if (ui->field[i] == OD_PASSWORD_EMPTY)
            return 0;
    unsigned key = ui->field[23];
    unsigned sum = 0;
    for (int i = 0; i < 20; ++i)
        sum += (ui->field[i] - key - (unsigned)i) & 0x1Fu;
    unsigned want =
        (((ui->field[20] - key - 20u) & 0x1Fu)      ) |
        (((ui->field[21] - key - 21u) & 0x1Fu) <<  5) |
        (((ui->field[22] - key - 22u) & 0x1Fu) << 10);
    return sum == want;
}

int od_password_ui_words(const od_password_ui *ui,
                          uint16_t words[OD_PASSWORD_WORDS])
{
    for (int i = 0; i < 20; ++i)
        if (ui->field[i] == OD_PASSWORD_EMPTY) {
            for (int w = 0; w < OD_PASSWORD_WORDS; ++w)
                words[w] = 0;
            return 0;
        }
    decode_field(ui->field, words);
    return 1;
}

void od_password_ui_forth(od_password_ui *ui)
{
    ui->position = (ui->position + 1) % OD_PASSWORD_FIELD;
}

void od_password_ui_back(od_password_ui *ui)
{
    ui->position = (ui->position + OD_PASSWORD_FIELD - 1) % OD_PASSWORD_FIELD;
}

/* Palette navigation: the cursor wraps over 0..34 (32 symbols + forth/back/END). */
static unsigned cursor_right(unsigned c)
{
    c = (c + 1) % OD_PASSWORD_CURSORS;
    return c;
}

static unsigned cursor_left(unsigned c)
{
    return c == 0 ? OD_PASSWORD_END : c - 1;
}

static unsigned cursor_down(unsigned c)
{
    unsigned v = c + 10;
    switch (v) {
    case 33: case 34:               return OD_PASSWORD_FORTH;
    case 35: case 36: case 37:      return OD_PASSWORD_BACK;
    case 38: case 39:               return OD_PASSWORD_END;
    case 42:                        return 2;
    case 43:                        return 5;
    case 44:                        return 8;
    default:                        return v < 34 ? v : v - 40;
    }
}

static unsigned cursor_up(unsigned c)
{
    int v = (int)c - 10;
    switch (v) {
    case -8: case -7: case -6:      return OD_PASSWORD_FORTH;
    case -5: case -4: case -3:      return OD_PASSWORD_BACK;
    case -2: case -1:               return OD_PASSWORD_END;
    case 23:                        return 25;
    case 24:                        return 28;
    default:                        return v >= 0 ? (unsigned)v
                                                    : (unsigned)(v + 40);
    }
}

od_password_result od_password_ui_tick(od_password_ui *ui, unsigned buttons)
{
    if (!od_rom_data_get()) return OD_PASSWORD_REJECTED;
    unsigned pressed = buttons & ~ui->previous;
    ui->previous = buttons;
    if (ui->result == OD_PASSWORD_ACCEPTED)
        return ui->result; /* the original has left this screen */
    ui->result = OD_PASSWORD_EDITING; /* a rejection lasts one tick */

    /* directions move the palette cursor. */
    if (pressed & OD_BTN_RIGHT) ui->cursor = cursor_right(ui->cursor);
    if (pressed & OD_BTN_LEFT)  ui->cursor = cursor_left(ui->cursor);
    if (pressed & OD_BTN_DOWN)  ui->cursor = cursor_down(ui->cursor);
    if (pressed & OD_BTN_UP)    ui->cursor = cursor_up(ui->cursor);

    /* confirm, back and END handling. */
    if (pressed & OD_BTN_A) {
        if (ui->cursor < OD_PASSWORD_SYMBOLS) {
            ui->field[ui->position] = od_password_code[ui->cursor];
            od_password_ui_forth(ui);
        } else if (ui->cursor == OD_PASSWORD_FORTH) {
            od_password_ui_forth(ui);
        } else if (ui->cursor == OD_PASSWORD_BACK) {
            od_password_ui_back(ui);
        } else { /* END */
            if (od_password_ui_validate(ui))
                ui->result = OD_PASSWORD_ACCEPTED;
            else {
                ui->attempts++;
                ui->result = OD_PASSWORD_REJECTED;
            }
        }
    }
    return ui->result;
}
