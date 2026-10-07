/* content_tables.c - parse the engine JSON of content/base/engine into
 * od_engine_tables.  See opendana/content_tables.h. */
#include "opendana/content_tables.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0 && err[0] == '\0')
        snprintf(err, cap, "%s", msg);
}

static const cJSON *jget(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static int json_array(const cJSON *root, const char *key, size_t want, const cJSON **out)
{
    const cJSON *a = jget(root, key);
    if (a == NULL || !cJSON_IsArray(a))
        return 0;
    if ((size_t)cJSON_GetArraySize(a) != want)
        return 0;
    *out = a;
    return 1;
}

static int get_u8_array(const cJSON *root, const char *key, uint8_t *dst, size_t want,
                        char *err, size_t cap)
{
    const cJSON *a;
    if (!json_array(root, key, want, &a)) {
        set_err(err, cap, "missing or badly sized 8-bit table");
        return 0;
    }
    for (size_t i = 0; i < want; i++) {
        const cJSON *v = cJSON_GetArrayItem(a, (int)i);
        if (v == NULL || !cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 255) {
            set_err(err, cap, "8-bit table value out of range");
            return 0;
        }
        dst[i] = (uint8_t)v->valuedouble;
    }
    return 1;
}

static int get_u16_array(const cJSON *root, const char *key, uint16_t *dst, size_t want,
                         char *err, size_t cap)
{
    const cJSON *a;
    if (!json_array(root, key, want, &a)) {
        set_err(err, cap, "missing or badly sized 16-bit table");
        return 0;
    }
    for (size_t i = 0; i < want; i++) {
        const cJSON *v = cJSON_GetArrayItem(a, (int)i);
        if (v == NULL || !cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 65535) {
            set_err(err, cap, "16-bit table value out of range");
            return 0;
        }
        dst[i] = (uint16_t)v->valuedouble;
    }
    return 1;
}

static int get_u8_scalar(const cJSON *obj, const char *key, uint8_t *out,
                         char *err, size_t cap)
{
    const cJSON *v = jget(obj, key);
    if (v == NULL || !cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 255) {
        set_err(err, cap, "missing or out-of-range 8-bit value");
        return 0;
    }
    *out = (uint8_t)v->valuedouble;
    return 1;
}

static int get_flame_animation(const cJSON *root, od_flame_anim *dst, size_t want,
                               char *err, size_t cap)
{
    const cJSON *a;
    if (!json_array(root, "flame_animation", want, &a)) {
        set_err(err, cap, "missing or badly sized flame_animation");
        return 0;
    }
    for (size_t i = 0; i < want; i++) {
        const cJSON *rec = cJSON_GetArrayItem(a, (int)i);
        const cJSON *pose;
        if (rec == NULL || !cJSON_IsObject(rec)) {
            set_err(err, cap, "flame_animation entry must be an object");
            return 0;
        }
        if (!get_u8_scalar(rec, "count", &dst[i].count, err, cap) ||
            !get_u8_scalar(rec, "wait", &dst[i].wait, err, cap))
            return 0;
        if (!json_array(rec, "pose", 4, &pose)) {
            set_err(err, cap, "flame_animation pose must have 4 words");
            return 0;
        }
        for (size_t k = 0; k < 4; k++) {
            const cJSON *v = cJSON_GetArrayItem(pose, (int)k);
            if (v == NULL || !cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 65535) {
                set_err(err, cap, "flame_animation pose word out of range");
                return 0;
            }
            dst[i].pose[k] = (uint16_t)v->valuedouble;
        }
    }
    return 1;
}

static int check_schema(const cJSON *doc, const char *where, char *err, size_t cap)
{
    const cJSON *v = jget(doc, "schema_version");
    if (v == NULL || !cJSON_IsNumber(v) || (int)v->valuedouble != OD_CONTENT_SCHEMA_VERSION) {
        char msg[96];
        snprintf(msg, sizeof msg, "%.60s: bad schema_version", where);
        set_err(err, cap, msg);
        return 0;
    }
    return 1;
}

static od_status read_json(const od_content_vfs *vfs, const char *rel, cJSON **out)
{
    char *text = NULL;
    size_t size = 0;
    if (od_content_vfs_read(vfs, rel, &text, &size) != OD_OK)
        return OD_ERR_IO;
    cJSON *doc = cJSON_ParseWithLength(text, size);
    free(text);
    if (doc == NULL)
        return OD_ERR_FORMAT;
    *out = doc;
    return OD_OK;
}

od_status od_engine_tables_parse(od_engine_tables *out, const od_content_vfs *vfs,
                                   char *err, size_t errcap)
{
    if (out == NULL || vfs == NULL)
        return OD_ERR_FORMAT;
    if (err != NULL && errcap > 0)
        err[0] = '\0';
    memset(out, 0, sizeof(*out));

    cJSON *doc = NULL;
    od_status st;

    st = read_json(vfs, "engine/enemy-tables.json", &doc);
    if (st != OD_OK) {
        set_err(err, errcap, "cannot read engine/enemy-tables.json");
        return st;
    }
    if (!check_schema(doc, "engine/enemy-tables.json", err, errcap) ||
        !get_u8_array(doc, "horizontal_animations", out->horizontal_animations,
                      OD_ENEMY_H_ANIMS, err, errcap) ||
        !get_u8_array(doc, "vertical_animations", out->vertical_animations,
                      OD_ENEMY_V_ANIMS, err, errcap) ||
        !get_u8_array(doc, "resistant_animations", out->resistant_animations,
                      OD_ENEMY_R_ANIMS, err, errcap) ||
        !get_flame_animation(doc, out->flame_animation, OD_FLAME_ANIM_COUNT, err, errcap)) {
        cJSON_Delete(doc);
        return OD_ERR_FORMAT;
    }
    cJSON_Delete(doc);

    st = read_json(vfs, "engine/hero-programs.json", &doc);
    if (st != OD_OK) {
        set_err(err, errcap, "cannot read engine/hero-programs.json");
        return st;
    }
    if (!check_schema(doc, "engine/hero-programs.json", err, errcap) ||
        !get_u8_array(doc, "programs_clear", out->hero_prog_clear, OD_HERO_PROG_BYTES, err, errcap) ||
        !get_u8_array(doc, "programs_covered", out->hero_prog_covered, OD_HERO_PROG_BYTES, err, errcap)) {
        cJSON_Delete(doc);
        return OD_ERR_FORMAT;
    }
    cJSON_Delete(doc);

    st = read_json(vfs, "engine/boss-tables.json", &doc);
    if (st != OD_OK) {
        set_err(err, errcap, "cannot read engine/boss-tables.json");
        return st;
    }
    if (!check_schema(doc, "engine/boss-tables.json", err, errcap) ||
        !get_u8_array(doc, "attack_data", out->boss_attack_data,
                      OD_BOSS_ATTACK_DATA, err, errcap) ||
        !get_u16_array(doc, "attack_offset", out->boss_attack_offset,
                       OD_BOSS_ATTACK_OFFSET, err, errcap) ||
        !get_u8_array(doc, "corridor_right", out->boss_corridor_right,
                      OD_BOSS_CORRIDOR, err, errcap) ||
        !get_u8_array(doc, "corridor_left", out->boss_corridor_left,
                      OD_BOSS_CORRIDOR, err, errcap) ||
        !get_u8_array(doc, "pose_data", out->boss_pose_data,
                      OD_BOSS_POSE_DATA, err, errcap) ||
        !get_u8_array(doc, "retire_spans", out->boss_retire_spans,
                      OD_BOSS_RETIRE_BYTES, err, errcap)) {
        cJSON_Delete(doc);
        return OD_ERR_FORMAT;
    }
    cJSON_Delete(doc);
    return OD_OK;
}

/* ------------------------------------------------------------------------- */
/* Process-wide installation                                                  */
/* ------------------------------------------------------------------------- */

static od_engine_tables g_tables;
static int g_installed;

void od_engine_tables_install(const od_engine_tables *tables)
{
    if (tables == NULL) {
        g_installed = 0;
        return;
    }
    g_tables = *tables;
    g_installed = 1;
}

const od_engine_tables *od_engine_tables_get(void)
{
    if (g_installed)
        return &g_tables;

    char base[1024];
    if (od_content_resolve_base(base, sizeof base, NULL) != OD_OK)
        return NULL;

    od_content_vfs vfs;
    memset(&vfs, 0, sizeof(vfs));
    snprintf(vfs.layers[0].root, sizeof(vfs.layers[0].root), "%s", base);
    vfs.count = 1;

    od_engine_tables tmp;
    if (od_engine_tables_parse(&tmp, &vfs, NULL, 0) != OD_OK)
        return NULL;
    od_engine_tables_install(&tmp);
    return &g_tables;
}
