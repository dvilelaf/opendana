/* input_map.c - pure input translation and reassignment.
 *
 * No SDL, no file descriptors kept open, no global state: everything here is a
 * function of its arguments, which is what makes the mapping testable and what
 * keeps a controller from ever reaching the simulation with anything other than
 * the 8-bit NES mask.
 *
 * Default mapping, by position (D2, owner's decision 2026-10-05):
 *
 *     SOUTH (bottom) -> NES B        EAST (right)  -> NES A
 *     dpad           -> dpad         START/BACK    -> START/SELECT
 *     GUIDE          -> menu action  left stick    -> dpad, deadzone .5, hyst .1
 *
 * Reassignment keeps the mapping a function (one physical control per NES
 * button or action) and is persisted in the `[input]` section of settings.ini.
 */
#include "opendana/ext/input_map.h"

#include "opendana/game.h"
#include "opendana/userdir.h"   /* UTF-8 file access on Windows (C6.36) */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r'; }

static int ascii_lower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 'a';
    return c;
}

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void trim(char *s)
{
    char *p = s;
    while (*p && is_space((unsigned char)*p))
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && is_space((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

/* Deflection in millesimals of full scale (0..1000). */
static int milles(int v)
{
    long a = v < 0 ? -(long)v : (long)v;
    if (a > 32767)
        a = 32767;
    return (int)(a * 1000 / 32767);
}

/* Is the direction `sign` (-1|+1) engaged this frame? Hysteresis: a direction
 * already on stays on until the stick falls below deadzone - hysteresis. */
static int direction_on(int was_on, int value, int sign, int deadzone, int hysteresis)
{
    if (sign * value <= 0)
        return 0;                       /* centred or opposite: release */
    int mag = milles(value);
    if (was_on) {
        int release = deadzone - hysteresis;
        if (release < 0)
            release = 0;
        return mag >= release;
    }
    return mag >= deadzone;
}

/* ------------------------------------------------------------------------- */
/* Defaults                                                                  */
/* ------------------------------------------------------------------------- */

void od_binding_clear(od_binding *b)
{
    b->kind = OD_BIND_NONE;
    b->value = 0;
}

int od_binding_set_nes(od_binding *b, unsigned nes)
{
    if (nes == 0 || (nes & ~0xFFu) != 0)
        return 0;
    b->kind = OD_BIND_NES;
    b->value = (unsigned short)nes;
    return 1;
}

int od_binding_set_action(od_binding *b, od_action a)
{
    if (a <= OD_ACT_NONE || a >= OD_ACTION_COUNT)
        return 0;
    b->kind = OD_BIND_ACTION;
    b->value = (unsigned short)a;
    return 1;
}

void od_pad_map_default(od_pad_map *m)
{
    memset(m, 0, sizeof *m);
    for (int i = 0; i < OD_PAD_COUNT; i++)
        od_binding_clear(&m->bind[i]);
    /* By position (D2): bottom -> B, right -> A. */
    od_binding_set_nes(&m->bind[OD_PAD_SOUTH], OD_BTN_B);
    od_binding_set_nes(&m->bind[OD_PAD_EAST], OD_BTN_A);
    od_binding_set_nes(&m->bind[OD_PAD_START], OD_BTN_START);
    od_binding_set_nes(&m->bind[OD_PAD_BACK], OD_BTN_SELECT);
    od_binding_set_nes(&m->bind[OD_PAD_DPAD_UP], OD_BTN_UP);
    od_binding_set_nes(&m->bind[OD_PAD_DPAD_DOWN], OD_BTN_DOWN);
    od_binding_set_nes(&m->bind[OD_PAD_DPAD_LEFT], OD_BTN_LEFT);
    od_binding_set_nes(&m->bind[OD_PAD_DPAD_RIGHT], OD_BTN_RIGHT);
    od_binding_set_action(&m->bind[OD_PAD_GUIDE], OD_ACT_MENU);
    m->deadzone = 500;
    m->hysteresis = 100;
    m->axis_dpad = 1;
}

void od_key_map_default(od_key_map *m)
{
    memset(m, 0, sizeof *m);
    od_binding_set_nes(&m->bind[OD_KEY_UP], OD_BTN_UP);
    od_binding_set_nes(&m->bind[OD_KEY_DOWN], OD_BTN_DOWN);
    od_binding_set_nes(&m->bind[OD_KEY_LEFT], OD_BTN_LEFT);
    od_binding_set_nes(&m->bind[OD_KEY_RIGHT], OD_BTN_RIGHT);
    od_binding_set_nes(&m->bind[OD_KEY_A], OD_BTN_A);
    od_binding_set_nes(&m->bind[OD_KEY_B], OD_BTN_B);
    od_binding_set_nes(&m->bind[OD_KEY_START], OD_BTN_START);
    od_binding_set_nes(&m->bind[OD_KEY_SELECT], OD_BTN_SELECT);
    od_binding_set_action(&m->bind[OD_KEY_MENU], OD_ACT_MENU);
    od_binding_set_action(&m->bind[OD_KEY_STATE_SAVE], OD_ACT_STATE_SAVE);
    od_binding_set_action(&m->bind[OD_KEY_STATE_LOAD], OD_ACT_STATE_LOAD);
    od_binding_set_action(&m->bind[OD_KEY_SLOT_PREV], OD_ACT_SLOT_PREV);
    od_binding_set_action(&m->bind[OD_KEY_SLOT_NEXT], OD_ACT_SLOT_NEXT);
    od_binding_set_action(&m->bind[OD_KEY_MUTE], OD_ACT_MUTE);
    od_binding_set_action(&m->bind[OD_KEY_RESET], OD_ACT_RESET);
    od_binding_set_action(&m->bind[OD_KEY_FULLSCREEN], OD_ACT_FULLSCREEN);
    od_binding_set_action(&m->bind[OD_KEY_SCREENSHOT], OD_ACT_SCREENSHOT);
    od_binding_set_action(&m->bind[OD_KEY_HELP], OD_ACT_HELP);
    od_binding_set_action(&m->bind[OD_KEY_QUIT], OD_ACT_QUIT);
}

void od_input_config_default(od_input_config *c)
{
    od_pad_map_default(&c->pad);
    od_key_map_default(&c->keymap);
}

/* ------------------------------------------------------------------------- */
/* Translation                                                               */
/* ------------------------------------------------------------------------- */

void od_pad_resolve_axes(od_pad_state *s, const od_pad_map *m)
{
    if (!m->axis_dpad) {
        s->axis_dpad = 0;
        return;
    }
    int dz = m->deadzone;
    int hy = m->hysteresis;
    int left = direction_on((s->axis_dpad & OD_BTN_LEFT) != 0, s->axis_left_x, -1, dz, hy);
    int right = direction_on((s->axis_dpad & OD_BTN_RIGHT) != 0, s->axis_left_x, 1, dz, hy);
    int up = direction_on((s->axis_dpad & OD_BTN_UP) != 0, s->axis_left_y, -1, dz, hy);
    int down = direction_on((s->axis_dpad & OD_BTN_DOWN) != 0, s->axis_left_y, 1, dz, hy);
    unsigned dpad = 0;
    if (left)
        dpad |= OD_BTN_LEFT;
    if (right)
        dpad |= OD_BTN_RIGHT;
    if (up)
        dpad |= OD_BTN_UP;
    if (down)
        dpad |= OD_BTN_DOWN;
    s->axis_dpad = dpad;
}

unsigned od_pad_to_nes(const od_pad_state *s, const od_pad_map *m)
{
    unsigned out = s->axis_dpad & 0xFFu;
    for (int i = 0; i < OD_PAD_COUNT; i++) {
        if ((s->buttons & OD_PAD_BIT((od_pad_button)i)) == 0)
            continue;
        const od_binding *b = &m->bind[i];
        if (b->kind == OD_BIND_NES)
            out |= b->value & 0xFFu;
    }
    return out;
}

unsigned od_pad_ui_buttons(const od_pad_state *s)
{
    if (s == NULL)
        return 0;
    unsigned out = s->axis_dpad & (OD_BTN_UP | OD_BTN_DOWN | OD_BTN_LEFT |
                                   OD_BTN_RIGHT);
    if (OD_PAD_DOWN(s, OD_PAD_DPAD_UP))    out |= OD_BTN_UP;
    if (OD_PAD_DOWN(s, OD_PAD_DPAD_DOWN))  out |= OD_BTN_DOWN;
    if (OD_PAD_DOWN(s, OD_PAD_DPAD_LEFT))  out |= OD_BTN_LEFT;
    if (OD_PAD_DOWN(s, OD_PAD_DPAD_RIGHT)) out |= OD_BTN_RIGHT;
    if (OD_PAD_DOWN(s, OD_PAD_SOUTH) || OD_PAD_DOWN(s, OD_PAD_START))
        out |= OD_BTN_A;
    if (OD_PAD_DOWN(s, OD_PAD_EAST))
        out |= OD_BTN_B;
    return out;
}

unsigned od_pad_actions(const od_pad_state *s, const od_pad_map *m)
{
    unsigned out = 0;
    for (int i = 0; i < OD_PAD_COUNT; i++) {
        if ((s->buttons & OD_PAD_BIT((od_pad_button)i)) == 0)
            continue;
        const od_binding *b = &m->bind[i];
        if (b->kind == OD_BIND_ACTION)
            out |= OD_ACT_BIT(b->value);
    }
    return out;
}

unsigned od_keys_to_nes(unsigned keys, const od_key_map *m)
{
    unsigned out = 0;
    for (int i = 0; i < OD_KEY_COUNT; i++) {
        if ((keys & OD_KEY_BIT((od_key)i)) == 0)
            continue;
        const od_binding *b = &m->bind[i];
        if (b->kind == OD_BIND_NES)
            out |= b->value & 0xFFu;
    }
    return out;
}

unsigned od_keys_actions(unsigned keys, const od_key_map *m)
{
    unsigned out = 0;
    for (int i = 0; i < OD_KEY_COUNT; i++) {
        if ((keys & OD_KEY_BIT((od_key)i)) == 0)
            continue;
        const od_binding *b = &m->bind[i];
        if (b->kind == OD_BIND_ACTION)
            out |= OD_ACT_BIT(b->value);
    }
    return out;
}

int od_actions_flank(unsigned current, unsigned previous, od_action *out, int max)
{
    unsigned edge = current & ~previous;
    int n = 0;
    for (int a = OD_ACT_MENU; a < OD_ACTION_COUNT && n < max; a++) {
        if (edge & OD_ACT_BIT((od_action)a))
            out[n++] = (od_action)a;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Reassignment                                                              */
/* ------------------------------------------------------------------------- */

static void clear_same(od_binding *arr, int count, int except, const od_binding *b)
{
    if (b->kind == OD_BIND_NONE)
        return;
    for (int i = 0; i < count; i++) {
        if (i == except)
            continue;
        if (arr[i].kind == b->kind && arr[i].value == b->value)
            od_binding_clear(&arr[i]);
    }
}

void od_pad_rebind(od_pad_map *m, od_pad_button b, const od_binding *bind)
{
    if (b < 0 || b >= OD_PAD_COUNT)
        return;
    clear_same(m->bind, OD_PAD_COUNT, (int)b, bind);
    m->bind[b] = *bind;
}

void od_key_rebind(od_key_map *m, od_key k, const od_binding *bind)
{
    if (k < 0 || k >= OD_KEY_COUNT)
        return;
    clear_same(m->bind, OD_KEY_COUNT, (int)k, bind);
    m->bind[k] = *bind;
}

/* ------------------------------------------------------------------------- */
/* Names                                                                     */
/* ------------------------------------------------------------------------- */

static const char *const PAD_NAMES[OD_PAD_COUNT] = {
    "south", "east", "west", "north", "back", "guide", "start",
    "lstick", "rstick", "lb", "rb",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right"
};

static const char *const KEY_NAMES[OD_KEY_COUNT] = {
    "up", "down", "left", "right", "a", "b", "start", "select",
    "menu", "state_save", "state_load", "slot_prev", "slot_next",
    "mute", "reset", "fullscreen", "screenshot", "help", "quit"
};

static const char *const ACTION_NAMES[OD_ACTION_COUNT] = {
    "none", "menu", "state_save", "state_load", "slot_prev", "slot_next",
    "mute", "reset", "fullscreen", "screenshot", "help", "quit"
};

static const struct { const char *name; unsigned bit; } NES_NAMES[] = {
    { "nes_a", OD_BTN_A },
    { "nes_b", OD_BTN_B },
    { "nes_select", OD_BTN_SELECT },
    { "nes_start", OD_BTN_START },
    { "nes_up", OD_BTN_UP },
    { "nes_down", OD_BTN_DOWN },
    { "nes_left", OD_BTN_LEFT },
    { "nes_right", OD_BTN_RIGHT },
};

const char *od_pad_button_name(od_pad_button b)
{
    if (b < 0 || b >= OD_PAD_COUNT)
        return "";
    return PAD_NAMES[b];
}

int od_pad_button_from_name(const char *name)
{
    if (name == NULL)
        return -1;
    for (int i = 0; i < OD_PAD_COUNT; i++)
        if (strcmp(name, PAD_NAMES[i]) == 0)
            return i;
    return -1;
}

const char *od_key_name(od_key k)
{
    if (k < 0 || k >= OD_KEY_COUNT)
        return "";
    return KEY_NAMES[k];
}

int od_key_from_name(const char *name)
{
    if (name == NULL)
        return -1;
    for (int i = 0; i < OD_KEY_COUNT; i++)
        if (strcmp(name, KEY_NAMES[i]) == 0)
            return i;
    return -1;
}

const char *od_nes_name(unsigned nes)
{
    for (size_t i = 0; i < sizeof NES_NAMES / sizeof NES_NAMES[0]; i++)
        if (NES_NAMES[i].bit == nes)
            return NES_NAMES[i].name;
    return "";
}

unsigned od_nes_from_name(const char *name)
{
    if (name == NULL)
        return 0;
    for (size_t i = 0; i < sizeof NES_NAMES / sizeof NES_NAMES[0]; i++)
        if (strcmp(name, NES_NAMES[i].name) == 0)
            return NES_NAMES[i].bit;
    return 0;
}

const char *od_action_name(od_action a)
{
    if (a < 0 || a >= OD_ACTION_COUNT)
        return "";
    return ACTION_NAMES[a];
}

od_action od_action_from_name(const char *name)
{
    if (name == NULL)
        return OD_ACT_NONE;
    for (int a = OD_ACT_NONE; a < OD_ACTION_COUNT; a++)
        if (strcmp(name, ACTION_NAMES[a]) == 0)
            return (od_action)a;
    return OD_ACT_NONE;
}

int od_binding_serialize(const od_binding *b, char *out, size_t n)
{
    if (b == NULL || out == NULL || n == 0)
        return 0;
    const char *name = "none";
    if (b->kind == OD_BIND_NES)
        name = od_nes_name(b->value);
    else if (b->kind == OD_BIND_ACTION)
        name = od_action_name(b->value);
    else if (b->kind != OD_BIND_NONE)
        name = "";
    int written = snprintf(out, n, "%s", name);
    return written > 0 && (size_t)written < n;
}

od_status od_binding_parse(const char *text, od_binding *out)
{
    if (text == NULL || out == NULL)
        return OD_ERR_FORMAT;
    char buf[32];
    size_t n = strlen(text);
    if (n >= sizeof buf)
        n = sizeof buf - 1;
    memcpy(buf, text, n);
    buf[n] = '\0';
    trim(buf);
    if (buf[0] == '\0' || strcmp(buf, "none") == 0) {
        od_binding_clear(out);
        return OD_OK;
    }
    unsigned nes = od_nes_from_name(buf);
    if (nes != 0) {
        od_binding_set_nes(out, nes);
        return OD_OK;
    }
    od_action a = od_action_from_name(buf);
    if (a != OD_ACT_NONE) {
        od_binding_set_action(out, a);
        return OD_OK;
    }
    return OD_ERR_FORMAT;
}

/* ------------------------------------------------------------------------- */
/* settings.ini persistence (`[input]` section)                              */
/* ------------------------------------------------------------------------- */

static const char *const PAD_KEYS[OD_PAD_COUNT] = {
    "pad_south", "pad_east", "pad_west", "pad_north", "pad_back", "pad_guide",
    "pad_start", "pad_lstick", "pad_rstick", "pad_lb", "pad_rb",
    "pad_dpad_up", "pad_dpad_down", "pad_dpad_left", "pad_dpad_right"
};

static const char *const KEY_KEYS[OD_KEY_COUNT] = {
    "key_up", "key_down", "key_left", "key_right", "key_a", "key_b", "key_start",
    "key_select", "key_menu", "key_state_save", "key_state_load", "key_slot_prev",
    "key_slot_next", "key_mute", "key_reset", "key_fullscreen", "key_screenshot",
    "key_help", "key_quit"
};

#define DEADZONE_KEY "pad_deadzone"
#define HYSTERESIS_KEY "pad_hysteresis"
#define AXIS_DPAD_KEY "pad_axis_dpad"

/* Is `key` one of the settings.ini keys this module owns? */
static int owned_key(const char *key)
{
    for (int i = 0; i < OD_PAD_COUNT; i++)
        if (strcmp(key, PAD_KEYS[i]) == 0)
            return 1;
    for (int i = 0; i < OD_KEY_COUNT; i++)
        if (strcmp(key, KEY_KEYS[i]) == 0)
            return 1;
    return strcmp(key, DEADZONE_KEY) == 0 || strcmp(key, HYSTERESIS_KEY) == 0 ||
           strcmp(key, AXIS_DPAD_KEY) == 0;
}

static void apply_kv(od_input_config *c, const char *key, const char *value)
{
    for (int i = 0; i < OD_PAD_COUNT; i++) {
        if (strcmp(key, PAD_KEYS[i]) == 0) {
            od_binding_parse(value, &c->pad.bind[i]);
            return;
        }
    }
    for (int i = 0; i < OD_KEY_COUNT; i++) {
        if (strcmp(key, KEY_KEYS[i]) == 0) {
            od_binding_parse(value, &c->keymap.bind[i]);
            return;
        }
    }
    if (strcmp(key, DEADZONE_KEY) == 0) {
        int v = atoi(value);
        if (v >= 0 && v <= 1000)
            c->pad.deadzone = v;
    } else if (strcmp(key, HYSTERESIS_KEY) == 0) {
        int v = atoi(value);
        if (v >= 0 && v <= 1000)
            c->pad.hysteresis = v;
    } else if (strcmp(key, AXIS_DPAD_KEY) == 0) {
        int v = atoi(value);
        if (v == 0 || v == 1)
            c->pad.axis_dpad = v;
    }
}

/* C6.32 (UI-25): read in chunks up to 1 MiB. Trusting ftell() made a
 * directory (or a pipe) a 2^63-byte allocation. */
static char *read_file(const char *path, size_t *size)
{
    FILE *fp = od_userdir_fopen(path, "rb");
    if (fp == NULL)
        return NULL;
    const size_t limit = 1024 * 1024;
    size_t len = 0, cap = 4096;
    char *data = malloc(cap + 1);
    while (data != NULL) {
        if (len == cap) {
            if (cap >= limit) {
                free(data);
                data = NULL;
                break;
            }
            char *grown = realloc(data, cap * 2 + 1);
            if (grown == NULL) {
                free(data);
                data = NULL;
                break;
            }
            data = grown;
            cap *= 2;
        }
        size_t got = fread(data + len, 1, cap - len, fp);
        len += got;
        if (got == 0)
            break;
    }
    if (data != NULL && ferror(fp)) {
        free(data);
        data = NULL;
    }
    fclose(fp);
    if (data == NULL)
        return NULL;
    data[len] = '\0';
    if (size)
        *size = len;
    return data;
}

od_status od_input_config_load(od_input_config *c, const char *path)
{
    if (c == NULL)
        return OD_ERR_RANGE;
    od_input_config_default(c);
    if (path == NULL)
        return OD_OK;
    size_t size = 0;
    char *data = read_file(path, &size);
    if (data == NULL)
        return OD_OK;                    /* missing file: defaults stand */
    char section[32] = "";
    for (char *p = data; *p;) {
        char *nl = strchr(p, '\n');
        if (nl != NULL)
            *nl = '\0';
        char *line = p;
        trim(line);
        if (line[0] != '\0' && line[0] != '#' && line[0] != ';') {
            if (line[0] == '[') {
                char *end = strchr(line, ']');
                size_t n = end ? (size_t)(end - line - 1) : 0;
                if (n >= sizeof section)
                    n = sizeof section - 1;
                memcpy(section, line + 1, n);
                section[n] = '\0';
                trim(section);
            } else if (ieq(section, "input")) {
                char *eq = strchr(line, '=');
                if (eq != NULL) {
                    *eq = '\0';
                    char *key = line;
                    char *value = eq + 1;
                    trim(key);
                    trim(value);
                    apply_kv(c, key, value);
                }
            }
        }
        if (nl == NULL)
            break;
        p = nl + 1;
    }
    free(data);
    return OD_OK;
}

/* ---- saving (line-preserving INI update, atomic write) ---------------- */

typedef struct {
    char *data;
    size_t len, cap;
} strbuf;

static int sb_reserve(strbuf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return 1;
    size_t cap = b->cap ? b->cap : 512;
    while (cap < b->len + extra + 1)
        cap *= 2;
    char *p = realloc(b->data, cap);
    if (p == NULL)
        return 0;
    b->data = p;
    b->cap = cap;
    return 1;
}

static int sb_puts(strbuf *b, const char *s)
{
    size_t n = strlen(s);
    if (!sb_reserve(b, n))
        return 0;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static int sb_printf(strbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[128];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp)
        return 0;
    return sb_puts(b, tmp);
}

static void sb_ensure_nl(strbuf *b)
{
    if (b->len > 0 && b->data[b->len - 1] != '\n')
        sb_puts(b, "\n");
}

static int emit_block(strbuf *b, const od_input_config *c)
{
    for (int i = 0; i < OD_PAD_COUNT; i++) {
        char v[32];
        od_binding_serialize(&c->pad.bind[i], v, sizeof v);
        if (!sb_printf(b, "%s = %s\n", PAD_KEYS[i], v))
            return 0;
    }
    if (!sb_printf(b, "%s = %d\n", DEADZONE_KEY, c->pad.deadzone))
        return 0;
    if (!sb_printf(b, "%s = %d\n", HYSTERESIS_KEY, c->pad.hysteresis))
        return 0;
    if (!sb_printf(b, "%s = %d\n", AXIS_DPAD_KEY, c->pad.axis_dpad))
        return 0;
    for (int i = 0; i < OD_KEY_COUNT; i++) {
        char v[32];
        od_binding_serialize(&c->keymap.bind[i], v, sizeof v);
        if (!sb_printf(b, "%s = %s\n", KEY_KEYS[i], v))
            return 0;
    }
    return 1;
}

static int write_atomic(const char *path, const char *data, size_t size)
{
    size_t n = strlen(path);
    char *tmp = malloc(n + 5);
    if (tmp == NULL)
        return OD_ERR_NOMEM;
    memcpy(tmp, path, n);
    memcpy(tmp + n, ".tmp", 5);
    FILE *fp = od_userdir_fopen(tmp, "wb");
    if (fp == NULL) {
        free(tmp);
        return OD_ERR_IO;
    }
    size_t wrote = fwrite(data, 1, size, fp);
    int ok = fflush(fp) == 0 && wrote == size;
    if (fclose(fp) != 0)
        ok = 0;
    if (!ok || od_userdir_rename(tmp, path) != 0) {
        od_userdir_remove(tmp);
        free(tmp);
        return OD_ERR_IO;
    }
    free(tmp);
    return OD_OK;
}

/* Merge the [input] block of `c` into `existing` (owned, may be NULL) and
 * write the result to `path` in one atomic replace. */
static od_status save_merged(const od_input_config *c, const char *path,
                              char *existing)
{
    strbuf out = {0};
    int wrote_block = 0;
    int saw_input = 0;

    if (existing != NULL) {
        int in_input = 0;
        for (char *p = existing; *p;) {
            char *nl = strchr(p, '\n');
            if (nl != NULL)
                *nl = '\0';
            /* Work on a trimmed copy for classification, keep the original. */
            char probe[256];
            size_t pl = strlen(p);
            if (pl >= sizeof probe)
                pl = sizeof probe - 1;
            memcpy(probe, p, pl);
            probe[pl] = '\0';
            trim(probe);

            if (probe[0] == '[') {
                if (in_input && !wrote_block) {
                    if (!emit_block(&out, c)) {
                        free(existing);
                        free(out.data);
                        return OD_ERR_NOMEM;
                    }
                    wrote_block = 1;
                }
                sb_ensure_nl(&out);
                sb_puts(&out, p);
                sb_puts(&out, "\n");
                char *end = strchr(probe, ']');
                if (end)
                    *end = '\0';
                in_input = ieq(probe + 1, "input");
                if (in_input)
                    saw_input = 1;
            } else if (in_input) {
                char key[64];
                const char *eq = strchr(probe, '=');
                size_t kn = eq ? (size_t)(eq - probe) : strlen(probe);
                if (kn >= sizeof key)
                    kn = sizeof key - 1;
                memcpy(key, probe, kn);
                key[kn] = '\0';
                trim(key);
                if (!owned_key(key)) {
                    sb_ensure_nl(&out);
                    sb_puts(&out, p);
                    sb_puts(&out, "\n");
                }
            } else {
                sb_ensure_nl(&out);
                sb_puts(&out, p);
                sb_puts(&out, "\n");
            }
            if (nl == NULL)
                break;
            p = nl + 1;
        }
        if (in_input && !wrote_block) {
            if (!emit_block(&out, c)) {
                free(existing);
                free(out.data);
                return OD_ERR_NOMEM;
            }
            wrote_block = 1;
        }
        free(existing);
    }

    if (!saw_input) {
        sb_ensure_nl(&out);
        sb_puts(&out, "[input]\n");
        if (!emit_block(&out, c)) {
            free(out.data);
            return OD_ERR_NOMEM;
        }
    }
    if (out.data == NULL) {
        out.data = malloc(1);
        if (out.data == NULL)
            return OD_ERR_NOMEM;
        out.data[0] = '\0';
        out.len = 0;
    }
    od_status st = write_atomic(path, out.data, out.len);
    free(out.data);
    return st;
}

od_status od_input_config_save(const od_input_config *c, const char *path)
{
    if (c == NULL || path == NULL)
        return OD_ERR_RANGE;
    size_t size = 0;
    return save_merged(c, path, read_file(path, &size));
}

od_status od_input_config_save_with(const od_input_config *c, const char *path,
                                      const char *base_text)
{
    if (c == NULL || path == NULL || base_text == NULL)
        return OD_ERR_RANGE;
    size_t n = strlen(base_text);
    char *copy = malloc(n + 1);
    if (copy == NULL)
        return OD_ERR_NOMEM;
    memcpy(copy, base_text, n + 1);
    return save_merged(c, path, copy);
}
