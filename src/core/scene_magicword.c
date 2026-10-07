/* scene_magicword.c - "Today's magicword", the `quit` of "What next?" (C3.3).
 *
 * The mode's main loop over the package data of screens/magicword.json. The
 * VRAM records, the magicword glyphs, the cursor and the colour cycles are the
 * original's; the timing comes from the VRAM queue rule and the mode's measured
 * init. */
#include "opendana/scene_magicword.h"

#include "opendana/game.h"
#include "opendana/password.h"
#include "opendana/content_gfx.h"

#include <string.h>

/* OD_SCENE_WORDS display vector (screens.c compose_display). */
#define MW_WORDS        2382u
#define MW_NT           32u     /* nametable 0 */
#define MW_BG_BANKS     2080u
#define MW_SPRITE_BANKS 2088u
#define MW_MASK         2092u
#define MW_BANDS        2093u
#define MW_SPRITE_COUNT 2124u
#define MW_SPRITES      2125u

/* The mode's init spends three frames before the palette sets are uploaded,
 * and the loop's first call runs on the fourth (palette_age 3 / program_start 4). */
#define MW_INIT_FRAMES  3u

static void set_palette(uint16_t *ppu, const uint8_t pal[32])
{
    for (unsigned i = 0; i < 32; ++i)
        ppu[i] = pal[i];
}

/* One record through the display pass's VRAM writer: the low six bits of `count` give the
 * byte count from `addr`, +1 or (bit 7) +32 per byte. Only nametable 0 is drawn here. */
static void apply_record(uint16_t *ppu, uint8_t count, uint16_t addr, const uint8_t *data)
{
    unsigned step = (count & 0x80) ? 32u : 1u;
    for (unsigned i = 0; i < (count & 0x3Fu); ++i, addr = (uint16_t)(addr + step))
        if (addr >= 0x2000 && addr < 0x2400)
            ppu[MW_NT + addr - 0x2000] = data[i];
}

void od_scene_magicword_begin(od_scene_magicword *m, const od_magicword_art *art)
{
    memset(m, 0, sizeof *m);
    if (!art)
        return;
    /* The mode's init: nametable cleared, CHR banks and mask; the PPU palette is
     * still the faded one (all black) until the palette sets are uploaded
     * (`palette_age`); nothing is drawn until the loop's first call. */
    memcpy(m->palette, art->palette, sizeof m->palette);
    for (unsigned i = 0; i < 32; ++i)
        m->ppu[i] = 0x0F;
    for (unsigned i = 0; i < 4; ++i)
        m->ppu[MW_BG_BANKS + i] = m->ppu[MW_BG_BANKS + 4 + i] =
            (uint16_t)(art->bg_banks[i] * 64u);
    for (unsigned i = 0; i < 2; ++i)
        m->ppu[MW_SPRITE_BANKS + i] = m->ppu[MW_SPRITE_BANKS + 2 + i] =
            (uint16_t)((art->sprite_banks[i] & 0xFEu) * 64u);
    m->ppu[MW_MASK] = art->mask;
    m->ppu[MW_BANDS] = 1; /* one band at row 0, no scroll, nametable 0 */
    m->prog = art->screen;
    m->prog_steps = art->screen_steps;
}

/* The progress's magicword with key `key`. The text is the package codec's
 * (password.c), so it decodes back to the same progress; each symbol is drawn
 * with the glyph of its 5-bit display code. */
static void build_word(od_scene_magicword *m, const od_magicword_art *art,
                       const uint16_t words[10], uint8_t key, uint8_t tiles[64])
{
    char text[OD_PASSWORD_FIELD + 1];
    uint16_t w[OD_PASSWORD_WORDS];
    for (unsigned i = 0; i < OD_PASSWORD_WORDS; ++i)
        w[i] = (uint16_t)(words[i] & 0x3FFu); /* packs the low ten bits */
    od_password_encode(w, key, text);
    m->key = key;
    unsigned n = 0;
    for (unsigned i = 0; i < OD_PASSWORD_FIELD; ++i) {
        int code = od_password_code_of(text[i]);
        m->field[i] = (uint8_t)(code < 0 ? 0 : code);
        tiles[n++] = art->glyphs[m->field[i] & 31u];
        if (i + 1 < OD_PASSWORD_FIELD && (i + 1) % art->word_group == 0)
            tiles[n++] = art->word_gap;
    }
    m->word_done = 1;
}

/* Run the VRAM program for one CPU frame through the queue. Returns the bytes
 * queued; m->prog becomes NULL when the program has finished. */
static unsigned run_program(od_scene_magicword *m, const od_magicword_art *art,
                            uint16_t frame_tick, const uint16_t words[10])
{
    unsigned queued = 0;
    while (m->prog && m->step < m->prog_steps) {
        const od_mw_step *st = &m->prog[m->step];
        if (st->op == OD_MW_OP_YIELD) {
            ++m->step; /* the rest runs on the next frame */
            m->rec = 0;
            return queued;
        }
        if (st->op == OD_MW_OP_WORD) {
            uint8_t tiles[64];
            if (art->word_count + queued >= art->queue_capacity)
                return queued;
            build_word(m, art, words, (uint8_t)(frame_tick & 0x1Fu), tiles);
            apply_record(m->ppu, art->word_count, art->word_addr, tiles);
            queued += 3u + art->word_count;
            ++m->step;
            continue;
        }
        while (m->rec < st->count) {
            const od_mw_record *r = &art->records[st->first + m->rec];
            if ((r->count & 0x3Fu) + queued >= art->queue_capacity)
                return queued; /* wait for the next frame */
            apply_record(m->ppu, r->count, r->addr, r->data);
            queued += 3u + (r->count & 0x3Fu);
            ++m->rec;
        }
        ++m->step;
        m->rec = 0;
    }
    m->prog = NULL;
    return queued;
}

/* The colour-cycle step, on a frame the loop reaches it. Returns 1 when it
 * uploads the palette, which itself waits when the queue cannot take 32 bytes. */
static int colour_cycle(const od_magicword_art *art, uint16_t frame_tick, uint16_t *ppu,
                        uint8_t *cycle3, uint8_t *cycle5, uint8_t *palette)
{
    if (!(frame_tick & art->bg_mask)) {
        for (unsigned k = 0; k < 3; ++k)
            palette[art->bg_entries[k]] = art->bg_tables[k][*cycle3 % 3u];
        *cycle3 = (uint8_t)((*cycle3 + 1u) % 3u);
    }
    if (frame_tick & art->sprite_mask)
        return 0;
    palette[art->sprite_entry] = art->sprite_table[*cycle5 % 5u];
    *cycle5 = (uint8_t)((*cycle5 + 1u) % 5u);
    (void)ppu;
    return 1;
}

od_magicword_event od_scene_magicword_tick(od_scene_magicword *m,
                                             const od_magicword_art *art,
                                             unsigned buttons, uint16_t frame_tick,
                                             const uint16_t words[10],
                                             uint8_t *cycle3, uint8_t *cycle5,
                                             uint16_t *display)
{
    /* The display pass shows what the previous CPU frame queued. */
    memcpy(display, m->ppu, sizeof m->ppu);
    unsigned pressed = buttons & ~m->prev_buttons;
    m->prev_buttons = buttons;
    od_magicword_event ev = OD_MAGICWORD_NONE;
    if (!art || !art->loaded) {
        ++m->age;
        return ev;
    }
    /* The palette buffer as the CPU sees it; ppu[0..31] is the copy the last
     * upload left in the PPU. */
    uint8_t *palette = m->palette;
    if (m->blocked) {
        /* The queue waited inside the palette upload: it goes out now and the loop
         * resumes on the next frame. */
        m->blocked = 0;
        set_palette(m->ppu, palette);
        ++m->age;
        return ev;
    }
    unsigned queued = 0;
    int handler_done = 0;
    if (m->age == art->palette_age)
        set_palette(m->ppu, palette); /* palette upload, shown next frame */
    if (m->age >= art->program_start) {
        switch (m->state) {
        case 0:
            queued = run_program(m, art, frame_tick, words);
            if (!m->prog) {
                m->state = 1; /* cursor state = 1 */
                handler_done = 1;
            }
            break;
        case 1:
            if (pressed & OD_BTN_A) { /* START/A edge */
                m->state = 2;
                m->prog = art->question;
                m->prog_steps = art->question_steps;
                m->step = m->rec = 0;
                queued = run_program(m, art, frame_tick, words);
            }
            handler_done = !m->prog;
            break;
        default:
            if (m->prog) {
                queued = run_program(m, art, frame_tick, words);
                handler_done = !m->prog;
                break;
            }
            handler_done = 1;
            m->ppu[MW_SPRITE_COUNT] = 0;
            if (pressed & OD_BTN_A) { /* confirm */
                if (m->option == 0) {
                    ev = OD_MAGICWORD_YES; /* state 0, back to the menu */
                } else {
                    ev = OD_MAGICWORD_NO;  /* state 3, quit */
                    m->state = 3;
                }
                break;
            }
            /* With No chosen only LEFT is read, else only RIGHT. */
            if (m->option == 0 && (pressed & OD_BTN_RIGHT))
                m->option = 1;
            else if (m->option == 1 && (pressed & OD_BTN_LEFT))
                m->option = 0;
            /* The cursor goes through the sprite writer (shown next frame). */
            m->ppu[MW_SPRITE_COUNT] = 1;
            m->ppu[MW_SPRITES + 0] = art->cursor_x[m->option & 1u];
            m->ppu[MW_SPRITES + 1] = (uint16_t)(art->cursor_y + 1u);
            m->ppu[MW_SPRITES + 2] = art->cursor_tile;
            m->ppu[MW_SPRITES + 3] = art->cursor_attr;
            break;
        }
    }
    /* The colour cycle runs once the main step has returned on this frame. */
    if (handler_done && ev == OD_MAGICWORD_NONE &&
        colour_cycle(art, frame_tick, m->ppu, cycle3, cycle5, palette)) {
        if (32u + queued >= art->queue_capacity)
            m->blocked = 1;
        else
            set_palette(m->ppu, palette);
    }
    ++m->age;
    return ev;
}

void od_scene_magicword_text(const od_scene_magicword *m, char out[25])
{
    out[0] = 0;
    if (!m->word_done)
        return;
    for (unsigned i = 0; i < OD_PASSWORD_FIELD; ++i)
        out[i] = od_password_char_of(m->field[i]);
    out[OD_PASSWORD_FIELD] = 0;
}

void od_magicword_night_cycle(const od_magicword_art *art, uint16_t frame_tick,
                               uint8_t *cycle4, uint16_t *display)
{
    if (!art || !art->loaded || (frame_tick & art->night_mask))
        return;
    display[art->night_entry] = art->night_table[*cycle4 % 4u];
    *cycle4 = (uint8_t)((*cycle4 + 1u) % 4u);
}

od_status od_scene_magicword_begin_static(od_scene_magicword *m,const uint16_t live[2382])
{
    const od_static_magicword *a=od_gfx_static_magicword();
    if(!m||!live||!a)return OD_ERR_FORMAT;
    od_scene_magicword next={0};memcpy(next.ppu,live,sizeof next.ppu);
    /* EFD4/EFE5: normal clear then semantic CHR and palette publication.
     * No program_start/palette_age/PPUmask/duration lifted from old art. */
    memset(next.ppu+32,0,2048*sizeof *next.ppu);
    /* The six CHR bank registers: the first two are the sprites and the last four
     * the background (the importer, the v1 renderer and the reference agree).
     * Leave the attribute tables at 0 like the mode's `memset` init: the box then
     * uses palette group 0. */
    next.ppu[2124]=0;next.ppu[2093]=1;
    memset(next.ppu+2094,0,30*sizeof *next.ppu);
    memcpy(next.palette,a->palette_buffer,32);
    for(unsigned i=0;i<32;++i)next.ppu[i]=15;
    for(unsigned i=0;i<4;++i)next.ppu[2080+i]=next.ppu[2084+i]=64u*a->chr[2+i];
    for(unsigned i=0;i<2;++i)next.ppu[2088+i]=next.ppu[2090+i]=64u*(a->chr[i]&254u);
    next.prog_steps=5;next.pending_palette=3; /*bit0 initpalette job,bit1 songrequest*/
    *m=next;return OD_OK;
}
od_status od_scene_magicword_take_static_sound(od_scene_magicword *m,unsigned *id,int *present)
{
    const od_static_magicword *a=od_gfx_static_magicword();
    if(!m||!a||!id||!present||od_scene_magicword_validate_static(m))return OD_ERR_FORMAT;
    *present=(m->pending_palette&2)!=0;*id=a->song_request;
    m->pending_palette&=~2;return OD_OK;
}
od_status od_scene_magicword_validate_static(const od_scene_magicword *m)
{
    const od_static_magicword *a=od_gfx_static_magicword();
    if(!m||!a||m->prog||m->state>3||m->option>1||m->word_done>1||m->key>31||
       m->pending_palette<0||m->pending_palette>3||m->blocked<0||m->blocked>1)return OD_ERR_FORMAT;
    unsigned n=m->state<2?5:1;
    if(m->prog_steps!=n||m->step>n||((m->pending_palette&1)&&(m->state||m->step||m->rec)))return OD_ERR_FORMAT;
    const od_static_mw_command *p=m->state<2?a->screen:a->question;
    if(m->step==n) {if(m->rec)return OD_ERR_FORMAT;}
    else if(p[m->step].op==OD_STATIC_MW_RECORDS) {if(m->rec>p[m->step].record_count)return OD_ERR_FORMAT;}
    else if(m->rec)return OD_ERR_FORMAT;
    for(unsigned i=0;i<24;++i)if(m->field[i]>31)return OD_ERR_FORMAT;
    for(unsigned i=0;i<32;++i)if(m->palette[i]>63||m->ppu[i]>63)return OD_ERR_FORMAT;
    for(unsigned i=32;i<2080;++i)if(m->ppu[i]>255)return OD_ERR_FORMAT;
    if(m->ppu[2124]>64||m->ppu[2093]>5)return OD_ERR_FORMAT;
    for(unsigned i=2080;i<2092;++i)if(m->ppu[i]&63u)return OD_ERR_FORMAT;
    return OD_OK;
}
static unsigned run_static_program(od_scene_magicword *m,const od_static_magicword *a,
                                   uint16_t frame_tick,const uint16_t words[10])
{
    unsigned queued=0;
    const od_static_mw_command *program=m->state==0?a->screen:a->question;
    while(m->step<m->prog_steps) {
        const od_static_mw_command *c=&program[m->step];
        if(c->op==OD_STATIC_MW_YIELD){++m->step;m->rec=0;return queued;}
        if(c->op==OD_STATIC_MW_WORD) {
            if(a->word.count+queued>=a->queue_capacity_bytes)return queued;
            char text[25];uint16_t w[10];uint8_t tiles[64];unsigned n=0;
            for(unsigned i=0;i<10;++i)w[i]=words[i]&0x3ffu;
            od_password_encode(w,(uint8_t)(frame_tick&31u),text);m->key=(uint8_t)(frame_tick&31u);
            for(unsigned i=0;i<24;++i) {
                int code=od_password_code_of(text[i]);m->field[i]=(uint8_t)(code<0?0:code);
                tiles[n++]=a->word.glyphs[m->field[i]&31u];
                if(i+1<24&&(i+1)%a->word.group==0)tiles[n++]=a->word.gap_tile;
            }
            apply_record(m->ppu,a->word.count,a->word.address,tiles);m->word_done=1;
            queued+=3u+a->word.count;++m->step;continue;
        }
        while(m->rec<c->record_count) {
            const od_static_mw_record *r=&c->records[m->rec];unsigned n=r->count&63u;
            if(n+queued>=a->queue_capacity_bytes)return queued;
            apply_record(m->ppu,r->count,r->address,r->data);queued+=3+n;++m->rec;
        }
        ++m->step;m->rec=0;
    }
    return queued;
}
od_magicword_event od_scene_magicword_tick_static(od_scene_magicword *m,
    unsigned buttons,uint16_t frame_tick,const uint16_t words[10],uint8_t *cycle3,
    uint8_t *cycle5,uint16_t display[2382])
{
    const od_static_magicword *a=od_gfx_static_magicword();
    if(!m||!a||!words||!cycle3||!cycle5||!display||od_scene_magicword_validate_static(m))return OD_MAGICWORD_NONE;
    memcpy(display,m->ppu,sizeof m->ppu);unsigned pressed=buttons&~m->prev_buttons;m->prev_buttons=buttons;
    /* The mode init holds the palette and the loop for MW_INIT_FRAMES: starting
     * the loop at entry would build the word on a frame whose animation counter
     * differs and encode different glyphs (persistent rows 97-103). */
    if(m->age<MW_INIT_FRAMES){++m->age;return OD_MAGICWORD_NONE;}
    if(m->pending_palette&1){set_palette(m->ppu,m->palette);m->pending_palette&=~1;++m->age;return OD_MAGICWORD_NONE;}
    if(m->blocked){m->blocked=0;set_palette(m->ppu,m->palette);++m->age;return OD_MAGICWORD_NONE;}
    unsigned queued=0;int done=0;od_magicword_event ev=OD_MAGICWORD_NONE;
    if(m->state==0) {
        queued=run_static_program(m,a,frame_tick,words);if(m->step==m->prog_steps){m->state=1;done=1;}
    } else if(m->state==1) {
        if(pressed&OD_BTN_A){m->state=2;m->prog_steps=1;m->step=m->rec=0;queued=run_static_program(m,a,frame_tick,words);}
        done=m->step==m->prog_steps;
    } else if(m->step<m->prog_steps) {
        queued=run_static_program(m,a,frame_tick,words);done=m->step==m->prog_steps;
    } else {
        done=1;m->ppu[2124]=0;
        if(pressed&OD_BTN_A){ev=m->option?OD_MAGICWORD_NO:OD_MAGICWORD_YES;if(m->option)m->state=3;}
        else {
            if(!m->option&&(pressed&OD_BTN_RIGHT))m->option=1;
            else if(m->option&&(pressed&OD_BTN_LEFT))m->option=0;
            m->ppu[2124]=1;m->ppu[2125]=a->cursor.x[m->option&1u];m->ppu[2126]=a->cursor.y+1u;
            m->ppu[2127]=a->cursor.tile;m->ppu[2128]=a->cursor.attr;
        }
    }
    if(done&&ev==OD_MAGICWORD_NONE) {
        if(!(frame_tick&a->cycle.bg_mask)) {
            for(unsigned k=0;k<3;++k)m->palette[a->cycle.bg_entries[k]]=a->cycle.bg_tables[k][*cycle3%3u];
            *cycle3=(uint8_t)((*cycle3+1u)%3u);
        }
        if(!(frame_tick&a->cycle.sprite_mask)) {
            m->palette[a->cycle.sprite_entry]=a->cycle.sprite_table[*cycle5%5u];*cycle5=(uint8_t)((*cycle5+1u)%5u);
            if(queued+32>=a->queue_capacity_bytes)m->blocked=1;else set_palette(m->ppu,m->palette);
        }
    }
    ++m->age;return ev;
}
void od_magicword_night_cycle_static(uint16_t frame_tick,uint8_t *cycle4,uint16_t display[2382])
{
    const od_static_magicword *a=od_gfx_static_magicword();
    if(!a||!cycle4||!display||(frame_tick&a->post_goodnight.cycle.mask))return;
    display[a->post_goodnight.cycle.entry]=a->post_goodnight.cycle.colours[*cycle4%4u];
    *cycle4=(uint8_t)((*cycle4+1u)%4u);
}
