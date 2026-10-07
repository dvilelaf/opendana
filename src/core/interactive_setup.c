/* Parser of the decoded setup documents (screens/interactive-setup.json),
 * deliberately separate from scene runtime. */
#include "opendana/interactive_setup.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

struct od_interactive_setup_stage { od_interactive_setup art; };
static od_interactive_setup g_art;
static int g_loaded;

static const cJSON *get(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static int shape(const cJSON *obj, const char *const *keys, unsigned n)
{
    if (!cJSON_IsObject(obj)) return 0;
    unsigned seen = 0, count = 0;
    for (const cJSON *v = obj->child; v; v = v->next) {
        unsigned i;
        for (i = 0; i < n; ++i) if (v->string && !strcmp(v->string, keys[i])) break;
        if (i == n || (seen & (1u << i))) return 0;
        seen |= 1u << i;
        ++count;
    }
    return count == n;
}

static int integer(const cJSON *v, unsigned max, unsigned *out)
{
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= max)) return 0;
    unsigned number = (unsigned)v->valuedouble;
    if (v->valuedouble != number) return 0;
    *out = number;
    return 1;
}

static int array_bytes(const cJSON *a, uint8_t *dst, int count, unsigned max)
{
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != count) return 0;
    int i = 0;
    for (const cJSON *v = a->child; v; v = v->next) {
        unsigned number;
        if (!integer(v, max, &number)) return 0;
        dst[i++] = (uint8_t)number;
    }
    return 1;
}

static int records(const cJSON *a, od_menu_record *dst, int count)
{
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != count) return 0;
    int i = 0;
    for (const cJSON *row = a->child; row; row = row->next, ++i) {
        unsigned ctl, addr;
        if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) != 3 ||
            !integer(cJSON_GetArrayItem(row, 0), 255, &ctl) ||
            !integer(cJSON_GetArrayItem(row, 1), 0x27ff, &addr)) return 0;
        unsigned n = ctl & 63u, stride = ctl & 128u ? 32u : 1u;
        if (ctl & 64u || !n || n >= 60 || addr < 0x2000 ||
            addr + (n - 1) * stride > 0x27ff ||
            !array_bytes(cJSON_GetArrayItem(row, 2), dst[i].data, (int)n, 255)) return 0;
        dst[i].count = (uint8_t)ctl;
        dst[i].addr = (uint16_t)addr;
    }
    return 1;
}

od_status od_interactive_setup_prepare_doc(const cJSON *doc,
    od_interactive_setup_stage **out)
{
    static const char *const root_keys[] = {"schema_version", "id", "password", "editor"};
    static const char *const pw_keys[] = {"chr", "palette_buffer", "field_frame",
        "choice_panel", "field_cursor_x", "palette_cursor_oam_yx"};
    static const char *const ed_keys[] = {"chr", "palette_buffer", "initial_layout",
        "toolbar_layout", "entry_cinematic"};
    if (!out) return OD_ERR_FORMAT;
    unsigned version;
    const cJSON *id = get(doc, "id"), *pw = get(doc, "password"), *ed = get(doc, "editor");
    if (!shape(doc, root_keys, 4) || !integer(get(doc, "schema_version"), 1, &version) ||
        version != 1 || !cJSON_IsString(id) || strcmp(id->valuestring, "interactive-setup") ||
        !shape(pw, pw_keys, 6) || !shape(ed, ed_keys, 5)) return OD_ERR_FORMAT;
    od_interactive_setup_stage *s = calloc(1, sizeof *s);
    if (!s) return OD_ERR_NOMEM;
    od_interactive_setup *a = &s->art;
    int ok = array_bytes(get(pw, "chr"), a->password_chr, 6, 127) &&
        array_bytes(get(pw, "palette_buffer"), a->password_palette_buffer, 32, 63) &&
        records(get(pw, "field_frame"), a->password_field_frame, 4) &&
        records(get(pw, "choice_panel"), a->password_choice_panel, 8) &&
        array_bytes(get(pw, "field_cursor_x"), a->password_field_cursor_x, 24, 255) &&
        array_bytes(get(ed, "chr"), a->editor_chr, 6, 127) &&
        array_bytes(get(ed, "palette_buffer"), a->editor_palette_buffer, 32, 63) &&
        records(get(ed, "initial_layout"), a->editor_initial_layout, 27) &&
        records(get(ed, "toolbar_layout"), a->editor_toolbar_layout, 8) &&
        integer(get(ed, "entry_cinematic"), 41, &a->editor_entry_cinematic);
    const cJSON *xy = get(pw, "palette_cursor_oam_yx");
    if (!cJSON_IsArray(xy) || cJSON_GetArraySize(xy) != 35) ok = 0;
    for (int i = 0; i < 35 && ok; ++i)
        ok = array_bytes(cJSON_GetArrayItem(xy, i), a->password_palette_cursor_oam_yx[i], 2, 255);
    if (!ok) { free(s); return OD_ERR_FORMAT; }
    *out = s;
    return OD_OK;
}

void od_interactive_setup_commit(od_interactive_setup_stage *s)
{
    if (!s) return;
    g_art = s->art;
    g_loaded = 1;
    free(s);
}

void od_interactive_setup_discard(od_interactive_setup_stage *s) { free(s); }
const od_interactive_setup *od_interactive_setup_get(void) { return g_loaded ? &g_art : NULL; }
void od_interactive_setup_reset(void) { memset(&g_art, 0, sizeof g_art); g_loaded = 0; }
