#include "opendana/rom_data.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static od_rom_data installed;
static int have_data;

/* This validates data-program grammar; it does not interpret CPU code. */
static int sequence_valid(const uint8_t *data, size_t size, int loops)
{
    size_t cursor = 0;
    while (cursor < size) {
        unsigned op = data[cursor++];
        if (op < 0x80) continue;
        if (op < 0xf0) {
            if (cursor + 2 > size || data[cursor + 1] >= 8) return 0;
            cursor += 2;
        } else if (op == 0xfb || op == 0xfa) {
            if (cursor >= size) return 0;
            ++cursor;
        } else if (op == 0xfe) {
            return loops && cursor + 2 == size &&
                   data[cursor] == 0xad && data[cursor + 1] == 0x9c;
        } else if (op == 0xff) {
            return !loops && cursor == size;
        } else return 0;
    }
    return 0;
}

static int data_valid(const od_rom_data *data)
{
    uint32_t seen = 0;
    for (unsigned i = 0; i < OD_ROM_PASSWORD_SYMBOLS; ++i) {
        unsigned code = data->password_code[i];
        unsigned char c = (unsigned char)data->password_display[i];
        if (code >= 32 || (seen & (UINT32_C(1) << code)) || c < 33 || c > 126)
            return 0;
        seen |= UINT32_C(1) << code;
        for (unsigned j = 0; j < i; ++j)
            if (data->password_display[j] == (char)c) return 0;
    }
    return data->password_display[32] == '\0' &&
           sequence_valid(data->boss_idle, sizeof data->boss_idle, 1) &&
           sequence_valid(data->boss_hit, sizeof data->boss_hit, 0) &&
           sequence_valid(data->boss_defeat, sizeof data->boss_defeat, 0);
}

static od_status fail(char *err, size_t cap, od_status status, const char *msg)
{
    if (err && cap) snprintf(err, cap, "%s", msg);
    return status;
}

static int bytes_parse(const cJSON *doc, const char *key, uint8_t *out, size_t size)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(doc, key);
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != (int)size) return 0;
    for (size_t i = 0; i < size; ++i) {
        const cJSON *v = cJSON_GetArrayItem(array, (int)i);
        if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= 255))
            return 0;
        unsigned n = (unsigned)v->valuedouble;
        if (v->valuedouble != (double)n) return 0;
        out[i] = (uint8_t)n;
    }
    return 1;
}

od_status od_rom_data_parse(od_rom_data *out, const od_content_vfs *vfs,
                             char *err, size_t cap)
{
    if (err && cap) err[0] = '\0';
    if (!out || !vfs) return fail(err, cap, OD_ERR_FORMAT, "invalid rom-data arguments");
    char *text = NULL;
    size_t size = 0;
    od_status status = od_content_vfs_read(vfs, OD_ROM_DATA_PATH, &text, &size);
    if (status != OD_OK)
        return fail(err, cap, status, "cannot read " OD_ROM_DATA_PATH);
    /* Require the whole document, not a valid prefix followed by garbage. */
    const char *end = NULL;
    cJSON *doc = cJSON_ParseWithLengthOpts(text, size + 1, &end, 1);
    free(text);
    if (!doc) return fail(err, cap, OD_ERR_FORMAT, "invalid rom-data JSON");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(doc, "schema_version");
    const cJSON *display = cJSON_GetObjectItemCaseSensitive(doc, "password_display");
    od_rom_data tmp = {0};
    int valid = cJSON_IsObject(doc) && cJSON_IsNumber(version) &&
        version->valuedouble == 1 && cJSON_IsString(display) &&
        strlen(display->valuestring) == OD_ROM_PASSWORD_SYMBOLS &&
        bytes_parse(doc, "boss_idle", tmp.boss_idle, sizeof tmp.boss_idle) &&
        bytes_parse(doc, "boss_hit", tmp.boss_hit, sizeof tmp.boss_hit) &&
        bytes_parse(doc, "boss_defeat", tmp.boss_defeat, sizeof tmp.boss_defeat) &&
        bytes_parse(doc, "password_code", tmp.password_code, sizeof tmp.password_code) &&
        bytes_parse(doc, "password_glyph", tmp.password_glyph, sizeof tmp.password_glyph);
    if (valid) {
        memcpy(tmp.password_display, display->valuestring, sizeof tmp.password_display);
        valid = data_valid(&tmp);
    }
    cJSON_Delete(doc);
    if (!valid) return fail(err, cap, OD_ERR_FORMAT, "invalid rom-data tables/version");
    *out = tmp;
    return OD_OK;
}

od_status od_rom_data_install(const od_rom_data *data)
{
    if (!data) {
        memset(&installed, 0, sizeof installed);
        have_data = 0;
        return OD_OK;
    }
    if (!data_valid(data)) return OD_ERR_FORMAT;
    installed = *data;
    have_data = 1;
    return OD_OK;
}

const od_rom_data *od_rom_data_get(void)
{
    if (have_data) return &installed;
    char base[1024];
    if (od_content_resolve_base(base, sizeof base, NULL) != OD_OK) return NULL;
    od_content_vfs vfs = {0};
    snprintf(vfs.layers[0].root, sizeof vfs.layers[0].root, "%s", base);
    vfs.count = 1;
    od_rom_data tmp;
    if (od_rom_data_parse(&tmp, &vfs, NULL, 0) != OD_OK) return NULL;
    if (od_rom_data_install(&tmp) != OD_OK) return NULL;
    return &installed;
}

/* ---- sound layout ----------------------------------------------------- */

static od_audio_layout audio_installed;
static int have_audio;

static int layout_valid(const od_audio_layout *l)
{
    if (l->slot_size != 8192 || !l->slot_count || l->slot_count > 6 ||
        !l->window_count || l->window_count > OD_AUDIO_WINDOWS)
        return 0;
    for (unsigned i = 0; i < l->window_count; ++i) {
        const od_audio_window *w = &l->window[i];
        if (w->slot >= 0 && (w->slot < l->first_slot || w->slot >= l->first_slot + l->slot_count))
            return 0;
        if ((unsigned)w->first + l->slot_size > 0x10000u) return 0;
        for (unsigned j = 0; j < i; ++j) {
            unsigned a = w->first, b = l->window[j].first;
            if (a < b + l->slot_size && b < a + l->slot_size) return 0;
        }
    }
    /* Every table must lie wholly inside the first slot. */
    const struct { uint16_t at; unsigned span; } t[] = {
        {l->fade_steps, 16}, {l->song_slot, 120}, {l->song_start, 240}, {l->periods, 32},
        {l->durations, 256}, {l->effects, 256}, {l->volume_patterns, 2}, {l->pitch_steps, 64},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; ++i)
        if (t[i].at + t[i].span > l->slot_size) return 0;
    for (unsigned i = 0; i < OD_AUDIO_WORLD_MUSIC; ++i)
        if (!l->world_music[i] || l->world_music[i] > 119) return 0;
    return 1;
}

static int json_uint(const cJSON *doc, const char *key, unsigned max, unsigned *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(doc, key);
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= max)) return 0;
    *out = (unsigned)v->valuedouble;
    return (double)*out == v->valuedouble;
}

od_status od_audio_layout_parse(od_audio_layout *out, const od_content_vfs *vfs,
                                char *err, size_t cap)
{
    if (err && cap) err[0] = '\0';
    if (!out || !vfs) return fail(err, cap, OD_ERR_FORMAT, "invalid audio-layout arguments");
    char *text = NULL;
    size_t size = 0;
    od_status status = od_content_vfs_read(vfs, OD_AUDIO_LAYOUT_PATH, &text, &size);
    if (status != OD_OK) return fail(err, cap, status, "cannot read " OD_AUDIO_LAYOUT_PATH);
    const char *end = NULL;
    cJSON *pack = cJSON_ParseWithLengthOpts(text, size + 1, &end, 1);
    free(text);
    if (!pack) return fail(err, cap, OD_ERR_FORMAT, "invalid " OD_AUDIO_LAYOUT_PATH);
    /* Synthetic packs may omit the layout; the caller decides what that means. */
    cJSON *doc = cJSON_GetObjectItemCaseSensitive(pack, "audio_layout");
    if (!doc) {
        cJSON_Delete(pack);
        return fail(err, cap, OD_ERR_IO, "no audio_layout in " OD_AUDIO_LAYOUT_PATH);
    }
    od_audio_layout tmp = {0};
    unsigned n = 0;
    int ok = cJSON_IsObject(doc);
    if (ok && json_uint(doc, "slot_size", 65535, &n)) tmp.slot_size = (uint16_t)n; else ok = 0;
    if (ok && json_uint(doc, "first_slot", 255, &n)) tmp.first_slot = (uint8_t)n; else ok = 0;
    if (ok && json_uint(doc, "slot_count", 255, &n)) tmp.slot_count = (uint8_t)n; else ok = 0;
    if (ok && json_uint(doc, "sample_base", 65535, &n)) tmp.sample_base = (uint16_t)n; else ok = 0;
    const cJSON *windows = cJSON_GetObjectItemCaseSensitive(doc, "windows");
    if (ok && cJSON_IsArray(windows) && cJSON_GetArraySize(windows) >= 1 &&
        cJSON_GetArraySize(windows) <= OD_AUDIO_WINDOWS) {
        tmp.window_count = (uint8_t)cJSON_GetArraySize(windows);
        for (unsigned i = 0; ok && i < tmp.window_count; ++i) {
            const cJSON *w = cJSON_GetArrayItem(windows, (int)i);
            const cJSON *slot = cJSON_GetObjectItemCaseSensitive(w, "slot");
            ok = cJSON_IsObject(w) && json_uint(w, "first", 65535, &n);
            tmp.window[i].first = (uint16_t)n;
            if (ok && cJSON_IsNumber(slot) && slot->valuedouble == -1) tmp.window[i].slot = -1;
            else if (ok && json_uint(w, "slot", 255, &n)) tmp.window[i].slot = (int16_t)n;
            else ok = 0;
        }
    } else ok = 0;
    const cJSON *tables = cJSON_GetObjectItemCaseSensitive(doc, "tables");
    const struct { const char *key; uint16_t *field; } keys[] = {
        {"fade_steps", &tmp.fade_steps}, {"song_slot", &tmp.song_slot},
        {"song_start", &tmp.song_start}, {"periods", &tmp.periods},
        {"durations", &tmp.durations}, {"effects", &tmp.effects},
        {"volume_patterns", &tmp.volume_patterns}, {"pitch_steps", &tmp.pitch_steps},
    };
    for (size_t i = 0; ok && i < sizeof keys / sizeof keys[0]; ++i) {
        ok = cJSON_IsObject(tables) && json_uint(tables, keys[i].key, 65535, &n);
        *keys[i].field = (uint16_t)n;
    }
    if (ok) ok = bytes_parse(doc, "world_music", tmp.world_music, sizeof tmp.world_music);
    cJSON_Delete(pack);
    if (!ok || !layout_valid(&tmp)) return fail(err, cap, OD_ERR_FORMAT, "invalid audio_layout in " OD_AUDIO_LAYOUT_PATH);
    *out = tmp;
    return OD_OK;
}

od_status od_audio_layout_install(const od_audio_layout *layout)
{
    if (!layout) {
        memset(&audio_installed, 0, sizeof audio_installed);
        have_audio = 0;
        return OD_OK;
    }
    if (!layout_valid(layout)) return OD_ERR_FORMAT;
    audio_installed = *layout;
    have_audio = 1;
    return OD_OK;
}

const od_audio_layout *od_audio_layout_get(void)
{
    if (have_audio) return &audio_installed;
    char base[1024];
    if (od_content_resolve_base(base, sizeof base, NULL) != OD_OK) return NULL;
    od_content_vfs vfs = {0};
    snprintf(vfs.layers[0].root, sizeof vfs.layers[0].root, "%s", base);
    vfs.count = 1;
    od_audio_layout tmp;
    if (od_audio_layout_parse(&tmp, &vfs, NULL, 0) != OD_OK) return NULL;
    if (od_audio_layout_install(&tmp) != OD_OK) return NULL;
    return &audio_installed;
}
