/* state.c - save states: the `.odstate` format (C5.3).
 *
 * Pure C11: no SDL, only stdio. The session (`od_flow_state`) is the game's
 * deterministic state; this module wraps it in a small versioned container
 * together with the sequencer block, and refuses anything that does not match
 * the running build and content (decision D3).
 *
 * The transient pointers inside `od_flow_state` (the loaded track change
 * arrays, the running magicword program, the audio callbacks/engine) are not
 * written as addresses: the change arrays are zeroed and re-linked from the
 * freshly loaded screens on `od_state_load`, the magicword program is stored
 * as a 1/2 kind, and `od_flow_state_rebind` turns the kinds back into art
 * pointers. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "opendana/ext/state.h"
#include "opendana/userdir.h"

#include "opendana/audio.h"
#include "opendana/content_gfx.h"
#include "../import/sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define STATE_HEADER 42u
#define STATE_IDENTITY 72u
#define STATE_TRAILER 4u

/* ---- little-endian helpers ---------------------------------------------- */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static void put_i16(uint8_t *p, int16_t v) { put_u16(p, (uint16_t)v); }
static void put_i32(uint8_t *p, int32_t v) { put_u32(p, (uint32_t)v); }

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static int16_t rd_i16(const uint8_t *p) { return (int16_t)rd_u16(p); }
static int32_t rd_i32(const uint8_t *p) { return (int32_t)rd_u32(p); }

/* ---- identity ----------------------------------------------------------- */

/* C6.15 epoch 3: cleanup_stage consumes former cinematic POD padding, so
 * sizeof/offset hashing alone cannot invalidate epoch2 cleanup semantics. */
#define OD_STATE_ABI 0x534b325343363703ull

uint64_t od_state_build_id(void)
{
    static const size_t sizes[] = {
        sizeof(od_flow_state), sizeof(od_session), sizeof(od_screens),
        sizeof(od_game), sizeof(od_play_view), sizeof(od_password_view),
        sizeof(od_editor_view), sizeof(od_audio_driver), sizeof(od_audio),
    };
    static const size_t offs[] = {
        offsetof(od_flow_state, session), offsetof(od_flow_state, screens),
        offsetof(od_flow_state, game), offsetof(od_flow_state, view),
        offsetof(od_flow_state, evw), offsetof(od_flow_state, sim_tick),
        offsetof(od_flow_state, rgb_hash),
    };
    uint64_t h = OD_STATE_ABI;
    h = od_fnv1a64(sizes, sizeof sizes, h);
    h = od_fnv1a64(offs, sizeof offs, h);
    size_t engine = od_audio_state_size();
    h = od_fnv1a64(&engine, sizeof engine, h);
    return h;
}

uint32_t od_state_content_hash(const od_levelset *levels)
{
    uint32_t h = OD_FNV1A32_INIT;
    if (levels == NULL)
        return h;
    h = od_fnv1a32(&levels->count, sizeof levels->count, h);
    for (int i = 0; i < levels->count; i++) {
        const od_level *lv = &levels->levels[i];
        h = od_fnv1a32(lv->grid, sizeof lv->grid, h);
        h = od_fnv1a32(&lv->player_position, sizeof lv->player_position, h);
        h = od_fnv1a32(&lv->metadata, sizeof lv->metadata, h);
        if (lv->command_len > 0 && lv->commands != NULL)
            h = od_fnv1a32(lv->commands, lv->command_len, h);
    }
    return h;
}

uint32_t od_state_checksum(const void *data, size_t len)
{
    return od_fnv1a32(data, len, OD_FNV1A32_INIT);
}

static uint32_t flow_content(const od_flow *f)
{
    if (f == NULL || f->assets == NULL)
        return OD_FNV1A32_INIT;
    uint32_t h = od_state_content_hash(&f->assets->levels);
    if (f->assets->chr != NULL)
        h = od_fnv1a32(f->assets->chr, f->assets->chr_size, h);
    return od_fnv1a32(&f->assets->palette_ram, sizeof f->assets->palette_ram, h);
}

/* SHA-256 identities use byte content, never an absolute host path. */
od_status od_state_file_digest(const char *path, uint8_t out[32])
{
    if (!path || !out) return OD_ERR_RANGE;
    FILE *in = od_userdir_fopen(path, "rb");
    if (!in) return OD_ERR_IO;
    od_sha256_ctx hash; od_sha256_init(&hash);
    uint8_t chunk[8192]; size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, in)) != 0)
        od_sha256_update(&hash, chunk, n);
    int bad = ferror(in); if (fclose(in) != 0) bad = 1;
    if (bad) return OD_ERR_IO;
    od_sha256_final(&hash, out); return OD_OK;
}

static int file_order(const void *a, const void *b)
{ return strcmp(*(const char *const *)a, *(const char *const *)b); }
static void hash_name(od_sha256_ctx *h, const char *name)
{
    uint8_t length[8]; put_u64(length,(uint64_t)strlen(name));
    od_sha256_update(h,length,sizeof length); od_sha256_update(h,name,strlen(name));
}
od_status od_state_resources_digest(const od_content_vfs *vfs,
    const char *const *paths, size_t count, uint8_t out[32])
{
    if (!vfs || !out || !paths || !count || count > 16384 ||
        vfs->count < 1 || vfs->count > OD_CONTENT_MAX_LAYERS)
        return OD_ERR_RANGE;
    const char **sorted = malloc(count * sizeof *sorted);
    if (!sorted) return OD_ERR_NOMEM;
    for (size_t i=0;i<count;i++) {
        if (!paths[i] || !*paths[i] || strlen(paths[i]) > 4096) {
            free(sorted); return OD_ERR_RANGE;
        }
        sorted[i] = paths[i];
    }
    if (count > 1) qsort(sorted,count,sizeof *sorted,file_order);
    od_status result = OD_OK;
    od_sha256_ctx hash; od_sha256_init(&hash);
    for (size_t i=0;i<count && result == OD_OK;i++) {
        if (i && strcmp(sorted[i],sorted[i-1]) == 0) continue;
        char *bytes = NULL; size_t size = 0;
        /* Never duplicate overlay/fallback policy: exactly the loader's VFS. */
        result = od_content_vfs_read(vfs,sorted[i],&bytes,&size);
        if (result == OD_OK) {
            /* C6.2 canonical resource encoding: no physical paths, descriptor
             * hashes, nested digests or extra domain bytes. */
            uint8_t length[8]; put_u64(length,(uint64_t)size);
            hash_name(&hash,sorted[i]); od_sha256_update(&hash,length,sizeof length);
            od_sha256_update(&hash,bytes,size);
        }
        free(bytes);
    }
    free(sorted);
    if (result == OD_OK) od_sha256_final(&hash,out);
    return result;
}

od_status od_state_vfs_digest(const od_content_vfs *vfs, uint8_t out[32])
{
#if defined(OD_CONTENT_SNAPSHOT_API) && OD_CONTENT_SNAPSHOT_API == 1
    /* C6.2 certifies pinned bytes AND full loader seals. Paths-only inventory
     * is not provenance, even if every returned path was successfully read. */
    return od_content_vfs_snapshot_digest(vfs,out);
#else
    (void)vfs; (void)out;
    fprintf(stderr,"state: verified loaded content snapshot API unavailable\n");
    return OD_ERR_FORMAT; /* Fail closed; an all-files walker was NOT identity. */
#endif
}

/* ---- layout-independent resume section (C6.62) ------------------------- */

#define SEC_FIXED 137u
#define SEC_VERSION 1u

static int nonzero32(const uint8_t *p)
{
    for (int i = 0; i < 32; i++)
        if (p[i]) return 1;
    return 0;
}

static size_t section_size(const od_flow_resume *r)
{
    return SEC_FIXED + (r->kind == OD_RESUME_LEVEL ? r->count : 0);
}

static void section_write(uint8_t *p, const od_flow_resume *r,
                          const od_state_context *ctx)
{
    memcpy(p, "ODRS", 4);
    put_u16(p + 4, SEC_VERSION);
    p[6] = (uint8_t)r->kind;
    if (ctx) {
        memcpy(p + 8, ctx->rom, 32);
        memcpy(p + 40, ctx->mods, 32);
    }
    memcpy(p + 72, r->cleared, sizeof r->cleared);
    put_i16(p + 91, (int16_t)r->progress_world);
    put_i16(p + 93, (int16_t)r->cursor);
    put_i16(p + 95, (int16_t)r->session_level);
    put_i16(p + 97, (int16_t)r->world);
    put_i16(p + 99, (int16_t)r->round);
    put_i16(p + 101, (int16_t)r->level);
    put_u32(p + 103, r->menu_repeat);
    memcpy(p + 107, r->menu_counters, 8);
    p[115] = r->oam_rotation;
    p[116] = r->oam_known;
    put_u64(p + 117, (uint64_t)r->start_tick);
    put_u64(p + 125, (uint64_t)r->tick);
    put_u32(p + 133, r->kind == OD_RESUME_LEVEL ? (uint32_t)r->count : 0u);
    if (r->kind == OD_RESUME_LEVEL)
        memcpy(p + SEC_FIXED, r->buttons, r->count);
}

/* Parse and bound-check a section. The buttons borrow `p`. */
static int section_parse(const uint8_t *p, size_t len, od_flow_resume *r,
                         const uint8_t **rom, const uint8_t **mods)
{
    memset(r, 0, sizeof *r);
    if (len < SEC_FIXED || memcmp(p, "ODRS", 4) != 0 || rd_u16(p + 4) != SEC_VERSION)
        return 0;
    uint32_t count = rd_u32(p + 133);
    r->kind = p[6];
    if (r->kind != OD_RESUME_NONE && r->kind != OD_RESUME_MAP &&
        r->kind != OD_RESUME_LEVEL)
        return 0;
    if (count > OD_RESUME_MAX_TICKS ||
        len != SEC_FIXED + (r->kind == OD_RESUME_LEVEL ? count : 0) ||
        (r->kind != OD_RESUME_LEVEL && count != 0))
        return 0;
    *rom = p + 8;
    *mods = p + 40;
    memcpy(r->cleared, p + 72, sizeof r->cleared);
    r->progress_world = rd_i16(p + 91);
    r->cursor = rd_i16(p + 93);
    r->session_level = rd_i16(p + 95);
    r->world = rd_i16(p + 97);
    r->round = rd_i16(p + 99);
    r->level = rd_i16(p + 101);
    r->menu_repeat = rd_u32(p + 103);
    memcpy(r->menu_counters, p + 107, 8);
    r->oam_rotation = p[115];
    r->oam_known = p[116];
    r->start_tick = (long)rd_u64(p + 117);
    r->tick = (long)rd_u64(p + 125);
    r->buttons = p + SEC_FIXED;
    r->count = count;
    return 1;
}

static int resume_ranges_valid(const od_flow_resume *r, const od_flow *live)
{
    int levels = live && live->assets ? live->assets->levels.count : 0;
    if (levels <= 0 || levels > OD_LEVEL_COUNT)
        return 0;
    if (r->kind != OD_RESUME_MAP && r->kind != OD_RESUME_LEVEL)
        return 0;
    if (r->session_level < -1 || r->session_level >= OD_LEVEL_COUNT ||
        r->world < 0 || r->world >= OD_LEVEL_COUNT / OD_WORLD_ROUNDS ||
        r->round < 0 || r->round >= OD_WORLD_ROUNDS ||
        r->progress_world < 0 || r->progress_world >= OD_WORLD_COUNT ||
        r->cursor < 0 || r->cursor >= OD_MAP_SLOTS || r->tick < 0)
        return 0;
    for (int i = OD_LEVEL_COUNT; i < (int)sizeof r->cleared * 8; i++)
        if (r->cleared[i >> 3] & (1u << (i & 7)))
            return 0;
    if (r->kind == OD_RESUME_LEVEL &&
        (r->level < 0 || r->level >= levels || r->count == 0 ||
         r->start_tick < 0 || r->tick != r->start_tick + (long)r->count))
        return 0;
    return 1;
}

/* The ROM hash is written by the importer in the package's source records; the
 * mod set is the ordered list of the identifiers of the layers above the base. */
od_status od_state_rom_identity(const od_content_vfs *vfs, uint8_t rom[32],
                                uint8_t mods[32])
{
    if (!vfs || !rom || !mods) return OD_ERR_RANGE;
    memset(rom, 0, 32);
    memset(mods, 0, 32);
    od_sha256_ctx hash; od_sha256_init(&hash);
    for (int i = 1; i < vfs->count; i++)
        od_sha256_update(&hash, vfs->layers[i].id, strlen(vfs->layers[i].id) + 1);
    od_sha256_final(&hash, mods);
    char *text = NULL; size_t size = 0;
    if (od_content_vfs_read(vfs, "screens/cinematics/bootstrap.json", &text, &size) != OD_OK)
        return OD_ERR_IO;
    od_status st = OD_ERR_FORMAT;
    const char *key = strstr(text, "\"rom_sha256\"");
    if (key != NULL && (key = strchr(key + 12, '"')) != NULL) {
        key++;
        uint8_t out[32];
        int ok = 1;
        for (int i = 0; i < 32 && ok; i++) {
            int v = 0;
            for (int k = 0; k < 2; k++) {
                char c = key[2 * i + k];
                int d = c >= '0' && c <= '9' ? c - '0' :
                        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0) { ok = 0; break; }
                v = v * 16 + d;
            }
            out[i] = (uint8_t)v;
        }
        if (ok) { memcpy(rom, out, 32); st = OD_OK; }
    }
    free(text);
    return st;
}

/* ---- pointer stripping / rebuilding ------------------------------------- */

/* Replace every transient pointer in a state copy with a value that survives
 * the process boundary: NULL, or a tiny kind the rebind decodes. */
static void strip_pointers(od_flow_state *s)
{
    for (int i = 0; i < OD_SCREEN_TRACKS; i++)
        s->screens.tracks[i].changes = NULL;
    s->screens.cinematic_data = NULL;
    s->screens.bootstrap_data = NULL;
    s->screens.editor_cinematic_data = NULL;
    s->screens.castle_round10_data = NULL;
    s->screens.world_clear_data = NULL;
    s->screens.goodnight_hidden_data = NULL;
    s->screens.boss_ending_data = NULL;
    s->screens.post_ending_data = NULL;
    s->screens.cinematic.data = NULL;
    if (s->screens.mw.prog != NULL)
        s->screens.mw.prog = (const od_mw_step *)(uintptr_t)
            (s->screens.mw.state == 0 ? 1u : 2u);
    s->view.fade = NULL;
    /* Editor levels are synthesized from inline cells (flow.c), never borrow
     * the command stream of a package level. Still strip this pointer slot. */
    s->evw.level.commands = NULL;
    s->audio.tick = NULL;
    s->audio.request = NULL;
    s->audio.play_music = NULL;
    s->audio.reset_init = NULL;
    s->audio.ctx = NULL;
    s->audio_core.engine = NULL;
}

/* ---- file helpers ------------------------------------------------------- */

static od_status read_file(const char *path, uint8_t **out, size_t *out_size)
{
    FILE *f = od_userdir_fopen(path, "rb");
    if (f == NULL)
        return OD_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return OD_ERR_IO; }
    long len = ftell(f);
    if (len < 0 || (unsigned long)len > OD_STATE_MAX_BYTES) {
        fclose(f);
        return OD_ERR_FORMAT;
    }
    rewind(f);
    uint8_t *buf = malloc((size_t)len + 1);
    if (buf == NULL) { fclose(f); return OD_ERR_NOMEM; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) { free(buf); return OD_ERR_IO; }
    *out = buf;
    *out_size = (size_t)len;
    return OD_OK;
}

/* C6.31: one atomic writer for every user file (opendana/userdir.h). */
static od_status write_atomic(const char *path, const uint8_t *buf, size_t len)
{
    return od_userdir_write_atomic(path, buf, len);
}

/* ---- save --------------------------------------------------------------- */

static int native_tasks_valid(const od_cinematic *s)
{
    /* Public native task state, not CPU state. Producer cinematic.c defines
     * ENTRY_RUN/INIT/PALETTE/CLEAR = 0..3 and the two reset observations.
     * Do not normalize an unknown stage: reject before audio/host mutation.
     * Program/transfer bounds require the owner's opaque-data validator. */
    return s->entry_stage <= 3 && s->reset_sync <= 4 && s->render_enabled <= 1 &&
           s->sounds.managed <= 1 && s->sounds.count <= 8 && !s->sounds.overflow;
}

static int state_enums_valid(const od_flow_state *s)
{
    return (s->screen == OD_FLOW_SEQUENCE || s->screen == OD_FLOW_PLAY ||
            s->screen == OD_FLOW_UNVERIFIED) &&
           (unsigned)s->screens.scene <= OD_SCREEN_UNVERIFIED &&
           (unsigned)s->screens.next <= OD_SCREEN_UNVERIFIED &&
             s->screens.track < OD_SCREEN_TRACKS && native_tasks_valid(&s->screens.cinematic) &&
             !s->screens.editor_sounds.overflow &&
             od_screens_editor_status(&s->screens)==OD_OK &&
             od_screens_castle_round10_status(&s->screens)==OD_OK &&
             od_screens_world_clear_status(&s->screens)==OD_OK &&
             od_screens_goodnight_hidden_status(&s->screens)==OD_OK &&
             od_screens_boss_ending_status(&s->screens)==OD_OK &&
             /* Opening root42 never enables START; a wire flag cannot grant
              * permission even though it is a structurally valid boolean. */
             !(s->screens.world_clear_phase && s->screens.world_clear_part==1 &&
               s->screens.cinematic.skip_enabled) &&
             od_editor_action_status(&s->evw.ed)==OD_OK &&
            od_editor_action_status(&s->evw.shown)==OD_OK;
}

/* C6.31 (CORE-13): the flow indexes the level table, the progress bitmap and
 * its own staged loads with these fields straight from the payload. A state
 * whose checksum is right but whose values are out of range (a hand-edited
 * file) is rejected before anything is applied, never used as an index. */
static int state_ranges_valid(const od_flow_state *s, const od_flow *live)
{
    int levels = live && live->assets ? live->assets->levels.count : OD_LEVEL_COUNT;
    if (levels <= 0 || levels > OD_LEVEL_COUNT)
        return 0;
    if (s->level < -1 || s->level >= levels)
        return 0;
    if (s->screen == OD_FLOW_PLAY && s->playing && s->level < 0)
        return 0;
    if (s->session.level < -1 || s->session.level >= OD_LEVEL_COUNT ||
        s->session.world < 0 || s->session.world >= OD_LEVEL_COUNT / OD_WORLD_ROUNDS ||
        s->session.round < 0 || s->session.round >= OD_WORLD_ROUNDS ||
        s->session.progress.world < 0 || s->session.progress.world >= OD_WORLD_COUNT ||
        s->session.progress.cursor < 0 || s->session.progress.cursor >= OD_MAP_SLOTS)
        return 0;
    for (int i = OD_LEVEL_COUNT; i < (int)sizeof s->session.progress.cleared * 8; i++)
        if (s->session.progress.cleared[i >> 3] & (1u << (i & 7)))
            return 0;
    if (s->view.load_stage > OD_PLAY_LOAD_FULL ||
        s->world_clear_world < -1 || s->world_clear_world >= OD_WORLD_COUNT ||
        s->castle_appears_world < -1 || s->castle_appears_world >= OD_WORLD_COUNT ||
        s->clear_timer < -1 || s->sim_tick < 0)
        return 0;
    return 1;
}

static int scoped_candidate_valid(od_screens *candidate,const od_flow *live,
                                  const od_state_context *ctx)
{
    /* Static v2 is indexed state, never a legacy art program token. Keep this
     * check before owner rebind and before the flow's legacy token decoder. */
    if (od_gfx_static_magicword() && candidate->mw.prog != NULL) return 0;
    if (!ctx || !ctx->screen_owner) {
        /* Isolated legacy/module states have no initialized owner. They may
         * not certify or resume a native PW task; do not fabricate an owner. */
        const od_cinematic_sounds zero={0};
        return !(candidate->scene==OD_SCREEN_MAGICWORD && od_gfx_static_magicword()) &&
            candidate->scene!=OD_SCREEN_PASSWORD && !candidate->password_phase &&
            !candidate->password_record && !candidate->password_brightness &&
            !memcmp(&candidate->password_sounds,&zero,sizeof zero) &&
            candidate->scene!=OD_SCREEN_EDITOR && !candidate->editor_phase &&
            !candidate->editor_record && !candidate->editor_row &&
            !candidate->editor_brightness && candidate->editor_status==OD_OK &&
            !candidate->editor_cinematic_data &&
            !candidate->castle_round10_data && !candidate->castle_round10_phase &&
            candidate->castle_round10_status==OD_OK &&
            !candidate->world_clear_data && !candidate->world_clear_phase &&
            candidate->world_clear_status==OD_OK &&
            !(candidate->scene==OD_SCREEN_WORLD_CLEAR && candidate->world_clear_part<2) &&
            !candidate->goodnight_hidden_data && !candidate->goodnight_hidden_phase &&
            !candidate->goodnight_hidden_kind && candidate->goodnight_hidden_status==OD_OK &&
            candidate->scene!=OD_SCREEN_GOODNIGHT && candidate->scene!=OD_SCREEN_HIDDEN_INTRO &&
            !candidate->boss_ending_data && !candidate->boss_ending_phase &&
            !candidate->boss_ending_kind && candidate->boss_ending_status==OD_OK &&
            !candidate->post_ending_data && !candidate->post_ending_phase &&
            !candidate->post_ending_kind && candidate->post_ending_status==OD_OK &&
            candidate->scene!=OD_SCREEN_BOSS && candidate->scene!=OD_SCREEN_ENDING &&
            !memcmp(&candidate->editor_sounds,&zero,sizeof zero);
    }
    if (ctx->screen_owner!=&live->screens) return 0;
    /* Rebind ONLY from this initialized live epoch. Never dereference a
     * candidate's serialized opaque pointers or publish to live state here. */
    candidate->cinematic_data=live->screens.cinematic_data;
    candidate->bootstrap_data=live->screens.bootstrap_data;
    candidate->editor_cinematic_data=live->screens.editor_cinematic_data;
    candidate->castle_round10_data=live->screens.castle_round10_data;
    candidate->world_clear_data=live->screens.world_clear_data;
    candidate->goodnight_hidden_data=live->screens.goodnight_hidden_data;
    candidate->boss_ending_data=live->screens.boss_ending_data;
    candidate->post_ending_data=live->screens.post_ending_data;
    od_screens_rebind_cinematic(candidate);
    return od_screens_validate_candidate(candidate,ctx->screen_owner)==OD_OK;
}

od_status od_state_save_ex(const od_flow *f, const char *path,
                            const od_state_context *ctx)
{
    if (f == NULL || path == NULL)
        return OD_ERR_RANGE;
    if (f->scene_sound_status != OD_OK) return OD_ERR_FORMAT;
    if (!state_enums_valid(&f->state)) return OD_ERR_FORMAT;
    od_screens screen_candidate=f->screens;
    if (!scoped_candidate_valid(&screen_candidate,f,ctx)) return OD_ERR_FORMAT;
    if ((ctx == NULL && f->audio_device) ||
        (ctx != NULL && (ctx->host_size > OD_STATE_MAX_BYTES ||
                        (ctx->host_size != 0 && ctx->host == NULL))))
        return OD_ERR_RANGE;

    const od_audio *audio = ctx ? ctx->audio : &f->audio_core;
    size_t host_size = ctx ? ctx->host_size : 0;
    size_t engine_size = od_audio_state_size();
    od_flow_resume resume;
    od_flow_resume_describe(f, &resume);
    size_t sec = section_size(&resume);
    size_t payload = sizeof(od_flow_state) + engine_size + host_size + sec + 4;
    size_t total = STATE_HEADER + payload + STATE_IDENTITY + STATE_TRAILER;
    if (payload > OD_STATE_MAX_BYTES || total > OD_STATE_MAX_BYTES)
        return OD_ERR_RANGE;

    uint8_t *buf = calloc(1, total);
    if (buf == NULL)
        return OD_ERR_NOMEM;

    memcpy(buf, OD_STATE_MAGIC, 4);
    put_u16(buf + 4, (uint16_t)OD_STATE_FORMAT);
    put_u64(buf + 6, od_state_build_id());
    put_u32(buf + 14, flow_content(f));
    put_u64(buf + 18, (uint64_t)f->sim_tick);
    put_u64(buf + 26, (uint64_t)time(NULL));
    put_i32(buf + 34, (int32_t)f->level);
    put_i16(buf + 38, (int16_t)(f->session.world + 1));
    put_i16(buf + 40, (int16_t)(f->session.round + 1));
    /* Keep the 42-byte metadata prefix and state payload position stable for
     * peek/pointer-audit tools. Format 2's strong identity is a fixed footer. */
    size_t identity = total - STATE_TRAILER - STATE_IDENTITY;
    if (ctx) {
        memcpy(buf + identity,ctx->build,32);
        memcpy(buf + identity + 32,ctx->content,32);
    }
    put_u32(buf + identity + 64,(uint32_t)host_size);
    put_u32(buf + identity + 68,audio && audio->engine ? 1u : 0u);

    od_flow_state copy = f->state;
    strip_pointers(&copy);
    memcpy(buf + STATE_HEADER, &copy, sizeof copy);
    od_audio_state_save(audio, buf + STATE_HEADER + sizeof copy);
    if (host_size) memcpy(buf + STATE_HEADER + sizeof copy + engine_size,
                          ctx->host,host_size);
    section_write(buf + STATE_HEADER + sizeof copy + engine_size + host_size,
                  &resume, ctx);
    put_u32(buf + identity - 4, (uint32_t)sec);

    put_u32(buf + total - STATE_TRAILER,
            od_state_checksum(buf, total - STATE_TRAILER));

    od_status st = write_atomic(path, buf, total);
    free(buf);
    return st;
}

/* ---- load --------------------------------------------------------------- */

static od_state_reject g_reject;
static od_status state_load_impl(od_flow *f, const char *path,
                            const od_state_context *ctx)
{
    if (f == NULL || path == NULL)
        return OD_ERR_RANGE;
    /* Runtime failure is outside the payload. A load must not implicitly heal
     * an overflow/drain failure or replace data under a latched failed step. */
    if (f->scene_sound_status != OD_OK) return OD_ERR_FORMAT;
    if ((ctx == NULL && f->audio_device) ||
        (ctx != NULL && (ctx->host_size > OD_STATE_MAX_BYTES ||
                        (ctx->host_size != 0 && ctx->host == NULL))))
        return OD_ERR_RANGE;

    uint8_t *buf = NULL;
    size_t size = 0;
    od_status st = read_file(path, &buf, &size);
    if (st != OD_OK)
        return st;

    size_t host_size = ctx ? ctx->host_size : 0;
    od_audio *audio = ctx ? ctx->audio : &f->audio_core;
    if (size < STATE_HEADER + STATE_IDENTITY + STATE_TRAILER ||
        memcmp(buf, OD_STATE_MAGIC, 4) != 0) {
        fprintf(stderr, "state: %s (from an older OpenDana version)\n", path);
        free(buf);
        return OD_ERR_FORMAT;
    }
    const unsigned fmt = rd_u16(buf + 4);
    if (fmt != OD_STATE_FORMAT && fmt != OD_STATE_FORMAT_NO_RESUME) {
        fprintf(stderr, "state: %s (unsupported format %u)\n", path, fmt);
        free(buf);
        return OD_ERR_FORMAT;
    }
    if (rd_u32(buf + size - STATE_TRAILER) !=
        od_state_checksum(buf, size - STATE_TRAILER)) {
        fprintf(stderr, "state: %s (checksum mismatch)\n", path);
        free(buf);
        return OD_ERR_FORMAT;
    }
    /* C6.31 (CORE-8): the file says which audio backend wrote it (the SDL
     * device's pointer-free block size, and whether a sequencer was running).
     * A state is not tied to it: one saved with sound loads with --no-audio
     * or without a device, and the other way round. */
    size_t identity = size - STATE_TRAILER - STATE_IDENTITY;
    uint32_t file_host = rd_u32(buf + identity + 64);
    uint32_t file_engine = rd_u32(buf + identity + 68);
    /* C6.62: the resume section sits right before the identity footer. */
    size_t seclen = 0;
    int have_sec = 0;
    od_flow_resume res;
    const uint8_t *file_rom = NULL, *file_mods = NULL;
    memset(&res, 0, sizeof res);
    if (fmt == OD_STATE_FORMAT) {
        seclen = identity >= STATE_HEADER + 4 ? rd_u32(buf + identity - 4) : 0;
        if (seclen > identity - 4 - STATE_HEADER) {
            fprintf(stderr, "state: %s (bad resume section)\n", path);
            free(buf);
            return OD_ERR_FORMAT;
        }
        have_sec = section_parse(buf + identity - 4 - seclen, seclen, &res,
                                 &file_rom, &file_mods);
    }
    size_t expected = file_host > OD_STATE_MAX_BYTES ? 0 :
                      STATE_HEADER + sizeof(od_flow_state) + od_audio_state_size() +
                      (size_t)file_host + (fmt == OD_STATE_FORMAT ? seclen + 4 : 0) +
                      STATE_IDENTITY + STATE_TRAILER;
    const int size_ok = file_engine <= 1 && size == expected;
    const int build_ok = rd_u64(buf + 6) == od_state_build_id();
    /* C6.43: the 32-byte build field is kept for file compatibility but never
     * compared: a state saved by one OpenDana version loads in another. Only
     * the format and the imported content identity decide. */
    const uint8_t zero[32] = {0};
    const int content_ok = rd_u32(buf + 14) == flow_content(f) &&
        memcmp(buf + identity + 32, ctx ? ctx->content : zero, 32) == 0;
    const int rom_mods_ok = have_sec && ctx && nonzero32(ctx->rom) &&
        memcmp(file_rom, ctx->rom, 32) == 0 && memcmp(file_mods, ctx->mods, 32) == 0;
    const int layout_ok = size_ok && build_ok;
    /* Three tiers: the raw dump when layout and content both fit; the level
     * start plus the logged inputs when the package comes from the same ROM and
     * mod set (or has the very same bytes) but the layout or content changed;
     * otherwise a refusal. */
    const int replay = !(layout_ok && content_ok) && have_sec && f->assets != NULL &&
        res.kind != OD_RESUME_NONE && (content_ok || rom_mods_ok);
    if (!replay && !layout_ok && !(have_sec && !content_ok)) {
        if (!size_ok)
            fprintf(stderr, "state: %s (size %zu, expected %zu)\n", path, size,
                    expected);
        else
            fprintf(stderr, "state: %s (state of another game version)\n", path);
        free(buf);
        return OD_ERR_FORMAT;
    }
    if (!replay && !content_ok) {
        fprintf(stderr, "state: %s (state of another content package)\n", path);
        g_reject = OD_STATE_REJECT_CONTENT;
        free(buf);
        return OD_ERR_FORMAT;
    }
    if (replay) {
        if (!resume_ranges_valid(&res, f) || res.tick != (long)rd_u64(buf + 18)) {
            fprintf(stderr, "state: %s (resume values out of range)\n", path);
            free(buf);
            return OD_ERR_FORMAT;
        }
        const size_t inputs = res.kind == OD_RESUME_LEVEL ? res.count : 0;
        od_audio *live = ctx ? ctx->audio : &f->audio_core;
        od_status rst = od_flow_resume_apply(f, &res);
        free(buf);
        if (rst != OD_OK) {
            fprintf(stderr, "state: %s (could not replay the level)\n", path);
            return OD_ERR_FORMAT;
        }
        if (live && live->engine)
            od_audio_reset_init(live);
        od_flow_timeline_break(f, "state loaded");
        fprintf(stderr, "state: %s (restored by replaying %zu inputs)\n", path, inputs);
        return OD_OK;
    }
    const int live_engine = audio && audio->engine;
    const int same_backend = file_host == host_size && (int)file_engine == live_engine;

    od_flow_state tmp;
    memcpy(&tmp, buf + STATE_HEADER, sizeof tmp);
    /* All eight opaque owners have exactly one wire representation: NULL.
     * Reject before enum/task/candidate validation or any owner lookup. */
    if (tmp.screens.cinematic_data != NULL || tmp.screens.bootstrap_data != NULL ||
        tmp.screens.editor_cinematic_data != NULL || tmp.screens.castle_round10_data != NULL ||
        tmp.screens.world_clear_data != NULL || tmp.screens.goodnight_hidden_data != NULL ||
        tmp.screens.boss_ending_data != NULL || tmp.screens.post_ending_data != NULL) {
        fprintf(stderr,"state: %s (serialized native owners must be null)\n",path);
        free(buf); return OD_ERR_FORMAT;
    }
    /* Legacy wire programs have only kinds 1/2. Static v2 accepts neither:
     * unchanged POD size is not authority to interpret a v1 token as v2. */
    if (tmp.screens.mw.prog &&
        (od_gfx_static_magicword() ||
         ((uintptr_t)tmp.screens.mw.prog != 1 && (uintptr_t)tmp.screens.mw.prog != 2))) {
        free(buf); return OD_ERR_FORMAT;
    }
    if (!state_enums_valid(&tmp)) {
        fprintf(stderr,"state: %s (invalid native task state)\n",path);
        free(buf); return OD_ERR_FORMAT;
    }
    if (!state_ranges_valid(&tmp, f)) {
        fprintf(stderr,"state: %s (values out of range)\n",path);
        free(buf); return OD_ERR_FORMAT;
    }
    if (!scoped_candidate_valid(&tmp.screens,f,ctx)) {
        fprintf(stderr,"state: %s (invalid/unowned native screen candidate)\n",path);
        free(buf); return OD_ERR_FORMAT;
    }
    /* The sequencer resumes from the file only when both sides have one; a
     * backend's own block only when it is the same backend. A file without a
     * sequencer silences the live one (its song belongs to another moment);
     * the next scene cue starts the music again. */
    if (file_engine && live_engine)
        od_audio_state_load(audio, buf + STATE_HEADER + sizeof tmp);
    else if (live_engine)
        od_audio_reset_init(audio);
    if (host_size && same_backend)
        memcpy(ctx->host,buf + STATE_HEADER + sizeof tmp +
               od_audio_state_size(),host_size);
    if (!same_backend)
        fprintf(stderr,"state: %s (saved with another audio backend; %s)\n",path,
                file_engine && live_engine ? "sequencer restored" : "sound restarts at the next cue");
    /* The audio backend is this process's configuration, not game state: the
     * core engine's handle and counters come from the file only when both
     * this flow and the file ran the core engine. */
    const int core_live = f->audio_core_on && audio == &f->audio_core;
    if (!(core_live && tmp.audio_core_on && file_host == 0 && file_engine))
        tmp.audio_core = f->audio_core;
    tmp.audio_core_on = f->audio_core_on;

    /* Keep the live transient pointers that the payload zeroed. */
    od_scene_change *changes[OD_SCREEN_TRACKS];
    for (int i = 0; i < OD_SCREEN_TRACKS; i++)
        changes[i] = f->screens.tracks[i].changes;
    od_cinematic_data *cinematic_data = f->screens.cinematic_data;
    od_cinematic_data *bootstrap_data = f->screens.bootstrap_data;
    od_cinematic_data *editor_cinematic_data = f->screens.editor_cinematic_data;
    od_cinematic_data *castle_round10_data = f->screens.castle_round10_data;
    od_cinematic_data *world_clear_data = f->screens.world_clear_data;
    od_cinematic_data *goodnight_hidden_data = f->screens.goodnight_hidden_data;
    od_cinematic_data *boss_ending_data = f->screens.boss_ending_data;
    od_cinematic_data *post_ending_data = f->screens.post_ending_data;
    void (*tick)(void *) = f->audio.tick;
    void (*request)(void *, unsigned) = f->audio.request;
    void (*play_music)(void *, int) = f->audio.play_music;
    void (*reset_init)(void *) = f->audio.reset_init;
    void *audio_ctx = f->audio.ctx;
    od_audio_engine *engine = f->audio_core.engine;

    f->state = tmp;

    for (int i = 0; i < OD_SCREEN_TRACKS; i++)
        f->screens.tracks[i].changes = changes[i];
    f->screens.cinematic_data = cinematic_data;
    f->screens.bootstrap_data = bootstrap_data;
    f->screens.editor_cinematic_data = editor_cinematic_data;
    f->screens.castle_round10_data = castle_round10_data;
    f->screens.world_clear_data = world_clear_data;
    f->screens.goodnight_hidden_data = goodnight_hidden_data;
    f->screens.boss_ending_data = boss_ending_data;
    f->screens.post_ending_data = post_ending_data;
    od_screens_rebind_cinematic(&f->screens);
    f->audio.tick = tick;
    f->audio.request = request;
    f->audio.play_music = play_music;
    f->audio.reset_init = reset_init;
    f->audio.ctx = audio_ctx;
    f->audio_core.engine = engine;

    od_flow_state_rebind(f);
    /* C6.62: keep the input log of the restored moment, so the next save is
     * resumable too (none when the file carries no usable log). */
    od_flow_resume_adopt(f, have_sec ? &res : NULL);
    free(buf);
    /* C6.31 (CORE-14): a --record script cannot jump to another timeline. */
    od_flow_timeline_break(f, "state loaded");
    return OD_OK;
}

/* C6.31 (CORE-16): the public API acts on an initialised flow. The context
 * names that flow's own screens (the owner the native cinematic programs and
 * the password/editor tasks are validated against) and its core sequencer,
 * with the module identity (no executable or package digest). With the NULL
 * context every initialised flow was refused (OD_ERR_FORMAT). A flow driving
 * the SDL device keeps its sequencer in the host: od_audio_sdl_state_*. */
static od_state_context module_context(const od_flow *f)
{
    od_state_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.audio = (od_audio *)&f->audio_core;
    ctx.screen_owner = &f->screens;
    return ctx;
}

od_status od_state_save(const od_flow *f, const char *path)
{
    if (f == NULL || path == NULL)
        return OD_ERR_RANGE;
    if (f->audio_device)
        return OD_ERR_RANGE;
    od_state_context ctx = module_context(f);
    return od_state_save_ex(f, path, &ctx);
}

od_state_reject od_state_last_reject(void)
{
    return g_reject;
}

od_status od_state_load_ex(od_flow *f, const char *path,
                            const od_state_context *ctx)
{
    g_reject = OD_STATE_REJECT_NONE;
    od_status st = state_load_impl(f, path, ctx);
    if (st == OD_ERR_FORMAT && g_reject == OD_STATE_REJECT_NONE)
        g_reject = OD_STATE_REJECT_FORMAT;
    return st;
}

od_status od_state_load(od_flow *f, const char *path)
{
    if (f == NULL || path == NULL)
        return OD_ERR_RANGE;
    if (f->audio_device)
        return OD_ERR_RANGE;
    od_state_context ctx = module_context(f);
    return od_state_load_ex(f, path, &ctx);
}

/* ---- peek --------------------------------------------------------------- */

od_status od_state_peek(const char *path, od_state_info *out)
{
    if (path == NULL || out == NULL)
        return OD_ERR_RANGE;
    memset(out, 0, sizeof *out);

    uint8_t *buf = NULL;
    size_t size = 0;
    od_status st = read_file(path, &buf, &size);
    if (st != OD_OK)
        return st;
    if (size < STATE_HEADER + STATE_TRAILER ||
        memcmp(buf, OD_STATE_MAGIC, 4) != 0 ||
        rd_u32(buf + size - STATE_TRAILER) !=
            od_state_checksum(buf, size - STATE_TRAILER)) {
        free(buf);
        return OD_ERR_FORMAT;
    }
    out->valid = 1;
    out->format = rd_u16(buf + 4);
    out->build_id = rd_u64(buf + 6);
    out->content = rd_u32(buf + 14);
    out->tick = (long)rd_u64(buf + 18);
    out->date = rd_u64(buf + 26);
    out->level = (int)rd_i32(buf + 34);
    out->world = (int)rd_i16(buf + 38);
    out->round = (int)rd_i16(buf + 40);
    free(buf);
    return OD_OK;
}

void od_state_default_path(char *out, size_t cap, int slot)
{
    if (out == NULL || cap == 0)
        return;
    /* C6.31 (PUB-12/UI-19): states/ in the shared per-user data directory. */
    char rel[64];
    snprintf(rel, sizeof rel, "states/slot-%d.odstate", slot);
    if (od_userdir_path(OD_USERDIR_DATA, rel, out, cap) == OD_OK)
        return;
    snprintf(out, cap, "slot-%d.odstate", slot);
}
