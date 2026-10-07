#include "opendana/assets.h"
#include "opendana/content_gfx.h"
#include "opendana/content_tables.h"
#include "opendana/rom_data.h"
#include "opendana/editor_data.h"
#include "opendana/scene_data.h"
#include "opendana/presentation_data.h"
#include "opendana/world_gfx.h"
#include "../core/content_snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void od_path_join(char *out, size_t cap, const char *dir, const char *name)
{
    if (dir == NULL || dir[0] == '\0' || strcmp(dir, ".") == 0) {
        snprintf(out, cap, "%s", name);
        return;
    }
    size_t n = strlen(dir);
    snprintf(out, cap, dir[n - 1] == '/' ? "%s%s" : "%s/%s", dir, name);
}

/* Internal semantic parser gate; no lazy/default resolver or publication. */
od_status od_content_cinematics_validate(const od_content_vfs *vfs);

static od_status assets_load_captured(od_assets *out, const od_content_vfs *vfs)
{
    if (out == NULL || vfs == NULL)
        return OD_ERR_FORMAT;
    memset(out, 0, sizeof(*out));
    /* Synthetic level-only VFSs are useful to module tests, but a product
     * bundle must have its own engine data; never lazy-load a different base. */
    od_engine_tables tables;
    od_status st = od_engine_tables_parse(&tables, vfs, NULL, 0);
    if (st != OD_OK)
        return st;
    od_rom_data data;
    st = od_rom_data_parse(&data, vfs, NULL, 0);
    if (st != OD_OK)
        return st;
    od_audio_layout audio_layout;
    od_status layout_st = od_audio_layout_parse(&audio_layout, vfs, NULL, 0);
    if (layout_st != OD_OK && layout_st != OD_ERR_IO)
        return layout_st;
    od_editor_data editor;
    st = od_editor_data_parse(&editor, vfs, NULL, 0);
    if (st != OD_OK)
        return st;
    od_scene_data scene;
    st = od_scene_data_parse(&scene, vfs, NULL, 0);
    if (st != OD_OK)
        return st;
    od_presentation_data presentation;
    st = od_presentation_data_parse(&presentation, vfs, NULL, 0);
    if (st != OD_OK)
        return st;
    st = od_content_cinematics_validate(vfs);
    if (st != OD_OK)
        return st;
    od_content_pack pack;
    st = od_content_pack_load(&pack, vfs);
    if (st != OD_OK)
        return st;
    uint8_t *copy = malloc(OD_CHR_BYTES);
    if (copy == NULL) { od_content_pack_free(&pack); return OD_ERR_NOMEM; }
    st = od_gfx_load(vfs);
    if (st == OD_OK) {
        const od_world_gfx *world = od_world_gfx_for_world(0);
        if (world == NULL || od_gfx_chr_size() != OD_CHR_BYTES)
            st = OD_ERR_FORMAT;
        else {
            /* No lazy/default calls; all inputs validated, and the bound
             * CHR allocation was reserved before graphics publication. */
            st = od_presentation_data_install(&presentation);
            if (st == OD_OK) st = od_scene_data_install(&scene);
            if (st == OD_OK) st = od_editor_data_install(&editor);
            if (st == OD_OK) st = od_rom_data_install(&data);
            if (st == OD_OK) st = od_audio_layout_install(layout_st == OD_OK ? &audio_layout : NULL);
            if (st == OD_OK) {
                od_engine_tables_install(&tables);
                od_content_snapshot_completed(vfs, OD_SNAPSHOT_TABLES);
                out->chr = copy;
                copy = NULL;
                out->chr_size = od_gfx_chr_size();
                memcpy(out->chr, od_gfx_chr(), out->chr_size);
                memcpy(out->palette_ram.ram, world->palette_ram, 32);
                out->levels = pack.levels;
                memset(&pack.levels, 0, sizeof(pack.levels));
                od_gfx_bind_assets(out);
            }
        }
    }
    free(copy);
    od_content_pack_free(&pack);
    return st;
}

/* Internal, content.c (C6.29): say which layer and stage the game rejects. */
unsigned od_content_error_serial(void);
void od_content_explain_failure(const char *base_dir, const char *const *mod_dirs, int mod_count);

od_status od_assets_load_content(od_assets *out, const od_content_vfs *vfs)
{
    if (!out || !vfs) return OD_ERR_FORMAT;
    memset(out, 0, sizeof *out);
    od_content_vfs captured;
    unsigned serial = od_content_error_serial();
    od_status st = od_content_vfs_capture(vfs, &captured);
    if (st != OD_OK) return st;
    st = assets_load_captured(out, &captured);
    od_content_snapshot_release(captured.snapshot);
    if (st != OD_OK && st != OD_ERR_NOMEM && od_content_error_serial() == serial &&
        vfs->count >= 1) {
        const char *mods[OD_CONTENT_MAX_LAYERS];
        int n = 0;
        for (int i = 1; i < vfs->count && n < OD_CONTENT_MAX_LAYERS; i++)
            mods[n++] = vfs->layers[i].root;
        od_content_explain_failure(vfs->layers[0].root, mods, n);
    }
    return st;
}

od_status od_assets_load(od_assets *out, const char *dir)
{
    (void)dir;
    if (out == NULL)
        return OD_ERR_FORMAT;
    memset(out, 0, sizeof(*out));
    char base[1024];
    od_content_vfs vfs = {0};
    od_status st = od_content_resolve_base(base, sizeof(base), NULL);
    if (st == OD_OK)
        st = od_content_vfs_init(&vfs, base, NULL, 0);
    if (st == OD_OK) st = od_assets_load_content(out, &vfs);
    od_content_vfs_free(&vfs);
    return st;
}

void od_assets_free(od_assets *assets)
{
    if (assets == NULL)
        return;
    od_gfx_bind_assets(NULL);
    od_levelset_free(&assets->levels);
    free(assets->chr);
    assets->chr = NULL;
    assets->chr_size = 0;
}
