/* import.c - orchestrate the one-time ROM import.
 *
 * Every extractor runs into a sibling temporary directory
 * `<dest>.tmp-<pid>-XXXXXX`. The finished tree is checked against the
 * versioned reference manifest (content_manifest.h, hashes only), flushed to
 * disk and only then renamed over the destination, so a crash, a `kill` or a
 * full disk never leaves a half imported package (C6.29, C6.22 PUB-6/PUB-7).
 * Leftovers of an interrupted import (`.tmp-*`, `.old-*` of a process that is
 * gone) are removed, or restored, at the next import and at startup.
 */
#include "opendana/import.h"

#include "content_manifest.h"
#include "fsutil.h"
#include "extract_cinematics.h"
#include "rom.h"
#include "sha256.h"
#include "opendana/content.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The extractors.
 * Each writes its files under `dir` (the temporary package root). */
od_status od_import_levels(const od_rom *rom, const char *dir, char *err, size_t cap);
od_status od_import_graphics(const od_rom *rom, const char *dir, char *err, size_t cap);
od_status od_import_audio(const od_rom *rom, const char *dir, char *err, size_t cap);
od_status od_import_palettes(const od_rom *rom, const char *dir, char *err, size_t cap);
od_status od_import_sprites(const od_rom *rom, const char *dir, char *err, size_t cap);
od_status od_import_worlds(const od_rom *rom, const char *dir, char *err, size_t cap);
od_status od_import_engine(const od_rom *rom, const char *dir, char *err, size_t cap);
od_status od_import_arena(const od_rom *rom, const char *dir, char *err, size_t cap);
od_status od_import_pack(const od_rom *rom, const char *dir, char *err, size_t cap);

typedef struct {
    const char *name;
    od_status (*fn)(const od_rom *, const char *, char *, size_t);
} extractor;

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0)
        snprintf(err, cap, "%s", msg);
}

/* The built-in reference as one text, built once from content_manifest.h. */
const char *od_import_reference_manifest(void)
{
    static char *text;
    if (text == NULL) {
        size_t n = sizeof(od_content_reference_lines) / sizeof(od_content_reference_lines[0]);
        size_t need = 1;
        for (size_t i = 0; i < n; i++)
            need += strlen(od_content_reference_lines[i]) + 1;
        char *built = (char *)malloc(need);
        if (built == NULL)
            return NULL;
        size_t at = 0;
        for (size_t i = 0; i < n; i++)
            at += (size_t)snprintf(built + at, need - at, "%s\n", od_content_reference_lines[i]);
        text = built;
    }
    return text;
}

od_status od_import_default_dir(char *out, size_t cap, const char *override)
{
    if (out == NULL || cap == 0)
        return OD_ERR_RANGE;
    if (override != NULL && override[0] != '\0') {
        if (strlen(override) >= cap)
            return OD_ERR_RANGE;
        snprintf(out, cap, "%s", override);
        return OD_OK;
    }
    /* C6.25: one helper decides the per-user folder (C6.31). On Windows it
     * is UTF-8 from the wide environment (a user named "Núñez", C6.29/C6.36). */
    char data[2048];
    if (od_userdir(OD_USERDIR_DATA, data, sizeof data) != OD_OK)
        return OD_ERR_IO;
    int n = snprintf(out, cap, "%s/%s", data, OD_IMPORT_CONTENT_REL);
    return (n > 0 && (size_t)n < cap) ? OD_OK : OD_ERR_RANGE;
}

int od_import_content_present(const char *dir)
{
    if (dir == NULL || dir[0] == '\0')
        return 0;
    char probe[4096];
    od_fs_join(probe, sizeof(probe), dir, "pack.json");
    return od_fs_exists(probe);
}

od_status od_import_resolve_content(char *out, size_t cap, const char *cli_override)
{
    if (out == NULL || cap == 0)
        return OD_ERR_RANGE;
    /* C6.29: startup also clears what an interrupted import left behind. */
    {
        char xdg_dir[1024];
        if (od_import_default_dir(xdg_dir, sizeof(xdg_dir), NULL) == OD_OK)
            (void)od_import_recover(xdg_dir);
    }
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
    char xdg_dir[1024];
    if (od_import_default_dir(xdg_dir, sizeof(xdg_dir), NULL) == OD_OK &&
        od_import_content_present(xdg_dir)) {
        if (strlen(xdg_dir) >= cap)
            return OD_ERR_RANGE;
        snprintf(out, cap, "%s", xdg_dir);
        return OD_OK;
    }
    /* Development fallback: next to the binary, then the CWD (A11). */
    return od_content_resolve_base(out, cap, NULL);
}

/* ------------------------------------------------------------------------- */
/* Leftovers of an interrupted import                                         */
/* ------------------------------------------------------------------------- */

typedef struct {
    char parent[4096];
    char dest[4096];
    char tmp_prefix[512];
    char old_prefix[512];
    int count;
    int failed;
} recover_ctx;

/* Parse the owning pid of "<prefix><pid>[-...]". 0 when there is none (a
 * leftover of the pre-C6.29 naming, which carried no pid). */
static long leftover_pid(const char *rest)
{
    long pid = 0;
    int digits = 0;
    while (*rest >= '0' && *rest <= '9' && digits < 10) {
        pid = pid * 10 + (*rest - '0');
        rest++;
        digits++;
    }
    if (digits == 0 || (*rest != '\0' && *rest != '-'))
        return 0;
    return pid;
}

static int recover_entry(const char *name, void *vctx)
{
    recover_ctx *c = (recover_ctx *)vctx;
    size_t nt = strlen(c->tmp_prefix), no = strlen(c->old_prefix);
    int is_tmp = strncmp(name, c->tmp_prefix, nt) == 0;
    int is_old = !is_tmp && strncmp(name, c->old_prefix, no) == 0;
    if (!is_tmp && !is_old)
        return 0;
    long pid = leftover_pid(name + (is_tmp ? nt : no));
    /* Another import that is still running owns it: leave it alone. */
    if (pid != 0 && pid != od_fs_getpid() && od_fs_process_alive(pid))
        return 0;
    char path[4096];
    od_fs_join(path, sizeof(path), c->parent, name);
    if (is_old && !od_fs_exists(c->dest)) {
        /* Killed between the two renames: the previous package is complete,
         * put it back where the game looks for it. */
        if (od_fs_rename(path, c->dest) == 0) {
            c->count++;
            return 0;
        }
    }
    if (od_fs_remove_r(path) == 0)
        c->count++;
    else
        c->failed = 1;
    return 0;
}

int od_import_recover(const char *dest_dir)
{
    if (dest_dir == NULL || dest_dir[0] == '\0')
        return -1;
    recover_ctx c;
    memset(&c, 0, sizeof(c));
    if (strlen(dest_dir) >= sizeof(c.dest))
        return -1;
    snprintf(c.dest, sizeof(c.dest), "%s", dest_dir);
    /* Strip trailing separators, then split into parent + leaf. */
    size_t n = strlen(c.dest);
    while (n > 1 && (c.dest[n - 1] == '/' || c.dest[n - 1] == '\\'))
        c.dest[--n] = '\0';
    const char *leaf = c.dest;
    for (const char *p = c.dest; *p; ++p)
        if (*p == '/' || *p == '\\') leaf = p + 1;
    if (leaf[0] == '\0' || strlen(leaf) + 8 >= sizeof(c.tmp_prefix))
        return -1;
    if (leaf == c.dest)
        snprintf(c.parent, sizeof(c.parent), ".");
    else
        snprintf(c.parent, sizeof(c.parent), "%.*s", (int)(leaf - c.dest - 1), c.dest);
    if (c.parent[0] == '\0')
        snprintf(c.parent, sizeof(c.parent), "/");
    snprintf(c.tmp_prefix, sizeof(c.tmp_prefix), "%s.tmp-", leaf);
    snprintf(c.old_prefix, sizeof(c.old_prefix), "%s.old-", leaf);
    if (!od_fs_is_dir(c.parent))
        return 0;
    if (od_fs_list_dir(c.parent, recover_entry, &c) != 0)
        return -1;
    if (c.count > 0)
        (void)od_fs_sync_parent(c.dest);
    return c.failed ? -1 : c.count;
}

/* ------------------------------------------------------------------------- */
/* Manifest                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    char rel[1024];
    char sha[65];
} manifest_entry;

typedef struct {
    manifest_entry *items;
    size_t count;
    size_t cap;
} manifest;

static int manifest_add(manifest *m, const char *rel, const char *sha)
{
    if (m->count == m->cap) {
        size_t ncap = m->cap ? m->cap * 2 : 64;
        manifest_entry *ni = (manifest_entry *)realloc(m->items, ncap * sizeof(*ni));
        if (ni == NULL)
            return -1;
        m->items = ni;
        m->cap = ncap;
    }
    snprintf(m->items[m->count].rel, sizeof(m->items[m->count].rel), "%s", rel);
    snprintf(m->items[m->count].sha, sizeof(m->items[m->count].sha), "%s", sha);
    m->count++;
    return 0;
}

static int manifest_scan(manifest *m, const char *root, const char *rel);

typedef struct {
    manifest *m;
    const char *root;
    const char *rel;
    int rc;
} scan_ctx;

static int scan_child(const char *name, void *vctx)
{
    scan_ctx *s = (scan_ctx *)vctx;
    char child_rel[1024];
    if (s->rel[0] == '\0')
        snprintf(child_rel, sizeof(child_rel), "%s", name);
    else
        snprintf(child_rel, sizeof(child_rel), "%s/%s", s->rel, name);
    if (manifest_scan(s->m, s->root, child_rel) != 0)
        s->rc = -1;
    return 0;
}

static int manifest_scan(manifest *m, const char *root, const char *rel)
{
    char path[4096];
    if (rel[0] == '\0')
        snprintf(path, sizeof(path), "%s", root);
    else
        od_fs_join(path, sizeof(path), root, rel);
    if (!od_fs_exists(path))
        return -1;
    if (od_fs_is_dir(path)) {
        scan_ctx s = { m, root, rel, 0 };
        if (od_fs_list_dir(path, scan_child, &s) != 0)
            return -1;
        return s.rc;
    }
    size_t size = 0;
    char *data = od_fs_read(path, &size);
    if (data == NULL)
        return -1;
    uint8_t digest[32];
    char hex[65];
    od_sha256(data, size, digest);
    od_sha256_hex(digest, hex);
    free(data);
    return manifest_add(m, rel, hex);
}

static int manifest_cmp(const void *a, const void *b)
{
    return strcmp(((const manifest_entry *)a)->rel, ((const manifest_entry *)b)->rel);
}

static char *manifest_text(const manifest *m)
{
    size_t need = 1;
    for (size_t i = 0; i < m->count; i++)
        need += strlen(m->items[i].sha) + 2 + strlen(m->items[i].rel) + 1;
    char *text = (char *)malloc(need);
    if (text == NULL)
        return NULL;
    size_t at = 0;
    for (size_t i = 0; i < m->count; i++)
        at += (size_t)snprintf(text + at, need - at, "%s  %s\n", m->items[i].sha, m->items[i].rel);
    text[at] = '\0';
    return text;
}

static int manifest_write(const char *path, const manifest *m)
{
    char *text = manifest_text(m);
    if (text == NULL)
        return -1;
    int rc = od_fs_write(path, text, strlen(text));
    free(text);
    return rc;
}

/* Compare the produced manifest with the reference text line by line and
 * describe the first difference. 0 when identical. */
static int manifest_check(const manifest *m, const char *reference, char *why, size_t cap)
{
    size_t i = 0, differ = 0;
    const char *p = reference;
    char first[600] = {0};
    while (*p != '\0' || i < m->count) {
        char line[1200] = {0};
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        int have_ref = *p != '\0';
        if (have_ref) {
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            memcpy(line, p, len);
        }
        const char *ref_rel = have_ref && len > 66 ? line + 66 : "";
        int c = !have_ref ? 1 : i >= m->count ? -1 : strcmp(ref_rel, m->items[i].rel);
        if (c == 0) {
            if (strncmp(line, m->items[i].sha, 64) != 0) {
                if (!differ++) snprintf(first, sizeof first, "%.300s has a different SHA-256", ref_rel);
            }
            i++;
            p = nl ? nl + 1 : p + strlen(p);
        } else if (c < 0) {
            if (!differ++) snprintf(first, sizeof first, "%.300s was not produced", ref_rel);
            p = nl ? nl + 1 : p + strlen(p);
        } else {
            if (!differ++) snprintf(first, sizeof first, "%.300s is not in the reference", m->items[i].rel);
            i++;
        }
    }
    if (differ && why != NULL && cap > 0)
        snprintf(why, cap, "the imported content does not match the reference manifest "
                 "(%zu difference(s); first: %s); nothing was installed", differ, first);
    return differ ? -1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Orchestration                                                              */
/* ------------------------------------------------------------------------- */

od_status od_import_rom_with_reference(const char *rom_path, const char *dest_dir,
                                         const char *reference, const char *manifest_out,
                                         char *err, size_t err_cap)
{
    if (rom_path == NULL || dest_dir == NULL || dest_dir[0] == '\0') {
        set_err(err, err_cap, "missing ROM path or destination directory");
        return OD_ERR_RANGE;
    }
    if (reference == NULL || reference[0] == '\0') {
        set_err(err, err_cap, "no reference manifest to check the import against");
        return OD_ERR_FORMAT;
    }

    od_rom rom;
    od_status st = od_rom_load(&rom, rom_path, err, err_cap);
    if (st != OD_OK)
        return st;

    char tmp[4096];
    /* Reserve suffix space for temporary, backup and manifest paths. */
    if (strlen(dest_dir) > sizeof(tmp) - 64) {
        od_rom_free(&rom);
        set_err(err, err_cap, "destination path is too long");
        return OD_ERR_RANGE;
    }
    if (od_fs_mkdir_parent(dest_dir) != 0) {
        od_rom_free(&rom);
        set_err(err, err_cap, "cannot create the user content directory");
        return OD_ERR_IO;
    }
    /* What an earlier, interrupted import left behind goes first. */
    (void)od_import_recover(dest_dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp-%ld-XXXXXX", dest_dir, od_fs_getpid());
    if (od_fs_mkdtemp(tmp) != 0) {
        od_rom_free(&rom);
        set_err(err, err_cap, "cannot create a temporary import directory");
        return OD_ERR_IO;
    }

    static const extractor extractors[] = {
        {"levels", od_import_levels},
        {"graphics", od_import_graphics},
        {"audio", od_import_audio},
        {"palettes", od_import_palettes},
        {"sprites", od_import_sprites},
        {"worlds", od_import_worlds},
        {"engine", od_import_engine},
        {"cinematics", od_import_cinematics},
        {"arena", od_import_arena},
    };
    for (size_t i = 0; i < sizeof(extractors) / sizeof(extractors[0]); i++) {
        st = extractors[i].fn(&rom, tmp, err, err_cap);
        if (st == OD_OK && strcmp(extractors[i].name, "arena") == 0)
            st = od_import_pack(&rom, tmp, err, err_cap);
        if (st != OD_OK) {
            od_fs_remove_r(tmp);
            od_rom_free(&rom);
            return st;
        }
    }
    od_rom_free(&rom);

    manifest m;
    memset(&m, 0, sizeof(m));
    if (manifest_scan(&m, tmp, "") != 0) {
        free(m.items);
        od_fs_remove_r(tmp);
        set_err(err, err_cap, "cannot compute the content manifest");
        return OD_ERR_IO;
    }
    qsort(m.items, m.count, sizeof(*m.items), manifest_cmp);

    /* The versioned reference decides. It is never
     * created, rewritten or skipped here (C6.22 PUB-6). */
    if (manifest_check(&m, reference, err, err_cap) != 0) {
        free(m.items);
        od_fs_remove_r(tmp);
        return OD_ERR_FORMAT;
    }
    /* Optional copy of the produced manifest (tests, tools). */
    if (manifest_out != NULL && manifest_out[0] != '\0' && manifest_write(manifest_out, &m) != 0) {
        free(m.items);
        od_fs_remove_r(tmp);
        set_err(err, err_cap, "cannot write the content manifest copy");
        return OD_ERR_IO;
    }
    free(m.items);

    /* Make the finished tree durable before it becomes visible. */
    if (od_fs_sync_tree(tmp) != 0) {
        od_fs_remove_r(tmp);
        set_err(err, err_cap, "cannot flush the imported content to disk");
        return OD_ERR_IO;
    }

    /* Swap the finished tree in, keeping the previous one only until rename. */
    char old[4096];
    snprintf(old, sizeof(old), "%s.old-%ld", dest_dir, od_fs_getpid());
    int had_old = 0;
    if (od_fs_exists(dest_dir)) {
        od_fs_remove_r(old);
        if (od_fs_rename(dest_dir, old) != 0) {
            od_fs_remove_r(tmp);
            set_err(err, err_cap, "cannot replace the existing content directory");
            return OD_ERR_IO;
        }
        had_old = 1;
    }
    if (od_fs_rename(tmp, dest_dir) != 0) {
        if (had_old)
            od_fs_rename(old, dest_dir);
        od_fs_remove_r(tmp);
        set_err(err, err_cap, "cannot move the imported content into place");
        return OD_ERR_IO;
    }
    (void)od_fs_sync_parent(dest_dir);
    if (had_old)
        od_fs_remove_r(old);
    return OD_OK;
}

od_status od_import_rom(const char *rom_path, const char *dest_dir,
                          const char *manifest_path, char *err, size_t err_cap)
{
    const char *reference = od_import_reference_manifest();
    if (reference == NULL) {
        set_err(err, err_cap, "out of memory building the reference manifest");
        return OD_ERR_NOMEM;
    }
    return od_import_rom_with_reference(rom_path, dest_dir, reference,
                                         manifest_path, err, err_cap);
}
