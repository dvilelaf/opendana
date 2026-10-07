/* settings.h - persistent settings for the modern interface (C11, no SDL).
 *
 * The product keeps its user-tweakable options in a
 * small `key = value` file under the XDG config directory:
 *
 *     $XDG_CONFIG_HOME/opendana/settings.ini   (else $HOME/.config/...)
 *
 * The file is human editable. Sections `[video]`, `[audio]`, `[states]` and
 * `[ui]` group the interface options. Values that are missing or invalid fall
 * back to the compiled default and are counted in `warnings`; a single bad
 * entry never stops the game. The module is pure C11 (stdio only) so it is
 * unit tested without a window.
 *
 * The audio gain is a pure function here too: the SDL device backend applies
 * exactly `od_audio_apply_gain()`, so a muted device receives silence while
 * the simulation and the core PCM are untouched. */
#ifndef OD_EXT_SETTINGS_H
#define OD_EXT_SETTINGS_H

#include "opendana/common.h"
#include "opendana/userdir.h"

#include <stddef.h>
#include <stdint.h>

#define OD_SETTINGS_APP_DIR     OD_USERDIR_APP  /* opendana/userdir.h */
#define OD_SETTINGS_VOLUME_MAX  100
#define OD_SETTINGS_STATES_MAX  16
#define OD_SETTINGS_LANG_MAX    8

typedef struct {
    /* [video] */
    int fullscreen;             /* default 0                              */
    int integer_scale;          /* default 1                              */
    int aspect_87;              /* default 0                              */
    int pause_on_focus_loss;    /* default 1                              */
    /* [audio] */
    int muted;                  /* default 0                              */
    int volume;                 /* 0..100, default 100                    */
    /* [states] (used from C5.3) */
    int states_slots;           /* 1..16, default 8                       */
    int states_current;         /* 0..slots-1, default 0                  */
    /* [ui] */
    char language[OD_SETTINGS_LANG_MAX];  /* default "en" (D6)          */
    int desktop_prompted;       /* C6.46: the applications-menu question was answered; default 0 */
    int controls_hint_shown;    /* C6.40: the one-time "F1: controls" hint was shown; default 0 */

    /* Diagnostics from the last parse/load (never persisted). */
    int warnings;               /* invalid entries ignored                */
    int changed;                /* parse changed at least one value        */
} od_settings;

/* Reset every field to its compiled default. */
void od_settings_defaults(od_settings *s);

/* Parse INI `text` (length `len`; 0 means NUL-terminated) into `s`.
 * Unknown keys/sections and invalid values are ignored and counted in
 * `s->warnings`. Values are applied on top of the current contents, so call
 * od_settings_defaults() first for a clean parse. Returns OD_OK unless the
 * arguments are invalid. */
od_status od_settings_parse(od_settings *s, const char *text, size_t len);

/* Read `path`. A missing file is not an error: `s` is left at its defaults and
 * OD_OK is returned, so a fresh install starts from the defaults. */
od_status od_settings_load(od_settings *s, const char *path);

/* Write `path` atomically (temporary file + rename). The parent directory must
 * exist (the app creates it, as it does for the save file). */
od_status od_settings_save(const od_settings *s, const char *path);

/* Canonical INI text for `s`, NUL-terminated. Returns the length, or 0 when
 * the buffer is too small. */
int od_settings_format(const od_settings *s, char *out, size_t cap);

/* Default settings path: settings.ini in the per-user config directory
 * (opendana/userdir.h: $XDG_CONFIG_HOME/opendana, else ~/.config/...;
 * %APPDATA%\opendana on Windows; ~/Library/Application Support/
 * opendana on macOS), else "opendana-settings.ini". */
void od_settings_default_path(char *out, size_t cap);

/* Linear gain in permille: 0 when muted, else volume*10. */
int od_settings_gain_permille(const od_settings *s);

/* Apply the gain to a mono S16 buffer, exactly as the SDL device callback
 * does. gain 0 produces silence; 1000 leaves the samples unchanged. */
void od_audio_apply_gain(int16_t *samples, size_t count, int gain_permille);

#endif /* OD_EXT_SETTINGS_H */
