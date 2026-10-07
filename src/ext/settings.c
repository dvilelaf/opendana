/* settings.c - persistent settings for the modern interface (C5.1).
 *
 * See include/opendana/ext/settings.h for the contract. Pure C11: stdio, stdlib and
 * string only, no SDL and no renderer, so it builds and is tested everywhere. */
#include "opendana/ext/settings.h"
#include "opendana/userdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small text helpers ------------------------------------------------- */

static int ascii_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0)
        return;
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Parse a boolean: 1/0, true/false, yes/no, on/off (case-insensitive). */
static int parse_bool(const char *v, int *out)
{
    if (ascii_ieq(v, "1") || ascii_ieq(v, "true") ||
        ascii_ieq(v, "yes") || ascii_ieq(v, "on")) {
        *out = 1;
        return 1;
    }
    if (ascii_ieq(v, "0") || ascii_ieq(v, "false") ||
        ascii_ieq(v, "no") || ascii_ieq(v, "off")) {
        *out = 0;
        return 1;
    }
    return 0;
}

/* Strict decimal integer (optional sign) with full-string consumption. */
static int parse_int(const char *v, long *out)
{
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v || (end && *end != '\0'))
        return 0;
    *out = n;
    return 1;
}

void od_settings_defaults(od_settings *s)
{
    if (s == NULL)
        return;
    memset(s, 0, sizeof *s);
    s->fullscreen = 0;
    s->integer_scale = 1;
    s->aspect_87 = 0;
    s->pause_on_focus_loss = 1;
    s->muted = 0;
    s->volume = OD_SETTINGS_VOLUME_MAX;
    s->states_slots = 8;
    s->states_current = 0;
    copy_str(s->language, sizeof s->language, "en");
    s->controls_hint_shown = 0;
    s->desktop_prompted = 0;
}

/* ---- parsing ------------------------------------------------------------ */

od_status od_settings_parse(od_settings *s, const char *text, size_t len)
{
    if (s == NULL || text == NULL)
        return OD_ERR_RANGE;

    s->warnings = 0;
    s->changed = 0;

    char section[16] = "";
    size_t i = 0;
    while (i < (len ? len : strlen(text))) {
        /* One line, trimmed of CR/LF and surrounding blanks. */
        size_t line_start = i;
        while (i < (len ? len : strlen(text)) && text[i] != '\n')
            i++;
        size_t line_end = i;
        if (i < (len ? len : strlen(text)))
            i++; /* skip '\n' */
        while (line_end > line_start &&
               (text[line_end - 1] == '\r' || text[line_end - 1] == ' ' ||
                text[line_end - 1] == '\t'))
            line_end--;
        while (line_start < line_end &&
               (text[line_start] == ' ' || text[line_start] == '\t'))
            line_start++;

        size_t n = line_end - line_start;
        if (n == 0 || text[line_start] == '#' || text[line_start] == ';')
            continue;

        char line[160];
        if (n >= sizeof line)
            n = sizeof line - 1;
        memcpy(line, text + line_start, n);
        line[n] = '\0';

        if (line[0] == '[') {
            char *close = strchr(line, ']');
            if (close == NULL) {
                s->warnings++;
                continue;
            }
            *close = '\0';
            copy_str(section, sizeof section, line + 1);
            continue;
        }
        /* C6.32 (UI-16): another module's section (the [input] bindings of
         * input_map.c share this file) is not ours to judge; a section nobody
         * owns still counts as invalid. */
        if (ascii_ieq(section, "input"))
            continue;

        char *eq = strchr(line, '=');
        if (eq == NULL) {
            s->warnings++;
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        /* trim key/value */
        while (*key == ' ' || *key == '\t') key++;
        char *kend = key + strlen(key);
        while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t')) *--kend = '\0';
        while (*value == ' ' || *value == '\t') value++;
        char *vend = value + strlen(value);
        while (vend > value && (vend[-1] == ' ' || vend[-1] == '\t')) *--vend = '\0';

        long num = 0;
        int b = 0;
        int ok = 0;

        if (ascii_ieq(section, "audio")) {
            if (ascii_ieq(key, "muted")) {
                if (parse_bool(value, &b)) { s->muted = b; ok = 1; }
            } else if (ascii_ieq(key, "volume")) {
                if (parse_int(value, &num) && num >= 0 && num <= OD_SETTINGS_VOLUME_MAX) {
                    s->volume = (int)num; ok = 1;
                }
            }
        } else if (ascii_ieq(section, "video")) {
            if (ascii_ieq(key, "fullscreen")) {
                if (parse_bool(value, &b)) { s->fullscreen = b; ok = 1; }
            } else if (ascii_ieq(key, "integer_scale")) {
                if (parse_bool(value, &b)) { s->integer_scale = b; ok = 1; }
            } else if (ascii_ieq(key, "aspect_87")) {
                if (parse_bool(value, &b)) { s->aspect_87 = b; ok = 1; }
            } else if (ascii_ieq(key, "pause_on_focus_loss")) {
                if (parse_bool(value, &b)) { s->pause_on_focus_loss = b; ok = 1; }
            }
        } else if (ascii_ieq(section, "states")) {
            if (ascii_ieq(key, "slots")) {
                if (parse_int(value, &num) && num >= 1 && num <= OD_SETTINGS_STATES_MAX) {
                    s->states_slots = (int)num; ok = 1;
                }
            } else if (ascii_ieq(key, "current")) {
                if (parse_int(value, &num) && num >= 0 &&
                    num < OD_SETTINGS_STATES_MAX) {
                    s->states_current = (int)num; ok = 1;
                }
            }
        } else if (ascii_ieq(section, "ui")) {
            if (ascii_ieq(key, "language")) {
                if (value[0] != '\0' && strlen(value) < sizeof s->language) {
                    copy_str(s->language, sizeof s->language, value);
                    ok = 1;
                }
            } else if (ascii_ieq(key, "desktop_prompted")) {
                if (parse_bool(value, &b)) { s->desktop_prompted = b; ok = 1; }
            } else if (ascii_ieq(key, "controls_hint_shown")) {
                if (parse_bool(value, &b)) { s->controls_hint_shown = b; ok = 1; }
            }
        }

        if (ok)
            s->changed = 1;
        else
            s->warnings++;
    }

    /* Keep the current slot coherent after either key changed. */
    if (s->states_slots < 1)
        s->states_slots = 1;
    if (s->states_slots > OD_SETTINGS_STATES_MAX)
        s->states_slots = OD_SETTINGS_STATES_MAX;
    if (s->states_current < 0)
        s->states_current = 0;
    if (s->states_current >= s->states_slots)
        s->states_current = s->states_slots - 1;

    return OD_OK;
}

/* ---- formatting / IO ---------------------------------------------------- */

int od_settings_format(const od_settings *s, char *out, size_t cap)
{
    if (s == NULL || out == NULL || cap == 0)
        return 0;
    int n = snprintf(out, cap,
        "# OpenDana settings. Edit freely;\n"
        "# invalid entries fall back to the defaults.\n"
        "[video]\n"
        "fullscreen = %d\n"
        "integer_scale = %d\n"
        "aspect_87 = %d\n"
        "pause_on_focus_loss = %d\n"
        "[audio]\n"
        "muted = %d\n"
        "volume = %d\n"
        "[states]\n"
        "slots = %d\n"
        "current = %d\n"
        "[ui]\n"
        "language = %s\n"
        "controls_hint_shown = %d\n"
        "desktop_prompted = %d\n",
        s->fullscreen ? 1 : 0, s->integer_scale ? 1 : 0, s->aspect_87 ? 1 : 0,
        s->pause_on_focus_loss ? 1 : 0, s->muted ? 1 : 0,
        s->volume, s->states_slots, s->states_current, s->language,
        s->controls_hint_shown ? 1 : 0, s->desktop_prompted ? 1 : 0);
    if (n < 0 || (size_t)n >= cap)
        return 0;
    return n;
}

od_status od_settings_load(od_settings *s, const char *path)
{
    if (s == NULL || path == NULL)
        return OD_ERR_RANGE;

    od_settings_defaults(s);

    FILE *f = od_userdir_fopen(path, "rb");
    if (f == NULL)
        return OD_OK; /* fresh install: defaults */

    char *buf = NULL;
    size_t len = 0, cap = 0;
    char chunk[1024];
    size_t got;
    od_status st = OD_OK;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (len + got + 1 > cap) {
            size_t ncap = (len + got + 1) * 2 + 256;
            char *grown = realloc(buf, ncap);
            if (grown == NULL) {
                free(buf);
                fclose(f);
                return OD_ERR_IO;
            }
            buf = grown;
            cap = ncap;
        }
        memcpy(buf + len, chunk, got);
        len += got;
    }
    if (ferror(f))
        st = OD_ERR_IO;
    fclose(f);

    if (st != OD_OK) {
        free(buf);
        return st;
    }
    if (buf == NULL) {
        buf = malloc(1);
        if (buf == NULL)
            return OD_ERR_IO;
    }
    buf[len] = '\0';

    st = od_settings_parse(s, buf, len);
    free(buf);
    return st;
}

od_status od_settings_save(const od_settings *s, const char *path)
{
    if (s == NULL || path == NULL)
        return OD_ERR_RANGE;

    char out[1024];
    int n = od_settings_format(s, out, sizeof out);
    if (n <= 0)
        return OD_ERR_RANGE;

    /* C6.31: the shared atomic writer (temporary file + replace). The old
     * Windows fallback removed settings.ini before the second rename. */
    return od_userdir_write_atomic(path, out, (size_t)n);
}

void od_settings_default_path(char *out, size_t cap)
{
    if (out == NULL || cap == 0)
        return;
    /* C6.31 (PUB-12/UI-19): the shared per-user config directory (APPDATA on
     * Windows, Application Support on macOS, XDG elsewhere). */
    if (od_userdir_path(OD_USERDIR_CONFIG, "settings.ini", out, cap) == OD_OK)
        return;
    snprintf(out, cap, "opendana-settings.ini");
}

/* ---- audio gain --------------------------------------------------------- */

int od_settings_gain_permille(const od_settings *s)
{
    if (s == NULL || s->muted)
        return 0;
    int v = s->volume;
    if (v < 0)
        v = 0;
    if (v > OD_SETTINGS_VOLUME_MAX)
        v = OD_SETTINGS_VOLUME_MAX;
    return v * 10;
}

void od_audio_apply_gain(int16_t *samples, size_t count, int gain_permille)
{
    if (samples == NULL)
        return;
    if (gain_permille < 0)
        gain_permille = 0;
    if (gain_permille > 1000)
        gain_permille = 1000;
    if (gain_permille == 1000)
        return;
    for (size_t i = 0; i < count; i++)
        samples[i] = (int16_t)(((int32_t)samples[i] * gain_permille) / 1000);
}
