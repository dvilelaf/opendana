/* content.c - layered content package loader. See opendana/content.h. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "opendana/content.h"
#include "opendana/userdir.h"
#include "content_snapshot.h"

#include "cJSON.h"
#include "opendana/content_gfx.h"
#include "opendana/content_tables.h"
#include "opendana/rom_data.h"
#include "opendana/editor_data.h"
#include "opendana/scene_data.h"
#include "opendana/presentation_data.h"
#include "opendana/platform.h"
#include "opendana/import.h"

#include "../import/fsutil.h"
#include "../import/sha256.h"

#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#if !defined(_WIN32)
#include <dirent.h>
#endif

#define CONTENT_MAX_COMMANDS 512
#define CONTENT_MAX_LEVELS OD_LEVEL_COUNT

/* ------------------------------------------------------------------------- */
/* Small byte buffer                                                          */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} bytebuf;

static int bb_reserve(bytebuf *b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return 1;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra)
        cap *= 2;
    uint8_t *grown = (uint8_t *)realloc(b->data, cap);
    if (grown == NULL)
        return 0;
    b->data = grown;
    b->cap = cap;
    return 1;
}

static int bb_put(bytebuf *b, const void *src, size_t n)
{
    if (!bb_reserve(b, n))
        return 0;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 1;
}

static int bb_u8(bytebuf *b, uint8_t v) { return bb_put(b, &v, 1); }

static int bb_u16(bytebuf *b, uint16_t v)
{
    uint8_t t[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
    return bb_put(b, t, 2);
}

static int bb_u32(bytebuf *b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                     (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
    return bb_put(b, t, 4);
}

static int bb_u64(bytebuf *b, uint64_t v)
{
    uint8_t t[8];
    for (int i = 0; i < 8; i++)
        t[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    return bb_put(b, t, 8);
}

/* ------------------------------------------------------------------------- */
/* File helpers                                                               */
/* ------------------------------------------------------------------------- */

static int file_exists(const char *path)
{
    FILE *f = od_fs_fopen(path, "rb");
    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

/* Layer lookup is tri-state: only a genuinely absent path permits looking in
 * the next layer. An unreadable file, dangling link, directory or failed
 * metadata/read operation masks the base with an IO error, never vanilla. */
static int layer_file_probe(const char *path)
{
#if defined(_WIN32)
    /* Wide API: the path is UTF-8 (C6.29, C6.22 PUB-8). */
    WIN32_FIND_DATAW entry;
    wchar_t *wpath = od_fs_widen(path);
    if (wpath == NULL) return -1;
    HANDLE h = FindFirstFileW(wpath, &entry);
    DWORD first_error = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = first_error;
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) return -1;
        /* PATH_NOT_FOUND is not proof that a mod omitted the resource: a
         * present parent may be a file or dangling reparse point. */
        char parent[1200];
        snprintf(parent, sizeof parent, "%s", path);
        for (;;) {
            char *a = strrchr(parent, '/'), *b = strrchr(parent, '\\');
            char *slash = a == NULL ? b : b == NULL ? a : a > b ? a : b;
            if (slash == NULL || slash == parent || (slash == parent + 2 && parent[1] == ':')) break;
            *slash = '\0';
            wchar_t *wparent = od_fs_widen(parent);
            if (wparent == NULL) return -1;
            h = FindFirstFileW(wparent, &entry);
            if (h != INVALID_HANDLE_VALUE) {
                FindClose(h);
                HANDLE dir = CreateFileW(wparent, FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
                free(wparent);
                if (dir == INVALID_HANDLE_VALUE) return -1;
                BY_HANDLE_FILE_INFORMATION info;
                int directory = GetFileInformationByHandle(dir, &info) &&
                    (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
                if (!CloseHandle(dir)) return -1;
                return directory ? 0 : -1;
            }
            e = GetLastError();
            free(wparent);
            if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) return -1;
        }
        return 0;
    }
    FindClose(h);
    if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return -1;
#else
    struct stat entry;
    if (lstat(path, &entry) != 0) {
        if (errno != ENOENT) return -1;
        /* ENOENT can also come from a dangling parent symlink. Walk parents
         * until an existing entry proves the remaining suffix is absent. */
        char parent[1200];
        snprintf(parent, sizeof parent, "%s", path);
        char *slash;
        while ((slash = strrchr(parent, '/')) != NULL) {
            if (slash == parent) break;
            *slash = '\0';
            if (lstat(parent, &entry) == 0) {
                if (stat(parent, &entry) != 0 || !S_ISDIR(entry.st_mode)) return -1;
                return 0;
            }
            if (errno != ENOENT) return -1;
        }
        return 0;
    }
    if (stat(path, &entry) != 0 || !S_ISREG(entry.st_mode)) return -1;
#endif
    FILE *f = od_fs_fopen(path, "rb");
    if (f == NULL) return -1; /* entry was present: even ENOENT now is failure */
    return fclose(f) == 0 ? 1 : -1;
}

static int read_file(const char *path, char **out, size_t *size)
{
    FILE *f = od_fs_fopen(path, "rb");
    if (f == NULL)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    char *buf = (char *)malloc((size_t)len + 1);
    if (buf == NULL) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)len, f);
    int failed = ferror(f);
    if (fclose(f) != 0) failed = 1;
    if (failed || got != (size_t)len) { free(buf); return 0; }
    buf[len] = '\0';
    *out = buf;
    *size = (size_t)len;
    return 1;
}

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0 && err[0] == '\0')
        snprintf(err, cap, "%s", msg);
}

/* ------------------------------------------------------------------------- */
/* JSON helpers                                                               */
/* ------------------------------------------------------------------------- */

static const cJSON *jget(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static int jint(const cJSON *obj, const char *key, int *out)
{
    const cJSON *v = jget(obj, key);
    if (v == NULL || !cJSON_IsNumber(v))
        return 0;
    *out = (int)v->valuedouble;
    return 1;
}

static const char *jstr(const cJSON *obj, const char *key)
{
    const cJSON *v = jget(obj, key);
    return (v != NULL && cJSON_IsString(v)) ? v->valuestring : NULL;
}

/* ------------------------------------------------------------------------- */
/* Level parsing                                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
    int index;
    int number;
    int world;
    int round;
    int has_fire;
    uint8_t player;
    uint8_t metadata;
    uint8_t grid[OD_GRID_CELLS];
    uint8_t commands[CONTENT_MAX_COMMANDS];
    int command_len;
} parsed_level;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_grid(const cJSON *obj, int gw, int gh, uint8_t *grid,
                      char *err, size_t errcap)
{
    const cJSON *rows = jget(obj, "grid");
    if (rows == NULL || !cJSON_IsArray(rows) || cJSON_GetArraySize(rows) != gh) {
        set_err(err, errcap, "grid needs 15 rows");
        return 0;
    }
    for (int y = 0; y < gh; y++) {
        const cJSON *row = cJSON_GetArrayItem(rows, y);
        if (row == NULL || !cJSON_IsString(row) || (int)strlen(row->valuestring) != gw) {
            set_err(err, errcap, "grid row must be 16 hex digits");
            return 0;
        }
        for (int x = 0; x < gw; x++) {
            int v = hex_value(row->valuestring[x]);
            if (v < 0) {
                set_err(err, errcap, "grid has a non-hex digit");
                return 0;
            }
            if (v > 0x0F) {
                set_err(err, errcap, "grid cell type above 0x0F");
                return 0;
            }
            grid[y * gw + x] = (uint8_t)v;
        }
    }
    return 1;
}

static int parse_objects(const cJSON *obj, parsed_level *lv, char *err, size_t errcap)
{
    const cJSON *objects = jget(obj, "objects");
    int len = 0;
    if (objects != NULL) {
        if (!cJSON_IsArray(objects)) {
            set_err(err, errcap, "objects must be an array");
            return 0;
        }
        int count = cJSON_GetArraySize(objects);
        for (int i = 0; i < count; i++) {
            const cJSON *o = cJSON_GetArrayItem(objects, i);
            if (o == NULL || !cJSON_IsObject(o)) {
                set_err(err, errcap, "object entry must be an object");
                return 0;
            }
            int special;
            if (jint(o, "special", &special)) {
                int argument;
                if (special < 0x80 || special > 0x9F || !jint(o, "argument", &argument) ||
                    argument < 0 || argument > 0xFF) {
                    set_err(err, errcap, "bad special object");
                    return 0;
                }
                if (len + 2 >= CONTENT_MAX_COMMANDS) {
                    set_err(err, errcap, "object stream too long");
                    return 0;
                }
                lv->commands[len++] = (uint8_t)special;
                lv->commands[len++] = (uint8_t)argument;
                if (special >= 0x81 && special <= 0x83)
                    lv->has_fire = 1;
            } else {
                int type;
                const cJSON *positions = jget(o, "positions");
                if (!jint(o, "type", &type) || type < 0x01 || type > 0x7F ||
                    positions == NULL || !cJSON_IsArray(positions)) {
                    set_err(err, errcap, "bad object run");
                    return 0;
                }
                int n = cJSON_GetArraySize(positions);
                if (n < 1 || n > 0xFF || len + 2 + n >= CONTENT_MAX_COMMANDS) {
                    set_err(err, errcap, "bad object run length");
                    return 0;
                }
                lv->commands[len++] = (uint8_t)type;
                lv->commands[len++] = (uint8_t)n;
                for (int k = 0; k < n; k++) {
                    const cJSON *pos = cJSON_GetArrayItem(positions, k);
                    int x, y;
                    if (pos == NULL || !cJSON_IsArray(pos) ||
                        cJSON_GetArraySize(pos) != 2 ||
                        !cJSON_IsNumber(cJSON_GetArrayItem(pos, 0)) ||
                        !cJSON_IsNumber(cJSON_GetArrayItem(pos, 1))) {
                        set_err(err, errcap, "position must be [x, y]");
                        return 0;
                    }
                    x = (int)cJSON_GetArrayItem(pos, 0)->valuedouble;
                    y = (int)cJSON_GetArrayItem(pos, 1)->valuedouble;
                    if (x < 0 || x >= OD_GRID_W || y < 0 || y >= OD_GRID_H) {
                        set_err(err, errcap, "position out of the grid");
                        return 0;
                    }
                    int cell = y * OD_GRID_W + x;
                    if (lv->grid[cell] != (uint8_t)type) {
                        set_err(err, errcap, "object run does not match the grid");
                        return 0;
                    }
                    lv->commands[len++] = (uint8_t)cell;
                }
            }
        }
    }
    if (len + 1 > CONTENT_MAX_COMMANDS) {
        set_err(err, errcap, "object stream too long");
        return 0;
    }
    lv->commands[len++] = 0x00; /* terminator */
    lv->command_len = len;
    return 1;
}

static int parse_level_obj(const cJSON *obj, int fallback_index, int gw, int gh,
                           parsed_level *out, char *err, size_t errcap)
{
    if (obj == NULL || !cJSON_IsObject(obj)) {
        set_err(err, errcap, "level entry must be an object");
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->index = fallback_index;
    out->number = fallback_index + 1;
    out->world = fallback_index / 10 + 1;
    out->round = fallback_index % 10 + 1;
    (void)jint(obj, "index", &out->index);
    (void)jint(obj, "number", &out->number);
    (void)jint(obj, "world", &out->world);
    (void)jint(obj, "round", &out->round);

    const cJSON *player = jget(obj, "player");
    int px, py;
    if (player == NULL || !cJSON_IsObject(player) ||
        !jint(player, "x", &px) || !jint(player, "y", &py) ||
        px < 0 || px >= OD_GRID_W || py < 0 || py >= OD_GRID_H) {
        set_err(err, errcap, "level has no Player (player x/y)");
        return 0;
    }
    out->player = (uint8_t)(py * OD_GRID_W + px);

    int metadata = 0;
    (void)jint(obj, "metadata", &metadata);
    if (metadata < 0 || metadata > 0xFF) {
        set_err(err, errcap, "metadata out of range");
        return 0;
    }
    out->metadata = (uint8_t)metadata;

    if (!parse_grid(obj, gw, gh, out->grid, err, errcap))
        return 0;
    for (int i = 0; i < OD_GRID_CELLS; i++)
        if (out->grid[i] == 0x0E)
            out->has_fire = 1;
    if (!parse_objects(obj, out, err, errcap))
        return 0;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Binary image reconstruction                                                */
/* ------------------------------------------------------------------------- */

static int build_levels_image(const parsed_level *lv, int count, bytebuf *out)
{
    uint32_t h32 = OD_FNV1A32_INIT;
    uint64_t h64 = OD_FNV1A64_INIT;
    for (int i = 0; i < count; i++) {
        h32 = od_fnv1a32(lv[i].grid, sizeof(lv[i].grid), h32);
        h64 = od_fnv1a64(lv[i].grid, sizeof(lv[i].grid), h64);
    }
    if (!bb_put(out, "ODLV", 4) || !bb_u8(out, 1) || !bb_u8(out, OD_GRID_W) ||
        !bb_u8(out, OD_GRID_H) || !bb_u8(out, 0) || !bb_u16(out, (uint16_t)count) ||
        !bb_u16(out, 0) || !bb_u32(out, h32) || !bb_u64(out, h64))
        return 0;
    for (int i = 0; i < count; i++) {
        if (!bb_u8(out, lv[i].player) || !bb_u8(out, lv[i].metadata) ||
            !bb_u16(out, (uint16_t)lv[i].command_len) ||
            !bb_put(out, lv[i].grid, sizeof(lv[i].grid)) ||
            !bb_put(out, lv[i].commands, (size_t)lv[i].command_len))
            return 0;
    }
    return 1;
}

static int build_enemies_image(const parsed_level *lv, int count, bytebuf *out)
{
    uint32_t h32 = OD_FNV1A32_INIT;
    uint64_t h64 = OD_FNV1A64_INIT;
    for (int i = 0; i < count; i++) {
        h32 = od_fnv1a32(lv[i].commands, (size_t)lv[i].command_len, h32);
        h64 = od_fnv1a64(lv[i].commands, (size_t)lv[i].command_len, h64);
    }
    if (!bb_put(out, "ODEN", 4) || !bb_u8(out, 1) || !bb_u8(out, OD_GRID_W) ||
        !bb_u8(out, OD_GRID_H) || !bb_u8(out, 0) || !bb_u16(out, (uint16_t)count) ||
        !bb_u16(out, 0) || !bb_u32(out, h32) || !bb_u64(out, h64))
        return 0;
    for (int i = 0; i < count; i++) {
        if (!bb_u8(out, lv[i].player) ||
            !bb_u16(out, (uint16_t)lv[i].command_len) ||
            !bb_put(out, lv[i].commands, (size_t)lv[i].command_len))
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* VFS                                                                        */
/* ------------------------------------------------------------------------- */

od_status od_content_resolve_base(char *out, size_t cap, const char *cli_override)
{
    if (out == NULL || cap == 0)
        return OD_ERR_RANGE;
    out[0] = '\0';

    if (cli_override != NULL && cli_override[0] != '\0') {
        if (strlen(cli_override) >= cap)
            return OD_ERR_RANGE;
        snprintf(out, cap, "%s", cli_override);
        return OD_OK;
    }

    const char *env = getenv("OD_CONTENT");
    if (env != NULL && env[0] != '\0') {
        if (strlen(env) >= cap)
            return OD_ERR_RANGE;
        snprintf(out, cap, "%s", env);
        return OD_OK;
    }

    /* C6.2: every default/lazy module loader follows the product's imported
     * package selection. Do not rediscover a different package from the CWD. */
    char user[1200], probe[1200];
    int len = -1;
    {   /* C6.25: the per-user folder comes from the one helper (C6.31),
         * UTF-8 on every platform (C6.36). */
        char data[1100];
        if (od_userdir(OD_USERDIR_DATA, data, sizeof data) == OD_OK)
            len = snprintf(user, sizeof user, "%s/content/base", data);
    }
    if (len > 0 && (size_t)len < sizeof user) {
        od_platform_join(probe, sizeof probe, user, "pack.json");
        if (file_exists(probe)) {
            if (strlen(user) >= cap)
                return OD_ERR_RANGE;
            snprintf(out, cap, "%s", user);
            return OD_OK;
        }
    }

    char candidates[6][1200];
    int n = 0;
    char exe[1200];
    if (od_platform_executable_dir(exe, sizeof(exe))) {
        od_platform_join(candidates[n++], sizeof(candidates[0]), exe, "content/base");
        od_platform_join(candidates[n++], sizeof(candidates[0]), exe, "../content/base");
        od_platform_join(candidates[n++], sizeof(candidates[0]), exe, "content");
        od_platform_join(candidates[n++], sizeof(candidates[0]), exe, "../content");
    }

    for (int i = 0; i < n; i++) {
        char probe[1200];
        od_platform_join(probe, sizeof(probe), candidates[i], "pack.json");
        if (file_exists(probe)) {
            snprintf(out, cap, "%s", candidates[i]);
            return OD_OK;
        }
    }
    return OD_ERR_IO;
}

static int safe_mod_id(const char *id);

od_status od_content_resolve_mod(char *out, size_t cap, const char *base_dir,
                                   const char *arg)
{
    if (out == NULL || cap == 0 || arg == NULL || arg[0] == '\0')
        return OD_ERR_RANGE;
    char probe[1200];
    od_platform_join(probe, sizeof(probe), arg, "mod.json");
    if (file_exists(probe) || base_dir == NULL || strchr(arg, '/') != NULL) {
        if (strlen(arg) >= cap)
            return OD_ERR_RANGE;
        snprintf(out, cap, "%s", arg);
        return OD_OK;
    }
    /* An ID: mods/<id> beside the package (the repository's mods/, then the
     * package's own content/mods/), found from the base like the base itself
     * is found from the executable, never from the CWD (A11). C6.29 (PUB-10):
     * an imported base lives under the user's data directory, so the ID is
     * also looked up beside the executable (<exe>/../mods, <exe>/mods). */
    if (!safe_mod_id(arg))
        return OD_ERR_IO;
    char dirs[4][1200];
    int nd = 0;
    const char *roots[] = {"../../mods", "../mods"};
    for (unsigned i = 0; i < 2; i++) {
        char rel[300];
        snprintf(rel, sizeof(rel), "%s/%s", roots[i], arg);
        od_platform_join(dirs[nd++], sizeof(dirs[0]), base_dir, rel);
    }
    char exe[1200];
    if (od_platform_executable_dir(exe, sizeof(exe))) {
        const char *exe_roots[] = {"../mods", "mods"};
        for (unsigned i = 0; i < 2; i++) {
            char rel[300];
            snprintf(rel, sizeof(rel), "%s/%s", exe_roots[i], arg);
            od_platform_join(dirs[nd++], sizeof(dirs[0]), exe, rel);
        }
    }
    for (int i = 0; i < nd; i++) {
        od_platform_join(probe, sizeof(probe), dirs[i], "mod.json");
        if (file_exists(probe)) {
            if (strlen(dirs[i]) >= cap)
                return OD_ERR_RANGE;
            snprintf(out, cap, "%s", dirs[i]);
            return OD_OK;
        }
    }
    return OD_ERR_IO;
}

/* ------------------------------------------------------------------------- */
/* Mod declarations (C6.29: C6.22 PUB-9, PUB-10, PUB-21)                      */
/* ------------------------------------------------------------------------- */

/* The last loader/validator diagnostic, for --validate and the product's
 * "cannot load" message. Internal: declared where it is used. */
static char g_content_error[512];
static unsigned g_content_error_serial;
static void content_error(const char *fmt, ...);

const char *od_content_last_error(void) { return g_content_error; }

/* Used by content_gfx.c for CHR patch errors (internal, declared there). */
void od_content_set_error(const char *msg)
{
    snprintf(g_content_error, sizeof g_content_error, "%s", msg ? msg : "");
    g_content_error_serial++;
    fprintf(stderr, "content: %s\n", g_content_error);
}

unsigned od_content_error_serial(void) { return g_content_error_serial; }

static od_status loader_check(const char *const *roots, int count, char *stage, size_t cap);

/* C6.29 (PUB-9): a load failed without saying why. Re-run the loader's parsers
 * (never installing) on the base alone and then with the mods, and say which
 * layer and stage the game rejects, so a bad mod is never reported as a bad
 * base. Internal: called by vfs_init and assets.c on their failure paths. */
void od_content_explain_failure(const char *base_dir, const char *const *mod_dirs, int mod_count)
{
    if (base_dir == NULL || mod_count < 0 || mod_count > OD_CONTENT_MAX_LAYERS - 1)
        return;
    const char *roots[OD_CONTENT_MAX_LAYERS];
    int n = 0;
    roots[n++] = base_dir;
    for (int i = 0; i < mod_count; i++)
        if (mod_dirs != NULL && mod_dirs[i] != NULL)
            roots[n++] = mod_dirs[i];
    char stage[400];
    unsigned before = g_content_error_serial;
    if (loader_check(roots, 1, stage, sizeof stage) != OD_OK) {
        if (g_content_error_serial == before)
            content_error("the package '%.300s' is not loadable: %s", base_dir, stage);
        return;
    }
    if (n == 1)
        return;
    before = g_content_error_serial;
    if (loader_check(roots, n, stage, sizeof stage) != OD_OK && g_content_error_serial == before) {
        char who[600] = "";
        for (int i = 1; i < n; i++) {
            char path[1200], id[64] = "mod";
            char *text = NULL;
            size_t size = 0;
            od_platform_join(path, sizeof path, roots[i], "mod.json");
            if (read_file(path, &text, &size)) {
                cJSON *doc = cJSON_ParseWithLength(text, size);
                const char *s = doc ? jstr(doc, "id") : NULL;
                if (s != NULL)
                    snprintf(id, sizeof id, "%s", s);
                cJSON_Delete(doc);
                free(text);
            }
            size_t at = strlen(who);
            snprintf(who + at, sizeof who - at, "%s'%s'", i > 1 ? ", " : "", id);
        }
        content_error("the game rejects mod %s at %s (the package alone loads)", who, stage);
    }
}



static void content_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_content_error, sizeof g_content_error, fmt, ap);
    va_end(ap);
    g_content_error_serial++;
    fprintf(stderr, "content: %s\n", g_content_error);
}

/* A path a layer may declare: relative to the layer root and inside it. No
 * absolute path, drive, backslash, empty, "." or ".." segment (PUB-21). */
static int safe_rel_path(const char *path)
{
    if (path == NULL || path[0] == '\0' || path[0] == '/')
        return 0;
    const char *part = path;
    for (const char *p = path;; ++p) {
        if (*p == ':' || *p == '\\')
            return 0;
        if (*p == '/' || *p == '\0') {
            size_t n = (size_t)(p - part);
            if (n == 0 || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.'))
                return 0;
            if (*p == '\0')
                return 1;
            part = p + 1;
        }
    }
}

int od_content_safe_rel_path(const char *path) { return safe_rel_path(path); }

static int safe_mod_id(const char *id)
{
    if (id == NULL || id[0] == '\0' || id[0] == '.')
        return 0;
    for (const char *p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
            return 0;
    return strstr(id, "..") == NULL;
}

/* Keys whose value is a list of files the layer ships. */
static const char *const decl_list_keys[] = {
    "levels", "worlds", "palettes", "audio", "screens", "cinematics", "engine"
};
static const char *const decl_graphics_keys[] = { "chr", "sprites" };

#define OD_CHR_TILE_COUNT 8192

/* Read and parse `<root>/mod.json` of a mod layer (NULL on any error). The
 * declaration is metadata, not a resource: it is never part of the pinned
 * snapshot (content_snapshot.c treats mod.json the same way). */
cJSON *od_content_mod_decl(const od_content_layer *layer)
{
    if (layer == NULL || !layer->is_mod)
        return NULL;
    char path[1200];
    od_platform_join(path, sizeof(path), layer->root, "mod.json");
    char *text = NULL;
    size_t size = 0;
    if (!read_file(path, &text, &size))
        return NULL;
    cJSON *doc = cJSON_ParseWithLength(text, size);
    free(text);
    return doc;
}

/* One declared path: inside the mod and present on disk. */
static od_status check_declared_file(const char *root, const char *id, const char *key,
                                      const cJSON *v)
{
    if (v == NULL || !cJSON_IsString(v)) {
        content_error("mod '%s': mod.json \"%s\" must list file paths (strings)", id, key);
        return OD_ERR_FORMAT;
    }
    if (!safe_rel_path(v->valuestring)) {
        content_error("mod '%s': mod.json \"%s\" names '%.200s', which is not a relative "
                      "path inside the mod (no '..', no absolute path)", id, key, v->valuestring);
        return OD_ERR_FORMAT;
    }
    char path[1200];
    od_platform_join(path, sizeof(path), root, v->valuestring);
    int present = layer_file_probe(path);
    if (present != 1) {
        content_error("mod '%s': mod.json \"%s\" declares '%.200s' but the mod does not "
                      "ship it", id, key, v->valuestring);
        return OD_ERR_IO;
    }
    return OD_OK;
}

static od_status check_declared_list(const char *root, const char *id, const char *key,
                                      const cJSON *list)
{
    if (list == NULL)
        return OD_OK;
    if (!cJSON_IsArray(list)) {
        content_error("mod '%s': mod.json \"%s\" must be a list", id, key);
        return OD_ERR_FORMAT;
    }
    const cJSON *v;
    cJSON_ArrayForEach(v, list) {
        od_status st = check_declared_file(root, id, key, v);
        if (st != OD_OK)
            return st;
    }
    return OD_OK;
}

/* Every file a mod declares exists inside it; chr_patches are well formed
 * (the PNG itself is checked when the graphics load). */
static od_status check_mod_declarations(const char *root, const char *id, const cJSON *doc)
{
    for (size_t k = 0; k < sizeof decl_list_keys / sizeof decl_list_keys[0]; k++) {
        od_status st = check_declared_list(root, id, decl_list_keys[k], jget(doc, decl_list_keys[k]));
        if (st != OD_OK)
            return st;
    }
    const cJSON *graphics = jget(doc, "graphics");
    if (graphics == NULL)
        return OD_OK;
    if (!cJSON_IsObject(graphics)) {
        content_error("mod '%s': mod.json \"graphics\" must be an object", id);
        return OD_ERR_FORMAT;
    }
    for (size_t k = 0; k < sizeof decl_graphics_keys / sizeof decl_graphics_keys[0]; k++) {
        char key[32];
        snprintf(key, sizeof key, "graphics.%s", decl_graphics_keys[k]);
        od_status st = check_declared_list(root, id, key,
                                            jget(graphics, decl_graphics_keys[k]));
        if (st != OD_OK)
            return st;
    }
    const cJSON *patches = jget(graphics, "chr_patches");
    if (patches == NULL)
        return OD_OK;
    if (!cJSON_IsArray(patches)) {
        content_error("mod '%s': mod.json \"graphics.chr_patches\" must be a list", id);
        return OD_ERR_FORMAT;
    }
    const cJSON *patch;
    cJSON_ArrayForEach(patch, patches) {
        const cJSON *tile = jget(patch, "tile");
        if (!cJSON_IsObject(patch) || tile == NULL || !cJSON_IsNumber(tile) ||
            tile->valuedouble != (double)(int)tile->valuedouble ||
            tile->valueint < 0 || tile->valueint >= OD_CHR_TILE_COUNT) {
            content_error("mod '%s': each graphics.chr_patches entry needs \"png\" and an "
                          "integer \"tile\" in 0..%d", id, OD_CHR_TILE_COUNT - 1);
            return OD_ERR_FORMAT;
        }
        od_status st = check_declared_file(root, id, "graphics.chr_patches", jget(patch, "png"));
        if (st != OD_OK)
            return st;
    }
    return OD_OK;
}

/* A resource list a mod declares must name files the package reads through
 * that same list (the effective pack.json, i.e. the overlay's). Otherwise the
 * mod "changes" something the game never loads (C6.22 PUB-10). Levels are the
 * exception: a level file that is not a package world file is a level patch. */
static int list_contains(const cJSON *list, const char *rel)
{
    const cJSON *v;
    if (list == NULL || !cJSON_IsArray(list))
        return 0;
    cJSON_ArrayForEach(v, list)
        if (cJSON_IsString(v) && strcmp(v->valuestring, rel) == 0)
            return 1;
    return 0;
}

static od_status check_mod_against_pack(const od_content_layer *layer, const cJSON *doc,
                                         const cJSON *pack)
{
    for (size_t k = 1; k < sizeof decl_list_keys / sizeof decl_list_keys[0]; k++) {
        const cJSON *list = jget(doc, decl_list_keys[k]), *v;
        if (list == NULL || !cJSON_IsArray(list))
            continue;
        cJSON_ArrayForEach(v, list) {
            if (!list_contains(jget(pack, decl_list_keys[k]), v->valuestring)) {
                content_error("mod '%s': declares %s file '%.200s' but the package never "
                              "reads it (not in pack.json \"%s\")", layer->id,
                              decl_list_keys[k], v->valuestring, decl_list_keys[k]);
                return OD_ERR_FORMAT;
            }
        }
    }
    const cJSON *graphics = jget(doc, "graphics");
    const cJSON *pack_graphics = jget(pack, "graphics");
    for (size_t k = 0; graphics != NULL && k < sizeof decl_graphics_keys / sizeof decl_graphics_keys[0]; k++) {
        const cJSON *list = jget(graphics, decl_graphics_keys[k]), *v;
        if (list == NULL || !cJSON_IsArray(list))
            continue;
        cJSON_ArrayForEach(v, list) {
            if (!list_contains(jget(pack_graphics, decl_graphics_keys[k]), v->valuestring)) {
                content_error("mod '%s': declares graphics.%s file '%.200s' but the package "
                              "never reads it (not in pack.json graphics.%s)", layer->id,
                              decl_graphics_keys[k], v->valuestring, decl_graphics_keys[k]);
                return OD_ERR_FORMAT;
            }
        }
    }
    return OD_OK;
}

static od_status check_mods_against_pack(const od_content_vfs *vfs)
{
    if (vfs->count < 2)
        return OD_OK;
    char *text = NULL;
    size_t size = 0;
    od_status st = od_content_vfs_read(vfs, "pack.json", &text, &size);
    if (st != OD_OK)
        return st;
    cJSON *pack = cJSON_ParseWithLength(text, size);
    free(text);
    if (pack == NULL)
        return OD_ERR_FORMAT;
    for (int i = 1; i < vfs->count && st == OD_OK; i++) {
        cJSON *doc = od_content_mod_decl(&vfs->layers[i]);
        if (doc == NULL) {
            st = OD_ERR_FORMAT;
            break;
        }
        st = check_mod_against_pack(&vfs->layers[i], doc, pack);
        cJSON_Delete(doc);
    }
    cJSON_Delete(pack);
    return st;
}

static od_status layer_init(od_content_layer *layer, const char *root, int is_mod)
{
    char probe[1200];
    const char *decl = is_mod ? "mod.json" : "pack.json";
    od_platform_join(probe, sizeof(probe), root, decl);
    char *text = NULL;
    size_t size = 0;
    if (!read_file(probe, &text, &size)) {
        if (is_mod)
            content_error("mod at '%.300s': cannot read mod.json", root);
        return OD_ERR_IO;
    }
    cJSON *doc = cJSON_ParseWithLength(text, size);
    free(text);
    if (doc == NULL) {
        if (is_mod)
            content_error("mod at '%.300s': mod.json is not valid JSON", root);
        return OD_ERR_FORMAT;
    }
    memset(layer, 0, sizeof(*layer));
    snprintf(layer->root, sizeof(layer->root), "%s", root);
    layer->is_mod = is_mod;
    const char *id = jstr(doc, "id");
    snprintf(layer->id, sizeof(layer->id), "%s", id != NULL ? id : (is_mod ? "mod" : "base"));
    (void)jint(doc, "schema_version", &layer->schema_version);
    /* C6.53: a package is exactly version 2 (a version-1 import is stale). A mod may still
     * declare 1: overlays that name no renamed file or key load as before. */
    if (layer->schema_version != OD_PACK_SCHEMA_VERSION &&
        !(is_mod && layer->schema_version == OD_CONTENT_SCHEMA_VERSION)) {
        if (is_mod)
            content_error("mod '%s' (%.200s): mod.json schema_version %d is not supported "
                          "(this game reads version %d)", layer->id, root,
                          layer->schema_version, OD_PACK_SCHEMA_VERSION);
        cJSON_Delete(doc);
        return OD_ERR_FORMAT;
    }
    /* C6.29: what a mod declares must exist inside it (PUB-10, PUB-21). */
    od_status st = is_mod ? check_mod_declarations(root, layer->id, doc) : OD_OK;
    cJSON_Delete(doc);
    return st;
}

/* Internal parser gate; implemented by content_gfx.c, never installs. */
od_status od_content_cinematics_validate(const od_content_vfs *vfs);

/* Track owned handles by output address: vfs_init historically accepts an
 * uninitialized output and also atomically replaces a live one. Never inspect
 * an uninitialized caller pointer to release old ownership. Borrowed copies
 * are not separate owners; graphics retains its own snapshot reference. */
typedef struct vfs_owner {
    od_content_vfs *address;
    struct od_content_snapshot *snapshot;
    struct vfs_owner *next;
} vfs_owner;
static vfs_owner *vfs_owners;

static od_status content_vfs_prepare(od_content_vfs *vfs, const char *base_dir,
                                const char *const *mod_dirs, int mod_count)
{
    if (vfs == NULL || base_dir == NULL)
        return OD_ERR_FORMAT;
    memset(vfs, 0, sizeof(*vfs));
    if (mod_count < 0 || mod_count > OD_CONTENT_MAX_LAYERS - 1)
        return OD_ERR_RANGE;
    if (mod_count > 0 && mod_dirs == NULL) return OD_ERR_FORMAT;

    od_status st = layer_init(&vfs->layers[0], base_dir, 0);
    if (st != OD_OK)
        return st;
    vfs->count = 1;
    for (int i = 0; i < mod_count; i++) {
        if (mod_dirs[i] == NULL)
            continue;
        st = layer_init(&vfs->layers[vfs->count], mod_dirs[i], 1);
        if (st != OD_OK)
            return st;
        vfs->count++;
    }
    vfs->snapshot = od_content_snapshot_create(vfs);
    if (!vfs->snapshot) return OD_ERR_NOMEM;
    /* C6.29 (PUB-10): a mod's declared resources are ones the package reads. */
    st = check_mods_against_pack(vfs);
    if (st != OD_OK)
        return st;
    /* C5.6: the engine tables (flame animation, Hero programs, boss combat)
     * live in the package now. Load them in package-mod order so a mod can
     * override any of the three documents; a package without them (synthetic
     * test packs) leaves whatever was installed untouched. */
    {
        od_engine_tables tables;
        od_status table_st = od_engine_tables_parse(&tables, vfs, NULL, 0);
        /* Format-only synthetic packs have no engine declaration. A full
          * package MUST supply all five data families; parsing all before
         * installing prevents a failed layer from replacing half a session. */
        char *text = NULL;
        size_t size = 0;
        od_status doc_st = od_content_vfs_read(vfs, "pack.json", &text, &size);
        if (doc_st != OD_OK)
            return doc_st;
        cJSON *doc = cJSON_ParseWithLength(text, size);
        free(text);
        if (doc == NULL)
            return OD_ERR_FORMAT;
        int full = jget(doc, "engine") != NULL;
        cJSON_Delete(doc);
        if (full) {
            od_rom_data data;
            if (table_st != OD_OK)
                return table_st;
            od_status data_st = od_rom_data_parse(&data, vfs, NULL, 0);
            if (data_st != OD_OK)
                return data_st;
            /* The sound layout is optional for synthetic full packs. */
            od_audio_layout audio_layout;
            od_status layout_st = od_audio_layout_parse(&audio_layout, vfs, NULL, 0);
            if (layout_st != OD_OK && layout_st != OD_ERR_IO)
                return layout_st;
            od_editor_data editor;
            data_st = od_editor_data_parse(&editor, vfs, NULL, 0);
            if (data_st != OD_OK)
                return data_st;
            od_scene_data scene;
            data_st = od_scene_data_parse(&scene, vfs, NULL, 0);
            if (data_st != OD_OK)
                return data_st;
            od_presentation_data presentation;
            data_st = od_presentation_data_parse(&presentation, vfs, NULL, 0);
            if (data_st != OD_OK)
                return data_st;
            data_st = od_content_cinematics_validate(vfs);
            if (data_st != OD_OK)
                return data_st;
            /* Propagate graphics failure before table-group publication. The
             * external graphics caches still require their owner staging API. */
            data_st = od_gfx_apply(vfs);
            if (data_st != OD_OK)
                return data_st;
            /* All installation inputs have now passed their validators. */
            data_st = od_presentation_data_install(&presentation);
            if (data_st != OD_OK)
                return data_st;
            data_st = od_scene_data_install(&scene);
            if (data_st != OD_OK)
                return data_st;
            data_st = od_editor_data_install(&editor);
            if (data_st != OD_OK)
                return data_st;
            data_st = od_rom_data_install(&data);
            if (data_st != OD_OK)
                return data_st;
            data_st = od_audio_layout_install(layout_st == OD_OK ? &audio_layout : NULL);
            if (data_st != OD_OK)
                return data_st;
            od_engine_tables_install(&tables);
            od_content_snapshot_completed(vfs, OD_SNAPSHOT_TABLES);
        } else if (table_st == OD_OK) {
            st = od_gfx_apply(vfs);
            if (st != OD_OK) return st;
            od_engine_tables_install(&tables);
        } else {
            st = od_gfx_apply(vfs);
            if (st != OD_OK) return st;
        }
    }
    /* C2.2: if an asset bundle was loaded before this VFS, re-apply the
     * graphics/audio layers to it so a mod also overrides the CHR and palettes.
     * No-op when nothing is bound (e.g. a test that only uses the VFS). */
    return OD_OK;
}

od_status od_content_vfs_init(od_content_vfs *vfs, const char *base_dir,
    const char *const *mod_dirs, int mod_count)
{
    if (vfs == NULL) return OD_ERR_FORMAT;
    vfs_owner *owner = vfs_owners;
    while (owner && owner->address != vfs) owner = owner->next;
    int new_owner = owner == NULL;
    if (new_owner) {
        owner = malloc(sizeof *owner);
        if (!owner) return OD_ERR_NOMEM;
        owner->address = vfs; owner->snapshot = NULL; owner->next = NULL;
    }
    od_content_vfs candidate = {0};
    unsigned serial = g_content_error_serial;
    od_status st = content_vfs_prepare(&candidate, base_dir, mod_dirs, mod_count);
    if (st != OD_OK && st != OD_ERR_NOMEM && g_content_error_serial == serial)
        od_content_explain_failure(base_dir, mod_dirs, mod_count);
    if (st == OD_OK) {
        od_content_snapshot_release(owner->snapshot);
        owner->snapshot = candidate.snapshot;
        *vfs = candidate;
        if (new_owner) { owner->next = vfs_owners; vfs_owners = owner; }
    } else {
        od_content_snapshot_release(candidate.snapshot);
        if (new_owner) free(owner);
    }
    return st;
}

od_status od_content_vfs_capture(const od_content_vfs *src, od_content_vfs *out)
{
    if (!src || !out || src->count < 1 || src->count > OD_CONTENT_MAX_LAYERS) return OD_ERR_FORMAT;
    if (od_content_snapshot_matches(src)) {
        *out = *src;
        od_content_snapshot_retain(out->snapshot);
        return OD_OK;
    }
    memset(out, 0, sizeof *out);
    for (int i = 0; i < src->count; ++i) {
        od_status st = layer_init(&out->layers[i], src->layers[i].root, i != 0);
        if (st != OD_OK) return st;
        ++out->count;
    }
    out->snapshot = od_content_snapshot_create(out);
    return out->snapshot ? OD_OK : OD_ERR_NOMEM;
}

void od_content_vfs_free(od_content_vfs *vfs)
{
    if (!vfs) return;
    vfs_owner **link = &vfs_owners;
    while (*link && (*link)->address != vfs) link = &(*link)->next;
    if (*link) {
        vfs_owner *owner = *link;
        *link = owner->next;
        od_content_snapshot_release(owner->snapshot);
        free(owner);
    }
    memset(vfs, 0, sizeof(*vfs));
}

od_status od_content_vfs_resolve(const od_content_vfs *vfs, const char *rel,
                                   char *out, size_t cap)
{
    if (vfs == NULL || rel == NULL || out == NULL || cap == 0)
        return OD_ERR_FORMAT;
    /* C6.29 (PUB-21): a relative path never leaves its layer, whatever a mod's
     * pack.json or mod.json says. */
    if (!safe_rel_path(rel))
        return OD_ERR_FORMAT;
    for (int i = vfs->count - 1; i >= 0; i--) {
        char path[1200];
        od_platform_join(path, sizeof(path), vfs->layers[i].root, rel);
        int present = layer_file_probe(path);
        if (present < 0) return OD_ERR_IO;
        if (present > 0) {
            snprintf(out, cap, "%s", path);
            return OD_OK;
        }
    }
    return OD_ERR_IO;
}

od_status od_content_vfs_read_uncached(const od_content_vfs *vfs, const char *rel,
                                char **data, size_t *size)
{
    char path[1200];
    od_status st = od_content_vfs_resolve(vfs, rel, path, sizeof(path));
    if (st != OD_OK)
        return st;
    if (!read_file(path, data, size))
        return OD_ERR_IO;
    return OD_OK;
}

od_status od_content_vfs_read(const od_content_vfs *vfs, const char *rel,
    char **data, size_t *size)
{
    if (!vfs || !data || !size) return OD_ERR_FORMAT;
    *data = NULL; *size = 0;
    if (vfs->snapshot) return od_content_snapshot_read(vfs, rel, data, size);
    /* Legacy manually constructed format VFS has no snapshot and cannot
     * produce resource/state identity. Initialized product VFS is pinned. */
    return od_content_vfs_read_uncached(vfs, rel, data, size);
}

/* ------------------------------------------------------------------------- */
/* Pack load                                                                  */
/* ------------------------------------------------------------------------- */

/* C6.29: a mod may replace single levels without shipping a whole world.
 *
 * A file listed in a mod's mod.json "levels" that is NOT one of the package's
 * world files (pack.json "levels") is a level patch. It has the world-file
 * shape (schema/level.schema.json) and every entry carries an explicit
 * "number" (1..150); that entry replaces the package level with the same
 * number, and nothing else changes. A file whose path IS a package world file
 * keeps the old meaning (it replaces that whole world by path). Strict, so a
 * patch never replaces more than it says:
 *   - an entry without a valid "number", or with "index"/"world"/"round" that
 *     disagree with it (or with the file's "world"), is an error;
 *   - the same number in two patch entries (same or different mod) is an error.
 * Allowed from schema_version 1 (additive: existing mods load identically). */
static od_status apply_level_patches(const od_content_vfs *vfs, const cJSON *pack_files,
                                      parsed_level *parsed, int total)
{
    if (vfs->count < 2)
        return OD_OK;
    unsigned char seen[CONTENT_MAX_LEVELS];
    memset(seen, 0, sizeof seen);
    od_status st = OD_OK;
    for (int li = 1; li < vfs->count && st == OD_OK; li++) {
        const od_content_layer *layer = &vfs->layers[li];
        cJSON *decl = od_content_mod_decl(layer);
        if (decl == NULL) {
            content_error("mod '%s': cannot read mod.json", layer->id);
            return OD_ERR_FORMAT;
        }
        const cJSON *list = jget(decl, "levels"), *relv;
        cJSON_ArrayForEach(relv, list) {
            if (st != OD_OK)
                break;
            if (!cJSON_IsString(relv) || list_contains(pack_files, relv->valuestring))
                continue;   /* whole-world override by path, or checked at init */
            const char *rel = relv->valuestring;
            char *text = NULL;
            size_t size = 0;
            st = od_content_vfs_read(vfs, rel, &text, &size);
            if (st != OD_OK) {
                content_error("mod '%s': cannot read level patch '%.200s'", layer->id, rel);
                break;
            }
            cJSON *doc = cJSON_ParseWithLength(text, size);
            free(text);
            const cJSON *levels = doc ? jget(doc, "levels") : NULL;
            int schema = 0;
            if (doc == NULL || !jint(doc, "schema_version", &schema) ||
                schema != OD_CONTENT_SCHEMA_VERSION || levels == NULL ||
                !cJSON_IsArray(levels) || cJSON_GetArraySize(levels) < 1) {
                content_error("mod '%s': level patch '%.200s' must be a world-shaped file "
                              "(schema_version 1, a non-empty \"levels\" list)", layer->id, rel);
                cJSON_Delete(doc);
                st = OD_ERR_FORMAT;
                break;
            }
            int file_world = 0, has_file_world = jint(doc, "world", &file_world);
            int k = 0;
            const cJSON *entry;
            cJSON_ArrayForEach(entry, levels) {
                int number = 0, v = 0;
                char err[128] = {0};
                if (!cJSON_IsObject(entry) || !jint(entry, "number", &number) ||
                    number < 1 || number > total) {
                    content_error("mod '%s': level patch '%.200s' entry %d has no valid "
                                  "\"number\" (1..%d)", layer->id, rel, k, total);
                    st = OD_ERR_FORMAT;
                    break;
                }
                int idx = number - 1;
                int world = idx / 10 + 1, round = idx % 10 + 1;
                if ((jint(entry, "index", &v) && v != idx) ||
                    (jint(entry, "world", &v) && v != world) ||
                    (jint(entry, "round", &v) && v != round) ||
                    (has_file_world && file_world != world)) {
                    content_error("mod '%s': level patch '%.200s' entry %d: number %d does "
                                  "not agree with its index/world/round", layer->id, rel, k, number);
                    st = OD_ERR_FORMAT;
                    break;
                }
                if (seen[idx]) {
                    content_error("mod '%s': level %d-%d is patched twice (level patch "
                                  "'%.200s')", layer->id, world, round, rel);
                    st = OD_ERR_FORMAT;
                    break;
                }
                parsed_level tmp;
                if (!parse_level_obj(entry, idx, OD_GRID_W, OD_GRID_H, &tmp, err, sizeof err)) {
                    content_error("mod '%s': level patch '%.200s' level %d-%d: %s", layer->id,
                                  rel, world, round, err[0] ? err : "invalid level");
                    st = OD_ERR_FORMAT;
                    break;
                }
                tmp.index = idx;
                parsed[idx] = tmp;
                seen[idx] = 1;
                k++;
            }
            cJSON_Delete(doc);
        }
        cJSON_Delete(decl);
    }
    return st;
}

od_status od_content_pack_load(od_content_pack *out, const od_content_vfs *vfs)
{
    if (out == NULL || vfs == NULL || vfs->count < 1)
        return OD_ERR_FORMAT;
    memset(out, 0, sizeof(*out));

    char *text = NULL;
    size_t size = 0;
    od_status st = od_content_vfs_read(vfs, "pack.json", &text, &size);
    if (st != OD_OK)
        return st;
    cJSON *pack = cJSON_ParseWithLength(text, size);
    free(text);
    if (pack == NULL)
        return OD_ERR_FORMAT;

    int schema = 0;
    if (!jint(pack, "schema_version", &schema) || schema != OD_PACK_SCHEMA_VERSION) {
        cJSON_Delete(pack);
        return OD_ERR_FORMAT;
    }
    const cJSON *id = jget(pack, "id");
    const cJSON *name = jget(pack, "name");
    const cJSON *version = jget(pack, "version");
    snprintf(out->info.id, sizeof(out->info.id), "%s",
             (id && cJSON_IsString(id)) ? id->valuestring : "base");
    snprintf(out->info.name, sizeof(out->info.name), "%s",
             (name && cJSON_IsString(name)) ? name->valuestring : "");
    snprintf(out->info.version, sizeof(out->info.version), "%s",
             (version && cJSON_IsString(version)) ? version->valuestring : "");
    out->info.schema_version = schema;

    const cJSON *files = jget(pack, "levels");
    if (files == NULL || !cJSON_IsArray(files)) {
        cJSON_Delete(pack);
        return OD_ERR_FORMAT;
    }

    parsed_level *parsed = (parsed_level *)calloc(CONTENT_MAX_LEVELS, sizeof(parsed_level));
    if (parsed == NULL) {
        cJSON_Delete(pack);
        return OD_ERR_NOMEM;
    }

    int total = 0;
    int file_count = cJSON_GetArraySize(files);
    st = OD_OK;
    for (int i = 0; i < file_count && st == OD_OK; i++) {
        const cJSON *rel = cJSON_GetArrayItem(files, i);
        if (rel == NULL || !cJSON_IsString(rel)) {
            st = OD_ERR_FORMAT;
            break;
        }
        char *doc_text = NULL;
        size_t doc_size = 0;
        st = od_content_vfs_read(vfs, rel->valuestring, &doc_text, &doc_size);
        if (st != OD_OK)
            break;
        cJSON *doc = cJSON_ParseWithLength(doc_text, doc_size);
        free(doc_text);
        if (doc == NULL) {
            st = OD_ERR_FORMAT;
            break;
        }
        const cJSON *levels = jget(doc, "levels");
        if (levels == NULL || !cJSON_IsArray(levels)) {
            cJSON_Delete(doc);
            st = OD_ERR_FORMAT;
            break;
        }
        int n = cJSON_GetArraySize(levels);
        /* C6.29: a mod's file at a package world path replaces that whole
         * world, so it must hold all of it; never a silent partial world. */
        for (int li = vfs->count - 1; li >= 1; li--) {
            char probe[1200];
            od_platform_join(probe, sizeof probe, vfs->layers[li].root, rel->valuestring);
            if (layer_file_probe(probe) != 1)
                continue;
            if (n != 10) {
                content_error("mod '%s': '%.200s' replaces a whole package world by path, so it "
                              "must hold that world's 10 levels (it holds %d); single levels go "
                              "in a level patch file", vfs->layers[li].id, rel->valuestring, n);
                st = OD_ERR_FORMAT;
            }
            break;
        }
        for (int k = 0; k < n && st == OD_OK; k++) {
            if (total >= CONTENT_MAX_LEVELS) {
                st = OD_ERR_RANGE;
                break;
            }
            char err[128] = { 0 };
            parsed_level tmp;
            if (!parse_level_obj(cJSON_GetArrayItem(levels, k), total, OD_GRID_W,
                                 OD_GRID_H, &tmp, err, sizeof(err))) {
                st = OD_ERR_FORMAT;
                break;
            }
            if (tmp.index != total) {
                content_error("levels file '%.200s' holds level index %d where %d was "
                              "expected: a file that replaces a package world file must hold "
                              "that whole world (single levels go in a level patch file)",
                              rel->valuestring, tmp.index, total);
                st = OD_ERR_FORMAT;
                break;
            }
            parsed[total++] = tmp;
        }
        cJSON_Delete(doc);
    }
    /* C6.29: single-level patches of the mod layers, after the world files. */
    if (st == OD_OK)
        st = apply_level_patches(vfs, files, parsed, total);
    cJSON_Delete(pack);

    if (st == OD_OK && total == 0)
        st = OD_ERR_FORMAT;

    if (st == OD_OK) {
        bytebuf levels_image = { 0 };
        bytebuf enemies_image = { 0 };
        if (!build_levels_image(parsed, total, &levels_image) ||
            !build_enemies_image(parsed, total, &enemies_image)) {
            st = OD_ERR_NOMEM;
        } else {
            st = od_levelset_parse(&out->levels, levels_image.data, levels_image.len);
            if (st == OD_OK)
                st = od_enemyset_parse(&out->enemies, enemies_image.data, enemies_image.len);
        }
        free(levels_image.data);
        free(enemies_image.data);
    }
    free(parsed);
    if (st == OD_OK) od_content_snapshot_completed(vfs, OD_SNAPSHOT_LEVELS);

    if (st != OD_OK) {
        od_content_pack_free(out);
        return st;
    }
    out->info.level_count = out->levels.count;
    out->info.world_count = (out->levels.count + 9) / 10;
    return OD_OK;
}

void od_content_pack_free(od_content_pack *out)
{
    if (out == NULL)
        return;
    od_levelset_free(&out->levels);
    od_enemyset_free(&out->enemies);
    memset(&out->info, 0, sizeof(out->info));
}

void od_content_pack_describe(const od_content_pack *pack)
{
    if (pack == NULL)
        return;
    for (int i = 0; i < pack->levels.count; i++) {
        const od_level *lv = &pack->levels.levels[i];
        printf("%3d  start=(%2d,%2d)  meta=0x%02X  commands=%u\n", i,
               od_player_x(lv), od_player_y(lv), lv->metadata, lv->command_len);
    }
}

/* ------------------------------------------------------------------------- */
/* Single level                                                               */
/* ------------------------------------------------------------------------- */

static od_status level_from_parsed(od_content_level *out, const parsed_level *parsed,
                                    const char *source)
{
    memset(out, 0, sizeof(*out));
    out->buffer = (uint8_t *)malloc((size_t)parsed->command_len);
    if (out->buffer == NULL)
        return OD_ERR_NOMEM;
    memcpy(out->buffer, parsed->commands, (size_t)parsed->command_len);
    out->buffer_size = (size_t)parsed->command_len;
    out->level.player_position = parsed->player;
    out->level.metadata = parsed->metadata;
    out->level.command_len = (uint16_t)parsed->command_len;
    out->level.commands = out->buffer;
    memcpy(out->level.grid, parsed->grid, sizeof(out->level.grid));
    out->number = parsed->number;
    out->world = parsed->world;
    out->round = parsed->round;
    snprintf(out->source, sizeof(out->source), "%s", source != NULL ? source : "");
    return OD_OK;
}

static int select_round(const cJSON *levels, int round)
{
    int n = cJSON_GetArraySize(levels);
    if (round >= 1) {
        for (int i = 0; i < n; i++) {
            int r = 0;
            if (jint(cJSON_GetArrayItem(levels, i), "round", &r) && r == round)
                return i;
        }
        if (round <= n)
            return round - 1;
        return -1;
    }
    return n > 0 ? 0 : -1;
}

od_status od_content_level_load(od_content_level *out, const char *path, int round)
{
    if (out == NULL || path == NULL)
        return OD_ERR_FORMAT;
    memset(out, 0, sizeof(*out));

    char *text = NULL;
    size_t size = 0;
    if (!read_file(path, &text, &size))
        return OD_ERR_IO;
    cJSON *root = cJSON_ParseWithLength(text, size);
    free(text);
    if (root == NULL)
        return OD_ERR_FORMAT;

    const cJSON *levels = jget(root, "levels");
    const cJSON *chosen = NULL;
    if (levels != NULL && cJSON_IsArray(levels)) {
        int idx = select_round(levels, round);
        if (idx < 0) {
            cJSON_Delete(root);
            return OD_ERR_RANGE;
        }
        chosen = cJSON_GetArrayItem(levels, idx);
    } else {
        chosen = root;
    }

    parsed_level parsed;
    char err[128] = { 0 };
    int fallback = 0;
    if (round >= 1 && levels == NULL)
        fallback = round - 1;
    if (!parse_level_obj(chosen, fallback, OD_GRID_W, OD_GRID_H, &parsed, err, sizeof(err))) {
        cJSON_Delete(root);
        return OD_ERR_FORMAT;
    }
    cJSON_Delete(root);
    return level_from_parsed(out, &parsed, path);
}

void od_content_level_free(od_content_level *out)
{
    if (out == NULL)
        return;
    free(out->buffer);
    out->buffer = NULL;
    out->buffer_size = 0;
}

/* ------------------------------------------------------------------------- */
/* Validation                                                                 */
/* ------------------------------------------------------------------------- */

static void report_error(od_validate_report *r, const char *where, const char *why)
{
    r->errors++;
    if (r->message[0] == '\0')
        snprintf(r->message, sizeof(r->message), "%.200s: %.280s", where, why);
}

static void validate_level_obj(const cJSON *obj, int index, const char *where,
                               od_validate_report *r)
{
    parsed_level parsed;
    char err[128] = { 0 };
    if (!parse_level_obj(obj, index, OD_GRID_W, OD_GRID_H, &parsed, err, sizeof(err))) {
        report_error(r, where, err[0] ? err : "invalid level");
        return;
    }
    if (!parsed.has_fire) {
        report_error(r, where, "level has no fire");
        return;
    }
    r->levels++;
    /* Count object entries, not raw runs: a special is one object. */
    const cJSON *objects = jget(obj, "objects");
    if (objects != NULL && cJSON_IsArray(objects))
        r->objects += cJSON_GetArraySize(objects);
}

static void validate_world_doc(const cJSON *doc, const char *where, od_validate_report *r)
{
    int schema = 0;
    if (!jint(doc, "schema_version", &schema) || schema != OD_CONTENT_SCHEMA_VERSION) {
        report_error(r, where, "bad schema_version");
        return;
    }
    int gw = OD_GRID_W, gh = OD_GRID_H;
    (void)jint(doc, "grid_width", &gw);
    (void)jint(doc, "grid_height", &gh);
    if (gw != OD_GRID_W || gh != OD_GRID_H) {
        report_error(r, where, "bad grid dimensions");
        return;
    }
    const cJSON *levels = jget(doc, "levels");
    if (levels != NULL && cJSON_IsArray(levels)) {
        int n = cJSON_GetArraySize(levels);
        for (int i = 0; i < n; i++)
            validate_level_obj(cJSON_GetArrayItem(levels, i), i, where, r);
    } else if (jget(doc, "grid") != NULL) {
        validate_level_obj(doc, 0, where, r);
    } else {
        report_error(r, where, "not a level or world file");
    }
}

static void validate_json_file(const char *path, const char *where, od_validate_report *r)
{
    char *text = NULL;
    size_t size = 0;
    if (!read_file(path, &text, &size)) {
        report_error(r, where, "cannot read");
        return;
    }
    cJSON *doc = cJSON_ParseWithLength(text, size);
    free(text);
    if (doc == NULL) {
        report_error(r, where, "not valid JSON");
        return;
    }
    validate_world_doc(doc, where, r);
    cJSON_Delete(doc);
}

/* C6.29 (C6.22 PUB-9): `--validate` runs the same parsers the game runs when
 * it loads the package, on a VFS that is never installed: schema_version and
 * declarations of every layer, engine tables, ROM/editor/scene/presentation
 * data, cinematics, levels (with level patches) and graphics (CHR and its
 * patches, screens, audio, worlds, palettes, sprites). What the game would
 * reject, --validate rejects, and the message says which layer and stage. */
od_status od_gfx_check(const od_content_vfs *vfs, char *stage, size_t cap);

static od_status loader_check(const char *const *roots, int count,
                               char *stage, size_t cap)
{
    snprintf(stage, cap, "declaration");
    od_content_vfs *vfs = (od_content_vfs *)calloc(1, sizeof *vfs);
    if (vfs == NULL)
        return OD_ERR_NOMEM;
    od_status st = OD_OK;
    for (int i = 0; i < count && st == OD_OK; i++) {
        st = layer_init(&vfs->layers[i], roots[i], i > 0);
        if (st == OD_OK)
            vfs->count++;
    }
    if (st == OD_OK)
        st = check_mods_against_pack(vfs);
    char err[256] = {0};
    int full = 0;
    if (st == OD_OK) {
        snprintf(stage, cap, "pack.json");
        char *text = NULL;
        size_t size = 0;
        st = od_content_vfs_read(vfs, "pack.json", &text, &size);
        cJSON *doc = st == OD_OK ? cJSON_ParseWithLength(text, size) : NULL;
        free(text);
        if (st == OD_OK && doc == NULL)
            st = OD_ERR_FORMAT;
        full = doc != NULL && jget(doc, "engine") != NULL;
        cJSON_Delete(doc);
    }
    if (st == OD_OK && full) {
        od_engine_tables *tables = (od_engine_tables *)malloc(sizeof *tables);
        od_rom_data *data = (od_rom_data *)malloc(sizeof *data);
        od_editor_data *editor = (od_editor_data *)malloc(sizeof *editor);
        od_scene_data *scene = (od_scene_data *)malloc(sizeof *scene);
        od_presentation_data *pres = (od_presentation_data *)malloc(sizeof *pres);
        if (!tables || !data || !editor || !scene || !pres) {
            st = OD_ERR_NOMEM;
        } else {
            snprintf(stage, cap, "engine tables");
            st = od_engine_tables_parse(tables, vfs, err, sizeof err);
            if (st == OD_OK) {
                snprintf(stage, cap, "ROM data (engine/rom-data.json)");
                st = od_rom_data_parse(data, vfs, err, sizeof err);
            }
            if (st == OD_OK) {
                snprintf(stage, cap, "editor data (engine/editor-data.json)");
                st = od_editor_data_parse(editor, vfs, err, sizeof err);
            }
            if (st == OD_OK) {
                snprintf(stage, cap, "scene data (engine/scene-data.json)");
                st = od_scene_data_parse(scene, vfs, err, sizeof err);
            }
            if (st == OD_OK) {
                snprintf(stage, cap, "presentation data (engine/presentation-data.json)");
                st = od_presentation_data_parse(pres, vfs, err, sizeof err);
            }
            if (st == OD_OK) {
                snprintf(stage, cap, "cinematics");
                st = od_content_cinematics_validate(vfs);
            }
        }
        free(tables); free(data); free(editor); free(scene); free(pres);
    }
    if (st == OD_OK) {
        snprintf(stage, cap, "levels");
        od_content_pack pack;
        st = od_content_pack_load(&pack, vfs);
        if (st == OD_OK)
            od_content_pack_free(&pack);
    }
    if (st == OD_OK && full)
        st = od_gfx_check(vfs, stage, cap);
    if (st != OD_OK && err[0] != '\0') {
        size_t n = strlen(stage);
        snprintf(stage + n, cap > n ? cap - n : 0, ": %s", err);
    }
    free(vfs);
    return st;
}

static void report_loader(od_validate_report *r, const char *who, const char *stage)
{
    char why[1400];
    if (g_content_error[0] != '\0')
        snprintf(why, sizeof why, "the game rejects %s at %.120s (%.300s)", who, stage,
                 g_content_error);
    else
        snprintf(why, sizeof why, "the game rejects %s at %.300s", who, stage);
    r->errors++;
    if (r->message[0] == '\0')
        snprintf(r->message, sizeof(r->message), "%.500s", why);
}

static void validate_pack_dir(const char *dir, od_validate_report *r)
{
    char path[1200];
    od_platform_join(path, sizeof(path), dir, "pack.json");
    char *text = NULL;
    size_t size = 0;
    if (!read_file(path, &text, &size)) {
        report_error(r, path, "cannot read pack.json");
        return;
    }
    cJSON *doc = cJSON_ParseWithLength(text, size);
    free(text);
    if (doc == NULL) {
        report_error(r, path, "pack.json is not valid JSON");
        return;
    }
    int schema = 0;
    if (!jint(doc, "schema_version", &schema) || schema != OD_PACK_SCHEMA_VERSION) {
        report_error(r, path, "bad schema_version");
        cJSON_Delete(doc);
        return;
    }
    r->packs++;
    const cJSON *files = jget(doc, "levels");
    if (files == NULL || !cJSON_IsArray(files)) {
        report_error(r, path, "pack.json has no levels list");
        cJSON_Delete(doc);
        return;
    }
    int n = cJSON_GetArraySize(files);
    for (int i = 0; i < n; i++) {
        const cJSON *rel = cJSON_GetArrayItem(files, i);
        if (rel == NULL || !cJSON_IsString(rel)) {
            report_error(r, path, "bad levels entry");
            continue;
        }
        if (!safe_rel_path(rel->valuestring)) {
            report_error(r, rel->valuestring, "path leaves the package directory");
            continue;
        }
        char full[1200];
        od_platform_join(full, sizeof(full), dir, rel->valuestring);
        validate_json_file(full, rel->valuestring, r);
    }
    if (r->errors == 0 && r->levels != OD_LEVEL_COUNT)
        report_error(r, path, "expected 150 levels");
    cJSON_Delete(doc);
    /* The whole loader, as the game runs it (after the level checks so their
     * precise messages stay first). */
    if (r->errors == 0) {
        char stage[400];
        const char *roots[1] = { dir };
        g_content_error[0] = '\0';
        if (loader_check(roots, 1, stage, sizeof stage) != OD_OK)
            report_loader(r, "the package", stage);
    }
}

static void validate_mod_dir(const char *dir, od_validate_report *r)
{
    char path[1200];
    od_platform_join(path, sizeof(path), dir, "mod.json");
    char *text = NULL;
    size_t size = 0;
    if (!read_file(path, &text, &size)) {
        report_error(r, path, "cannot read mod.json");
        return;
    }
    cJSON *doc = cJSON_ParseWithLength(text, size);
    free(text);
    if (doc == NULL) {
        report_error(r, path, "mod.json is not valid JSON");
        return;
    }
    r->packs++;
    const char *id = jstr(doc, "id");
    char mod_id[64];
    snprintf(mod_id, sizeof mod_id, "%s", id != NULL ? id : "mod");
    int schema = 0;
    if (!jint(doc, "schema_version", &schema) ||
        (schema != OD_PACK_SCHEMA_VERSION && schema != OD_CONTENT_SCHEMA_VERSION)) {
        char why[200];
        snprintf(why, sizeof why, "mod '%s': schema_version %d is not supported (this game "
                 "reads version %d)", mod_id, schema, OD_PACK_SCHEMA_VERSION);
        report_error(r, path, why);
        cJSON_Delete(doc);
        return;
    }
    g_content_error[0] = '\0';
    if (check_mod_declarations(dir, mod_id, doc) != OD_OK) {
        report_error(r, path, g_content_error);
        cJSON_Delete(doc);
        return;
    }
    const cJSON *files = jget(doc, "levels");
    int validated = 0;
    if (files != NULL && cJSON_IsArray(files)) {
        int n = cJSON_GetArraySize(files);
        for (int i = 0; i < n; i++) {
            const cJSON *rel = cJSON_GetArrayItem(files, i);
            char full[1200];
            od_platform_join(full, sizeof(full), dir, rel->valuestring);
            validate_json_file(full, rel->valuestring, r);
            validated++;
        }
    }
    cJSON_Delete(doc);

#if !defined(_WIN32)
    if (validated == 0) {
        char levels_dir[1200];
        od_platform_join(levels_dir, sizeof(levels_dir), dir, "levels");
        DIR *d = opendir(levels_dir);
        if (d != NULL) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                size_t len = strlen(ent->d_name);
                if (len < 5 || strcmp(ent->d_name + len - 5, ".json") != 0)
                    continue;
                char full[1200];
                od_platform_join(full, sizeof(full), levels_dir, ent->d_name);
                validate_json_file(full, ent->d_name, r);
            }
            closedir(d);
        }
    }
#else
    (void)validated;
#endif
    if (r->errors != 0)
        return;

    /* The game loads a mod on top of a package: check it the same way, on the
     * package the game would use (OD_CONTENT, the imported one, or the one
     * next to the executable). The base is checked alone first so a broken
     * base is never blamed on the mod. */
    char base[1024];
    if (od_content_resolve_base(base, sizeof base, NULL) != OD_OK) {
        char why[300];
        snprintf(why, sizeof why, "mod '%s': no content package to check it against "
                 "(import your ROM first, or set OD_CONTENT)", mod_id);
        report_error(r, dir, why);
        return;
    }
    char stage[400], who[400];
    const char *roots[2] = { base, dir };
    g_content_error[0] = '\0';
    if (loader_check(roots, 1, stage, sizeof stage) != OD_OK) {
        snprintf(who, sizeof who, "the base package '%.300s' (not the mod)", base);
        report_loader(r, who, stage);
        return;
    }
    g_content_error[0] = '\0';
    if (loader_check(roots, 2, stage, sizeof stage) != OD_OK) {
        snprintf(who, sizeof who, "mod '%s' (%.300s)", mod_id, dir);
        report_loader(r, who, stage);
    }
}

od_status od_content_validate(const char *path, od_validate_report *report)
{
    if (report == NULL)
        return OD_ERR_FORMAT;
    memset(report, 0, sizeof(*report));
    if (path == NULL || path[0] == '\0') {
        report_error(report, "validate", "no path");
        return OD_ERR_FORMAT;
    }

    char probe[1200];
    od_platform_join(probe, sizeof(probe), path, "pack.json");
    if (file_exists(probe)) {
        validate_pack_dir(path, report);
    } else {
        od_platform_join(probe, sizeof(probe), path, "mod.json");
        if (file_exists(probe))
            validate_mod_dir(path, report);
        else
            validate_json_file(path, path, report);
    }
    return report->errors == 0 ? OD_OK : OD_ERR_FORMAT;
}

/* ------------------------------------------------------------------------- */
/* Identity of an imported package (C6.33)                                    */
/* ------------------------------------------------------------------------- */

od_status od_content_check_identity(const char *base_dir, const char *reference,
                                      char *why, size_t cap)
{
    if (why != NULL && cap > 0)
        why[0] = '\0';
    if (base_dir == NULL || base_dir[0] == '\0' || reference == NULL) {
        set_err(why, cap, "no package directory or no reference manifest");
        return OD_ERR_RANGE;
    }
    size_t listed = 0, differ = 0;
    char first[400] = {0};
    for (const char *p = reference; *p != '\0';) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *next = nl ? nl + 1 : p + len;
        if (len == 0) {
            p = next;
            continue;
        }
        /* "<64 lower-case hex>  <relative path>", the importer's own format. */
        char rel[1024];
        int line_ok = len > 66 && p[64] == ' ' && p[65] == ' ';
        for (int i = 0; line_ok && i < 64; i++)
            line_ok = (p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f');
        if (!line_ok || len - 66 >= sizeof rel) {
            set_err(why, cap, "the reference manifest is malformed");
            return OD_ERR_FORMAT;
        }
        memcpy(rel, p + 66, len - 66);
        rel[len - 66] = '\0';
        if (!safe_rel_path(rel)) {
            set_err(why, cap, "the reference manifest names an unsafe path");
            return OD_ERR_FORMAT;
        }
        listed++;
        /* Only the files the reference lists are read (about 2 MB, all of it
         * read by the loader anyway). Extra files are not looked at: pack.json,
         * itself listed, decides what the game reads. */
        char path[4096];
        od_fs_join(path, sizeof path, base_dir, rel);
        size_t size = 0;
        char *data = od_fs_read(path, &size);
        if (data == NULL) {
            if (!differ++)
                snprintf(first, sizeof first, "%.300s is missing", rel);
        } else {
            uint8_t digest[32];
            char hex[65];
            od_sha256(data, size, digest);
            od_sha256_hex(digest, hex);
            free(data);
            if (memcmp(hex, p, 64) != 0 && !differ++)
                snprintf(first, sizeof first, "%.300s has a different SHA-256", rel);
        }
        p = next;
    }
    if (listed == 0) {
        set_err(why, cap, "the reference manifest is empty");
        return OD_ERR_FORMAT;
    }
    if (differ != 0) {
        if (why != NULL && cap > 0)
            snprintf(why, cap, "%zu of %zu file(s) differ; first: %s", differ, listed,
                     first);
        return OD_ERR_FORMAT;
    }
    return OD_OK;
}

od_status od_content_check_imported(const char *base_dir, char *why, size_t cap)
{
    const char *reference = od_import_reference_manifest();
    if (reference == NULL) {
        if (why != NULL && cap > 0)
            snprintf(why, cap, "out of memory building the reference manifest");
        return OD_ERR_NOMEM;
    }
    return od_content_check_identity(base_dir, reference, why, cap);
}
