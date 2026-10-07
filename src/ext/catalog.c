/* catalog.c - catalogue of custom levels (C5.4, C11 pure).
 *
 * See include/opendana/ext/catalog.h. The catalogue directory
 * holds the level files plus a derived `catalog.json`; the files are the
 * source of truth and the index is regenerated on every scan, so nothing is
 * lost if it disappears.
 */
#include "opendana/ext/catalog.h"
#include "opendana/userdir.h"   /* UTF-8 file access on Windows (C6.36) */
#if defined(_WIN32)
#include "../import/fsutil.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define OD_MKDIR(p) od_userdir_mkdir_p(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#define OD_MKDIR(p) mkdir(p, 0755)
#endif

/* ========================================================================= */
/* Paths and files                                                           */
/* ========================================================================= */

static void path_join(char *out, size_t cap, const char *dir, const char *name)
{
    if (dir == NULL || dir[0] == '\0' || strcmp(dir, ".") == 0)
        snprintf(out, cap, "%s", name);
    else
        snprintf(out, cap, "%s/%s", dir, name);
}

static int file_exists(const char *path)
{
    FILE *f = od_userdir_fopen(path, "rb");
    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

static void ensure_dirs(const char *dir)
{
    if (dir == NULL || dir[0] == '\0')
        return;
    char buf[1200];
    snprintf(buf, sizeof buf, "%s", dir);
    size_t start = (buf[0] == '/') ? 1 : 0;
    for (size_t i = start; ; i++) {
        char c = buf[i];
        if (c == '/' || c == '\0') {
            buf[i] = '\0';
            if (buf[0] != '\0')
                (void)OD_MKDIR(buf);
            buf[i] = c;
            if (c == '\0')
                break;
        }
    }
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0)
        return;
    if (src == NULL)
        src = "";
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

/* ========================================================================= */
/* Ids                                                                       */
/* ========================================================================= */

static int id_char_ok(unsigned char c, int first)
{
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (!first && (c == '.' || c == '-' || c == '_')) return 1;
    return 0;
}

static int valid_id(const char *id)
{
    if (id == NULL || id[0] == '\0' || strlen(id) >= OD_LED_ID_MAX)
        return 0;
    for (size_t i = 0; id[i]; i++)
        if (!id_char_ok((unsigned char)id[i], i == 0))
            return 0;
    return 1;
}

void od_catalog_make_id(const char *name, char *out, size_t cap)
{
    if (out == NULL || cap == 0)
        return;
    size_t o = 0;
    int pending_dash = 0;
    const char *p = name != NULL ? name : "";
    for (; *p && o + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (pending_dash && o > 0 && out[o - 1] != '-') {
                out[o++] = '-';
                if (o + 1 >= cap)
                    break;
            }
            pending_dash = 0;
            out[o++] = (char)c;
        } else if (c == '.' || c == '-' || c == '_') {
            /* keep separators only between alphanumerics */
            pending_dash = 1;
        } else {
            pending_dash = 1;
        }
    }
    /* Drop trailing separators. */
    while (o > 0 && out[o - 1] == '-')
        o--;
    if (o == 0)
        copy_str(out, cap, "level");
    else
        out[o] = '\0';
}

static int id_in_use(const char *dir, const char *id)
{
    char path[1400];
    char name[OD_LED_ID_MAX + 8];
    snprintf(name, sizeof name, "%s.json", id);
    path_join(path, sizeof path, dir, name);
    return file_exists(path);
}

static void unique_id(const char *dir, const char *name, char *out, size_t cap)
{
    char base[OD_LED_ID_MAX];
    od_catalog_make_id(name, base, sizeof base);
    if (!id_in_use(dir, base)) {
        copy_str(out, cap, base);
        return;
    }
    for (int k = 2; k < 10000; k++) {
        char probe[OD_LED_ID_MAX * 2];
        snprintf(probe, sizeof probe, "%s-%d", base, k);
        if (strlen(probe) < OD_LED_ID_MAX && !id_in_use(dir, probe)) {
            copy_str(out, cap, probe);
            return;
        }
    }
    copy_str(out, cap, base); /* extremely unlikely */
}

/* ========================================================================= */
/* Index writer                                                              */
/* ========================================================================= */

static void write_json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        if (*p == '"' || *p == '\\')
            fprintf(f, "\\%c", *p);
        else if (*p < 0x20)
            fprintf(f, "\\u%04x", (unsigned)*p);
        else
            fputc(*p, f);
    }
    fputc('"', f);
}

static void write_index(const char *dir, const od_catalog_entry *list, int n)
{
    char path[1400];
    path_join(path, sizeof path, dir, "catalog.json");
    char tmp[1400];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return;
    FILE *f = od_userdir_fopen(tmp, "wb");
    if (f == NULL)
        return;
    fputs("{\n  \"schema_version\": 1,\n  \"entries\": [", f);
    for (int i = 0; i < n; i++) {
        const od_catalog_entry *e = &list[i];
        fputs(i ? ",\n" : "\n", f);
        fputs("    { \"id\": ", f);
        write_json_string(f, e->id);
        fputs(", \"name\": ", f);
        write_json_string(f, e->name);
        fputs(", \"author\": ", f);
        write_json_string(f, e->author);
        fprintf(f, ", \"created\": %lld, \"modified\": %lld, \"theme_world\": %d",
                (long long)e->created, (long long)e->modified, e->theme_world);
        fputs(", \"file\": ", f);
        write_json_string(f, e->file);
        fputs(", \"thumb\": ", f);
        write_json_string(f, e->thumb);
        fputs(" }", f);
    }
    fputs("\n  ]\n}\n", f);
    if (fclose(f) != 0) {
        od_userdir_remove(tmp);
        return;
    }
    if (od_userdir_rename(tmp, path) != 0) {
        od_userdir_remove(path);
        if (od_userdir_rename(tmp, path) != 0)
            od_userdir_remove(tmp);
    }
}

/* ========================================================================= */
/* Directory listing                                                         */
/* ========================================================================= */

#if defined(_WIN32)
struct level_files { char **names; int n; };
static int collect_level_file(const char *name, void *ctx)
{
    struct level_files *lf = (struct level_files *)ctx;
    if (name[0] == '.' || strcmp(name, "catalog.json") == 0 || !ends_with(name, ".json"))
        return 0;
    char **grown = (char **)realloc(lf->names, (size_t)(lf->n + 1) * sizeof(char *));
    if (grown == NULL)
        return 1;
    lf->names = grown;
    lf->names[lf->n] = (char *)malloc(strlen(name) + 1);
    if (lf->names[lf->n] == NULL)
        return 1;
    strcpy(lf->names[lf->n], name);
    lf->n++;
    return 0;
}
#endif

/* Collect the `<name>.json` files other than catalog.json. Caller frees each
 * name and the array. Returns the count (0 on error or empty). */
static int list_level_files(const char *dir, char ***out)
{
    char **names = NULL;
    int n = 0;
#if defined(_WIN32)
    struct level_files lf = { NULL, 0 };
    (void)od_fs_list_dir(dir, collect_level_file, &lf);   /* wide, UTF-8 names */
    names = lf.names;
    n = lf.n;
#else
    DIR *d = opendir(dir);
    if (d == NULL) {
        *out = NULL;
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (name[0] == '.')
            continue;
        if (strcmp(name, "catalog.json") == 0 || !ends_with(name, ".json"))
            continue;
        char **grown = (char **)realloc(names, (size_t)(n + 1) * sizeof(char *));
        if (grown == NULL)
            break;
        names = grown;
        names[n] = (char *)malloc(strlen(name) + 1);
        if (names[n] == NULL)
            break;
        strcpy(names[n], name);
        n++;
    }
    closedir(d);
#endif
    /* Deterministic order. */
    for (int i = 1; i < n; i++) {
        char *key = names[i];
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) {
            names[j + 1] = names[j];
            j--;
        }
        names[j + 1] = key;
    }
    *out = names;
    return n;
}

static void free_names(char **names, int n)
{
    for (int i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

/* ========================================================================= */
/* Scan                                                                      */
/* ========================================================================= */

static int read_entry(const char *dir, const char *filename, od_catalog_entry *e)
{
    char path[1500];
    path_join(path, sizeof path, dir, filename);
    od_led_doc doc;
    memset(&doc, 0, sizeof doc);
    if (od_led_load_json(&doc, path) != OD_OK) {
        od_led_free(&doc);
        return 0;
    }
    memset(e, 0, sizeof *e);
    size_t stem_len = strlen(filename) - 5; /* ".json" */
    if (stem_len >= sizeof e->id)
        stem_len = sizeof e->id - 1;
    memcpy(e->id, filename, stem_len);
    e->id[stem_len] = '\0';
    copy_str(e->name, sizeof e->name,
             doc.name[0] ? doc.name : e->id);
    copy_str(e->author, sizeof e->author, doc.author);
    e->created = doc.created;
    e->modified = doc.modified;
    e->theme_world = doc.theme_world;
    copy_str(e->file, sizeof e->file, filename);
    char thumb[300];
    snprintf(thumb, sizeof thumb, "%.*s.png", (int)stem_len, filename);
    char thumb_path[1500];
    path_join(thumb_path, sizeof thumb_path, dir, thumb);
    if (file_exists(thumb_path))
        copy_str(e->thumb, sizeof e->thumb, thumb);
    od_led_free(&doc);
    return 1;
}

od_status od_catalog_scan(const char *dir, od_catalog_entry *out, int max,
                            int *n)
{
    if (n != NULL)
        *n = 0;
    if (dir == NULL || out == NULL || max <= 0)
        return OD_ERR_RANGE;

    char **names = NULL;
    int files = list_level_files(dir, &names);
    int stored = 0;
    int truncated = 0;
    for (int i = 0; i < files; i++) {
        od_catalog_entry e;
        if (!read_entry(dir, names[i], &e))
            continue;
        if (stored >= max) {
            truncated = 1;
            continue;
        }
        out[stored++] = e;
    }
    free_names(names, files);

    od_catalog_sort(out, stored, OD_CATALOG_SORT_NAME);
    write_index(dir, out, stored);
    if (n != NULL)
        *n = stored;
    return truncated ? OD_ERR_RANGE : OD_OK;
}

/* Refresh catalog.json without forcing the caller to provide a buffer. */
static void refresh_index(const char *dir)
{
    od_catalog_entry *list = (od_catalog_entry *)malloc(
        (size_t)OD_CATALOG_MAX_ENTRIES * sizeof *list);
    if (list == NULL)
        return;
    int n = 0;
    (void)od_catalog_scan(dir, list, OD_CATALOG_MAX_ENTRIES, &n);
    free(list);
}

/* ========================================================================= */
/* Save / delete / import / export                                           */
/* ========================================================================= */

od_status od_catalog_save(const char *dir, const od_led_doc *d,
                            char id_out[OD_LED_ID_MAX])
{
    if (dir == NULL || d == NULL || id_out == NULL)
        return OD_ERR_RANGE;
    ensure_dirs(dir);

    char id[OD_LED_ID_MAX];
    if (valid_id(d->id))
        copy_str(id, sizeof id, d->id);
    else
        unique_id(dir, d->name, id, sizeof id);

    od_led_doc copy = *d;
    copy_str(copy.id, sizeof copy.id, id);

    char name[OD_LED_ID_MAX + 8];
    char path[1500];
    snprintf(name, sizeof name, "%s.json", id);
    path_join(path, sizeof path, dir, name);
    od_status st = od_led_save_json(&copy, path);
    if (st != OD_OK)
        return st;

    refresh_index(dir);
    copy_str(id_out, OD_LED_ID_MAX, id);
    return OD_OK;
}

od_status od_catalog_delete(const char *dir, const char *id)
{
    if (dir == NULL || !valid_id(id))
        return OD_ERR_RANGE;
    char name[OD_LED_ID_MAX + 8];
    char path[1500];
    snprintf(name, sizeof name, "%s.json", id);
    path_join(path, sizeof path, dir, name);
    od_userdir_remove(path);
    snprintf(name, sizeof name, "%s.png", id);
    path_join(path, sizeof path, dir, name);
    od_userdir_remove(path);
    refresh_index(dir);
    return OD_OK;
}

od_status od_catalog_import(const char *dir, const char *path,
                              char id_out[OD_LED_ID_MAX])
{
    if (dir == NULL || path == NULL || id_out == NULL)
        return OD_ERR_RANGE;
    od_led_doc doc;
    memset(&doc, 0, sizeof doc);
    od_status st = od_led_load_json(&doc, path);
    if (st != OD_OK) {
        od_led_free(&doc);
        return st;
    }
    doc.id[0] = '\0'; /* force a fresh id in the destination catalogue */
    st = od_catalog_save(dir, &doc, id_out);
    od_led_free(&doc);
    return st;
}

od_status od_catalog_export(const char *dir, const char *id, const char *path)
{
    if (dir == NULL || !valid_id(id) || path == NULL)
        return OD_ERR_RANGE;
    char name[OD_LED_ID_MAX + 8];
    char src[1500];
    snprintf(name, sizeof name, "%s.json", id);
    path_join(src, sizeof src, dir, name);

    FILE *in = od_userdir_fopen(src, "rb");
    if (in == NULL)
        return OD_ERR_IO;
    FILE *out = od_userdir_fopen(path, "wb");
    if (out == NULL) {
        fclose(in);
        return OD_ERR_IO;
    }
    char buf[4096];
    size_t got;
    int ok = 1;
    while ((got = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, got, out) != got) { ok = 0; break; }
    if (ferror(in))
        ok = 0;
    if (fclose(in) != 0)
        ok = 0;
    if (fclose(out) != 0)
        ok = 0;
    return ok ? OD_OK : OD_ERR_IO;
}

/* ========================================================================= */
/* Sorting / lookup                                                          */
/* ========================================================================= */

static int entry_less(const od_catalog_entry *a, const od_catalog_entry *b,
                      od_catalog_order by)
{
    if (by == OD_CATALOG_SORT_MODIFIED) {
        if (a->modified != b->modified)
            return a->modified > b->modified;
        return strcmp(a->name, b->name) < 0;
    }
    int c = strcmp(a->name, b->name);
    if (c != 0)
        return c < 0;
    return strcmp(a->id, b->id) < 0;
}

void od_catalog_sort(od_catalog_entry *list, int n, od_catalog_order by)
{
    if (list == NULL)
        return;
    for (int i = 1; i < n; i++) {
        od_catalog_entry key = list[i];
        int j = i - 1;
        while (j >= 0 && entry_less(&key, &list[j], by)) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = key;
    }
}

int od_catalog_find(const od_catalog_entry *list, int n, const char *id)
{
    if (list == NULL || id == NULL)
        return -1;
    for (int i = 0; i < n; i++)
        if (strcmp(list[i].id, id) == 0)
            return i;
    return -1;
}
