/* Scene assets are extracted tile layouts, not captured pixel buffers. */
#include "opendana/screens.h"
#include "opendana/content_gfx.h"
#include "opendana/scene_editor.h"
#include "opendana/editor.h"
#include "opendana/interactive_setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

/* Bounded textual resources; no FILE stream or terminator required. */
typedef struct { const char *text; size_t size, at; } scene_text;

static int text_token(scene_text *s,char *out,size_t cap)
{
    while(s->at<s->size && isspace((unsigned char)s->text[s->at])) ++s->at;
    size_t begin=s->at;
    while(s->at<s->size && !isspace((unsigned char)s->text[s->at])) ++s->at;
    size_t n=s->at-begin;
    if(!n || n>=cap || memchr(s->text+begin,0,n)) return 0;
    memcpy(out,s->text+begin,n); out[n]=0;
    return 1;
}

static int text_number(scene_text *s,unsigned base,unsigned maximum,unsigned *out)
{
    char token[32]; unsigned value=0;
    if(!text_token(s,token,sizeof token)) return 0;
    for(size_t i=0;token[i];++i) {
        unsigned char c=(unsigned char)token[i];
        unsigned digit=c>='0' && c<='9'?(unsigned)(c-'0'):
            c>='a' && c<='f'?(unsigned)(c-'a'+10):
            c>='A' && c<='F'?(unsigned)(c-'A'+10):UINT_MAX;
        if(digit>=base || digit>maximum || value>(maximum-digit)/base) return 0;
        value=value*base+digit;
    }
    *out=value; return 1;
}

static int text_end(scene_text *s)
{
    while(s->at<s->size && isspace((unsigned char)s->text[s->at])) ++s->at;
    return s->at==s->size;
}

static od_status parse_track_text(od_scene_track *out,const char *text,size_t size)
{
    if(!out || !text || !size) return OD_ERR_FORMAT;
    scene_text input={text,size,0}; char magic[32],loop_token[32];
    unsigned duration,count,loop_value; int loop;
    if(!text_token(&input,magic,sizeof magic) || strcmp(magic,"ODTRACK1") ||
       !text_number(&input,10,65535,&duration) || !duration ||
       !text_token(&input,loop_token,sizeof loop_token)) return OD_ERR_FORMAT;
    if(!strcmp(loop_token,"-1")) loop=-1;
    else {
        scene_text token={loop_token,strlen(loop_token),0};
        if(!text_number(&token,10,duration-1,&loop_value)) return OD_ERR_FORMAT;
        loop=(int)loop_value;
    }
    if(!text_number(&input,10,4000000,&count)) return OD_ERR_FORMAT;
    od_scene_change *changes=calloc(count?count:1,sizeof *changes);
    if(!changes) return OD_ERR_IO;
    unsigned old_tick=0;
    for(unsigned i=0;i<count;++i) {
        unsigned tick,field,value;
        if(!text_number(&input,16,duration-1,&tick) || tick<old_tick ||
           !text_number(&input,16,OD_SCENE_WORDS-1,&field) ||
           !text_number(&input,16,8191,&value)) { free(changes); return OD_ERR_FORMAT; }
        changes[i]=(od_scene_change){(uint16_t)tick,(uint16_t)field,(uint16_t)value};
        old_tick=tick;
    }
    if(!text_end(&input)) { free(changes); return OD_ERR_FORMAT; }
    *out=(od_scene_track){duration,count,loop,changes}; return OD_OK;
}

static od_status parse_fade_text(od_screens *s,const char *text,size_t size)
{
    if(!s || !text || !size) return OD_ERR_FORMAT;
    scene_text input={text,size,0}; char magic[32]; unsigned count;
    uint8_t fade[16][32]={{0}};
    if(!text_token(&input,magic,sizeof magic) || strcmp(magic,"ODFADE1") ||
       !text_number(&input,10,16,&count) || !count) return OD_ERR_FORMAT;
    for(unsigned i=0;i<count;++i) for(unsigned k=0;k<32;++k) {
        unsigned value;
        if(!text_number(&input,16,63,&value)) return OD_ERR_FORMAT;
        fade[i][k]=(uint8_t)value;
    }
    if(!text_end(&input)) return OD_ERR_FORMAT;
    memcpy(s->level_fade,fade,sizeof fade); s->level_fade_frames=count;
    return OD_OK;
}

/* Borrow only already-published startup resources. Never load another package
 * during scene drawing: initialization fails closed on missing resources. */
static const uint8_t *boss_arena(void)
{
    return od_gfx_boss_arena();
}

static const od_screen_layouts *layouts(void)
{
    return od_gfx_layouts();
}

_Static_assert(OD_LAYOUT_WORDS == OD_SCENE_WORDS,
               "screens/layouts.json displays are scene displays");

static void apply(od_screens *s);
static void compose_display(const uint16_t *d, od_screen_id scene, unsigned age,
                            unsigned visible_selection, od_index_frame *fb,
                            const uint8_t *chr);
/* C6.18: the map's own palette and PPU setup from screens/map.json. */
static void map_apply_static(const od_map_art *art, uint16_t *display);

static od_status parse_scene_text(od_presentation *out,const char *text,size_t size)
{
    if(!out || !text || !size) return OD_ERR_FORMAT;
    scene_text input={text,size,0};
    char magic[32];
    od_presentation p;
    memset(&p, 0, sizeof p);
    unsigned v;
    if (!text_token(&input,magic,sizeof magic) || strcmp(magic, "ODSCENE1")) goto bad;
    for (int i = 0; i < 32; ++i) {
        if (!text_number(&input,16,63,&v)) goto bad;
        p.palette.ram[i] = (uint8_t)v;
    }
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 32; ++x) {
            if (!text_number(&input,16,8191,&v)) goto bad;
            p.tiles[y][x] = (uint16_t)v;
        }
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 32; ++x) {
            if (!text_number(&input,16,3,&v)) goto bad;
            p.palettes[y][x] = (uint8_t)v;
        }
    if (!text_number(&input,16,64,&p.sprite_count)) goto bad;
    for (unsigned i = 0; i < p.sprite_count; ++i) {
        od_sprite *s = &p.sprites[i];
        if (!text_number(&input,16,255,&v)) goto bad;
        s->x = (int)v;
        if (!text_number(&input,16,256,&v)) goto bad;
        s->y = (int)v;
        if (!text_number(&input,16,8191,&v)) goto bad;
        s->tile = (uint16_t)v;
        if (!text_number(&input,16,255,&v)) goto bad;
        s->attr = (uint8_t)v;
    }
    if (!text_end(&input)) goto bad;
    *out = p;
    return OD_OK;
bad:
    return OD_ERR_FORMAT;
}

od_status od_screen_load(od_presentation *out,const char *path)
{
    if(!out || !path) return OD_ERR_FORMAT;
    char *bytes=NULL; size_t size=0;
    od_status status=od_gfx_content_read(path,&bytes,&size);
    if(status==OD_OK) status=parse_scene_text(out,bytes,size);
    free(bytes); return status;
}

enum track_id {
    INTRO, TITLE, TITLE_SKIP, CONVERSATION, MENU, MENU_CLOSE, MAP,
    ROUND_SELECT, LEVEL_ENTRY, PASSWORD, EDITOR, BOSS, ENDING, INTRO_REPEAT,
    /* PISTAS-B: "What next?" and the rejected-magicword row are logic over
     * imported data (hearth.json "screen", password.json), no longer clips. */
    /* C3.2/A8 CASTILLOS: the round-10 castle cinematic of worlds 1..9, one clip per
     * world (the cinematic sequence is world*2+8), CASTLE_W1 + world index. */
    CASTLE_W1, CASTLE_W2, CASTLE_W3, CASTLE_W4, CASTLE_W5, CASTLE_W6,
    CASTLE_W7, CASTLE_W8, CASTLE_W9,
    /* MAGICWORD: the good-night cinematic after "Today's magicword" `No`. */
    GOODNIGHT,
    /* CIERRE-C3.6: the hidden-access cinematic (the hidden-rounds
     * announcement) between the title trigger and the hidden menu. */
    HIDDEN_INTRO,
    /* FIN-DE-MUNDO: the end-of-world cinematic of worlds 1..9 
     * WORLD_CLEAR_W1 + world index, and the castle opening after world 9's. */
    WORLD_CLEAR_W1, WORLD_CLEAR_W2, WORLD_CLEAR_W3, WORLD_CLEAR_W4,
    WORLD_CLEAR_W5, WORLD_CLEAR_W6, WORLD_CLEAR_W7, WORLD_CLEAR_W8,
    WORLD_CLEAR_W9, CASTLE_OPEN,
    /* CASTILLO-APARECE: the castle appearance of worlds 1..10 
     * CASTLE_APPEARS_W1 + world index. */
    CASTLE_APPEARS_W1, CASTLE_APPEARS_W2, CASTLE_APPEARS_W3, CASTLE_APPEARS_W4,
    CASTLE_APPEARS_W5, CASTLE_APPEARS_W6, CASTLE_APPEARS_W7, CASTLE_APPEARS_W8,
    CASTLE_APPEARS_W9, CASTLE_APPEARS_W10
};

enum { PW_OFF, PW_CLOSE, PW_FADE, PW_CLEAR, PW_CHR, PW_PALETTE,
       PW_UPLOAD, PW_PUBLISH, PW_READY, PW_FAILED };
enum { ED_OFF, ED_CLOSE, ED_FADE_OUT, ED_PROGRAM, ED_EXIT, ED_CLEAR, ED_CHR,
       ED_UPLOAD, ED_LAYOUT_PUBLISH, ED_FADE_IN, ED_MARKERS, ED_ROWS,
       ED_OAM, ED_LAST_PUBLISH, ED_CURSOR, ED_READY, ED_FAILED };

int od_screens_editor_ready(const od_screens *s)
{
    /* C6.31: the loop already runs while the init's last writes (round digit,
     * cursor) wait for the next display pass (ED_CURSOR). */
    return s && s->scene==OD_SCREEN_EDITOR &&
           (s->editor_phase==ED_READY || s->editor_phase==ED_CURSOR) && s->editor_status==OD_OK;
}
od_status od_screens_editor_status(const od_screens *s)
{
    return s?s->editor_status:OD_ERR_FORMAT;
}
od_status od_screens_editor_prepare_board(od_screens *s,const od_editor *ed)
{
    if(!s || !ed || s->scene!=OD_SCREEN_EDITOR || s->editor_phase>=ED_MARKERS) return OD_ERR_FORMAT;
    if(ed->start && ((ed->start>>4)<1 || (ed->start>>4)>12 || (ed->start&15)<1 || (ed->start&15)>14)) return OD_ERR_FORMAT;
    for(unsigned r=0;r<12;++r) for(unsigned c=0;c<14;++c)
        s->editor_board[r][c]=od_editor_cell_tile[ed->cells[OD_EDITOR_AREA_Y+r][OD_EDITOR_AREA_X+c]&15u];
    s->editor_start=ed->start;
    return OD_OK;
}
static int editor_setup_complete(const od_interactive_setup *art)
{
    if(!art || art->editor_entry_cinematic!=29) return 0;
    /* The queue comparison is >=60 before headers. A single record that can
     * never fit is a format error, not an indefinitely unready native task. */
    for(unsigned i=0;i<35;++i) {
        const od_menu_record *r=i<27?&art->editor_initial_layout[i]:&art->editor_toolbar_layout[i-27];
        unsigned n=r->count&63u,step=(r->count&128u)?32u:1u;
        if(!n || n>=60 || (r->count&64u) || r->addr<0x2000 ||
           r->addr+step*(n-1)>=0x2800) return 0;
    }
    return 1;
}

int od_screens_password_ready(const od_screens *s)
{
    return s && s->scene==OD_SCREEN_PASSWORD && s->password_phase==PW_READY;
}

static int password_display_valid(const uint16_t *d,int cleared,unsigned phase)
{
    for(unsigned i=0;i<OD_SCENE_WORDS;++i) if(d[i]>8191) return 0;
    for(unsigned i=0;i<32;++i) if(d[i]>63) return 0;
    for(unsigned i=32;i<2080;++i) if(d[i]>255) return 0;
    for(unsigned i=2080;i<2092;++i) if(d[i]&63u) return 0;
    if(d[2092]>255 || d[2093]>5 || d[2124]>64 || d[2381]>3) return 0;
    for(unsigned i=0;i<d[2124];++i) {
        const uint16_t *sprite=d+2125+4*i;
        if(sprite[0]>255 || sprite[1]>256 || sprite[3]>255) return 0;
    }
    if(cleared) {
        if(d[2093]!=1 || d[2381]) return 0;
        for(unsigned i=2094;i<2124;++i) if(d[i]) return 0;
        if(d[2124]!=(phase==PW_READY?2u:0u)) return 0;
    }
    return 1;
}

static int castle_track(unsigned track);
od_status od_screens_validate_candidate(const od_screens *c,const od_screens *live)
{
    if(!c || !live || !live->cinematic_data || !live->bootstrap_data || !live->editor_cinematic_data || !live->castle_round10_data || !live->world_clear_data || !live->goodnight_hidden_data || !live->boss_ending_data || !live->post_ending_data) return OD_ERR_FORMAT;
    /* Identity only: an untrusted serialized pointer is NEVER followed. */
    if(c->cinematic_data!=live->cinematic_data || c->bootstrap_data!=live->bootstrap_data ||
          c->editor_cinematic_data!=live->editor_cinematic_data || c->castle_round10_data!=live->castle_round10_data || c->world_clear_data!=live->world_clear_data || c->goodnight_hidden_data!=live->goodnight_hidden_data || c->boss_ending_data!=live->boss_ending_data || c->post_ending_data!=live->post_ending_data ||
        (c->cinematic.data!=live->cinematic_data && c->cinematic.data!=live->bootstrap_data &&
           c->cinematic.data!=live->editor_cinematic_data && c->cinematic.data!=live->castle_round10_data && c->cinematic.data!=live->world_clear_data && c->cinematic.data!=live->goodnight_hidden_data && c->cinematic.data!=live->boss_ending_data && c->cinematic.data!=live->post_ending_data)) return OD_ERR_FORMAT;
    if(c->post_ending_kind||c->post_ending_phase>4||c->post_ending_phase==2||
       (c->post_ending_status!=OD_OK&&c->post_ending_status!=OD_ERR_FORMAT))return OD_ERR_FORMAT;
    if(c->post_ending_phase) {
        if(c->scene!=OD_SCREEN_ENDING||c->track!=ENDING||c->boss_ending_phase||c->goodnight_hidden_phase||c->world_clear_phase||c->castle_round10_phase||
           od_cinematic_validate_candidate(&c->cinematic,live->post_ending_data)||
           ((c->post_ending_phase==4)!=(c->post_ending_status!=OD_OK))||
           (c->post_ending_phase==1&&c->cinematic.finished)||(c->post_ending_phase==3&&!c->cinematic.finished))return OD_ERR_FORMAT;
    } else if(c->post_ending_status!=OD_OK||c->cinematic.data==live->post_ending_data)return OD_ERR_FORMAT;
    if(c->boss_ending_phase>4||c->boss_ending_kind>1||(c->boss_ending_status!=OD_OK&&c->boss_ending_status!=OD_ERR_FORMAT))return OD_ERR_FORMAT;
    if((c->scene==OD_SCREEN_BOSS||c->scene==OD_SCREEN_ENDING)&&!c->boss_ending_phase&&!c->post_ending_phase)return OD_ERR_FORMAT;
    if(c->boss_ending_phase) {
        unsigned ending=c->boss_ending_kind;
        if(c->scene!=(ending?OD_SCREEN_ENDING:OD_SCREEN_BOSS)||c->track!=(ending?ENDING:BOSS)||c->cinematic.scene!=ending||
           !c->cinematic.sounds.managed||od_cinematic_validate_candidate(&c->cinematic,live->boss_ending_data)!=OD_OK||
           ((c->boss_ending_phase==4)!=(c->boss_ending_status!=OD_OK))||
           (c->boss_ending_phase==1&&(c->cinematic.finished||c->cinematic.cleanup_stage))||
           (c->boss_ending_phase>=2&&c->boss_ending_phase<=3&&!c->cinematic.finished)||
           (c->boss_ending_phase==2&&(c->cinematic.cleanup_stage<1||c->cinematic.cleanup_stage>2))||
           (c->boss_ending_phase==3&&c->cinematic.cleanup_stage!=3))return OD_ERR_FORMAT;
    } else if(c->boss_ending_status!=OD_OK||c->cinematic.data==live->boss_ending_data)return OD_ERR_FORMAT;
    if(c->goodnight_hidden_phase>4||c->goodnight_hidden_kind>1||
       (c->goodnight_hidden_status!=OD_OK&&c->goodnight_hidden_status!=OD_ERR_FORMAT))return OD_ERR_FORMAT;
    if((c->scene==OD_SCREEN_GOODNIGHT||c->scene==OD_SCREEN_HIDDEN_INTRO)&&!c->goodnight_hidden_phase)return OD_ERR_FORMAT;
    if(c->goodnight_hidden_phase) {
        unsigned hidden=c->goodnight_hidden_kind;
        if(c->scene!=(hidden?OD_SCREEN_HIDDEN_INTRO:OD_SCREEN_GOODNIGHT)||c->track!=(hidden?HIDDEN_INTRO:GOODNIGHT)||
           c->cinematic.scene!=hidden||!c->cinematic.sounds.managed||
           od_cinematic_validate_candidate(&c->cinematic,live->goodnight_hidden_data)!=OD_OK||
           ((c->goodnight_hidden_phase==4)!=(c->goodnight_hidden_status!=OD_OK))||
           (c->goodnight_hidden_phase==1&&(c->cinematic.finished||c->cinematic.cleanup_stage))||
           (!hidden&&c->goodnight_hidden_phase==2)||
           (c->goodnight_hidden_phase>=2&&c->goodnight_hidden_phase<=3&&!c->cinematic.finished)||
           (hidden&&c->goodnight_hidden_phase==2&&(c->cinematic.cleanup_stage<1||c->cinematic.cleanup_stage>2))||
           (hidden&&c->goodnight_hidden_phase==3&&c->cinematic.cleanup_stage!=3))return OD_ERR_FORMAT;
    } else if(c->goodnight_hidden_status!=OD_OK||c->cinematic.data==live->goodnight_hidden_data)return OD_ERR_FORMAT;
    if(c->world_clear_phase>4 || (c->world_clear_status!=OD_OK&&c->world_clear_status!=OD_ERR_FORMAT))return OD_ERR_FORMAT;
    if(c->scene==OD_SCREEN_WORLD_CLEAR&&c->world_clear_part<2&&!c->world_clear_phase)return OD_ERR_FORMAT;
    if(c->world_clear_phase) {
        if(c->scene!=OD_SCREEN_WORLD_CLEAR||c->world_clear_world>=9||c->world_clear_part>1||
           (c->world_clear_part==1&&c->world_clear_world!=8)||!c->cinematic.sounds.managed||
           c->track!=(c->world_clear_part?CASTLE_OPEN:WORLD_CLEAR_W1+c->world_clear_world)||
           c->cinematic.scene!=(c->world_clear_part?9:c->world_clear_world)||
           od_cinematic_validate_candidate(&c->cinematic,live->world_clear_data)!=OD_OK||
           ((c->world_clear_phase==4)!=(c->world_clear_status!=OD_OK))||
           (c->world_clear_phase==1&&c->cinematic.cleanup_stage)||
           (c->world_clear_phase==2&&(c->cinematic.cleanup_stage<1||c->cinematic.cleanup_stage>2))||
           (c->world_clear_phase==3&&c->cinematic.cleanup_stage!=3)||
           (c->world_clear_phase>=2&&c->world_clear_phase<=3&&!c->cinematic.finished))return OD_ERR_FORMAT;
    } else if(c->world_clear_status!=OD_OK)return OD_ERR_FORMAT;
    if(c->castle_round10_phase>4 || (c->castle_round10_status!=OD_OK && c->castle_round10_status!=OD_ERR_FORMAT))return OD_ERR_FORMAT;
    if(c->scene==OD_SCREEN_LEVEL_ENTRY && castle_track(c->track) && !c->castle_round10_phase)return OD_ERR_FORMAT;
    if(c->castle_round10_phase) {
        if(c->scene!=OD_SCREEN_LEVEL_ENTRY || c->castle_world>=9 || !castle_track(c->track) ||
           !c->cinematic.sounds.managed ||
           c->track!=CASTLE_W1+c->castle_world ||
           od_cinematic_validate_candidate(&c->cinematic,live->castle_round10_data)!=OD_OK ||
           c->cinematic.scene!=c->castle_world ||
           ((c->castle_round10_phase==4)!=(c->castle_round10_status!=OD_OK)) ||
           (c->castle_round10_phase>=2 && c->castle_round10_phase<=3 && !c->cinematic.finished))return OD_ERR_FORMAT;
    } else if(c->castle_round10_status!=OD_OK)return OD_ERR_FORMAT;
    if(c->editor_phase>ED_FAILED || c->editor_record>27 || c->editor_row>12 ||
       c->editor_brightness>15 || c->editor_sounds.managed>1 || c->editor_sounds.count>8 ||
       c->editor_sounds.overflow>1 || (c->editor_status!=OD_OK && c->editor_status!=OD_ERR_FORMAT)) return OD_ERR_FORMAT;
    if(c->editor_start && (c->editor_start>255 || (c->editor_start>>4)<1 || (c->editor_start>>4)>12 ||
       (c->editor_start&15)<1 || (c->editor_start&15)>14)) return OD_ERR_FORMAT;
    if(c->scene!=OD_SCREEN_EDITOR) { if(c->editor_phase!=ED_OFF) return OD_ERR_FORMAT; }
    else {
        if(c->editor_phase==ED_OFF || !c->editor_sounds.managed ||
           (c->editor_phase<ED_UPLOAD && c->editor_record) ||
           (c->editor_phase==ED_UPLOAD && c->editor_record>=27) ||
           (c->editor_phase>=ED_LAYOUT_PUBLISH && c->editor_phase<=ED_READY && c->editor_record!=27) ||
           (c->editor_phase<ED_ROWS && c->editor_row) ||
           (c->editor_phase>=ED_OAM && c->editor_phase<=ED_READY && c->editor_row!=12) ||
           (c->editor_phase>=ED_MARKERS && c->editor_phase<=ED_READY && c->editor_brightness!=15) ||
           (c->editor_phase==ED_READY && c->editor_status!=OD_OK) ||
           !password_display_valid(c->editor_next,0,0) || !password_display_valid(c->display,0,0)) return OD_ERR_FORMAT;
        if((c->editor_phase==ED_FAILED)!=(c->editor_status!=OD_OK)) return OD_ERR_FORMAT;
        if(c->editor_sounds.count && c->editor_phase!=ED_PROGRAM && c->editor_phase!=ED_CLEAR &&
           !((c->editor_phase==ED_CHR || c->editor_phase==ED_EXIT) && c->cinematic.finished &&
             c->editor_sounds.ids[c->editor_sounds.count-1]==1 &&
             od_cinematic_validate_candidate(&c->cinematic,live->editor_cinematic_data)==OD_OK) &&
           !(c->editor_phase==ED_FADE_IN && c->editor_brightness==1 &&
             c->editor_sounds.count==1 && c->editor_sounds.ids[0]==30)) return OD_ERR_FORMAT;
        for(unsigned r=0;r<12;++r) for(unsigned x=0;x<14;++x) if(c->editor_board[r][x]>255) return OD_ERR_FORMAT;
        if(c->editor_phase==ED_PROGRAM &&
           od_cinematic_validate_candidate(&c->cinematic,live->editor_cinematic_data)!=OD_OK) return OD_ERR_FORMAT;
    }
    if(c->password_phase>PW_FAILED || c->password_record>14 || c->password_brightness>14 ||
        c->password_sounds.count>1 || c->password_sounds.managed>1) return OD_ERR_FORMAT;
    if(c->scene==OD_SCREEN_MAGICWORD&&od_gfx_static_magicword()&&od_scene_magicword_validate_static(&c->mw))return OD_ERR_FORMAT;
    if(c->password_sounds.count && (!c->password_sounds.managed || c->password_sounds.ids[0]!=27 ||
       c->password_phase!=PW_UPLOAD || c->password_record)) return OD_ERR_FORMAT;
    if(c->scene!=OD_SCREEN_PASSWORD)
        return c->password_phase==PW_OFF?OD_OK:OD_ERR_FORMAT;
    if(c->password_phase==PW_OFF || !c->password_sounds.managed) return OD_ERR_FORMAT;
    if((c->password_phase<PW_UPLOAD && c->password_record) ||
       (c->password_phase==PW_UPLOAD && c->password_record>=14) ||
       ((c->password_phase==PW_READY || c->password_phase==PW_PUBLISH) && c->password_record!=14)) return OD_ERR_FORMAT;
    int cleared=c->password_phase>=PW_CHR && c->password_phase<=PW_READY;
    if(!password_display_valid(c->password_next,cleared,c->password_phase) ||
       !password_display_valid(c->display,0,c->password_phase)) return OD_ERR_FORMAT;
    return OD_OK;
}

static int castle_track(unsigned track)
{
    return track >= CASTLE_W1 && track <= CASTLE_W9;
}

void od_screens_rebind_cinematic(od_screens *s)
{
    s->cinematic.data=s->post_ending_phase?s->post_ending_data:s->boss_ending_phase?s->boss_ending_data:s->goodnight_hidden_phase?s->goodnight_hidden_data:s->world_clear_phase?s->world_clear_data:s->castle_round10_phase?s->castle_round10_data:s->scene==OD_SCREEN_EDITOR?s->editor_cinematic_data:
        s->track>=CASTLE_APPEARS_W1 && s->track<=CASTLE_APPEARS_W10?
        s->cinematic_data:s->bootstrap_data;
}

od_status od_screens_take_sounds(od_screens *s,od_cinematic_sounds *out)
{
    if(!s || !out) return OD_ERR_FORMAT;
    if(s->scene==OD_SCREEN_MAGICWORD&&od_gfx_static_magicword()) {
        unsigned id=0;int present=0;memset(out,0,sizeof *out);out->managed=1;
        od_status st=od_scene_magicword_take_static_sound(&s->mw,&id,&present);
        if(st)return st;
        if(present){out->count=1;out->ids[0]=(uint8_t)id;}return OD_OK;
    }
    if(s->post_ending_phase) {
        if(!s->cinematic.sounds.managed)return OD_ERR_FORMAT;
        od_status st=od_cinematic_take_sounds(&s->cinematic,out);
        return st||s->post_ending_status!=OD_OK?OD_ERR_FORMAT:OD_OK;
    }
    if(s->boss_ending_phase) {
        if(!s->cinematic.sounds.managed)return OD_ERR_FORMAT;
        od_status st=od_cinematic_take_sounds(&s->cinematic,out);
        return st||s->boss_ending_status!=OD_OK?OD_ERR_FORMAT:OD_OK;
    }
    if(s->goodnight_hidden_phase) {
        if(!s->cinematic.sounds.managed)return OD_ERR_FORMAT;
        od_status st=od_cinematic_take_sounds(&s->cinematic,out);
        return st||s->goodnight_hidden_status!=OD_OK?OD_ERR_FORMAT:OD_OK;
    }
    if(s->world_clear_phase) {
        if(!s->cinematic.sounds.managed)return OD_ERR_FORMAT;
        od_status st=od_cinematic_take_sounds(&s->cinematic,out);
        return st||s->world_clear_status!=OD_OK?OD_ERR_FORMAT:OD_OK;
    }
    if(s->castle_round10_phase) {
        od_status st=od_cinematic_take_sounds(&s->cinematic,out);
        return st || s->castle_round10_status!=OD_OK?OD_ERR_FORMAT:OD_OK;
    }
    if(s->scene==OD_SCREEN_EDITOR) {
        *out=s->editor_sounds;
        s->editor_sounds.count=0;
        return out->overflow || s->editor_status!=OD_OK?OD_ERR_FORMAT:OD_OK;
    }
    if(s->scene==OD_SCREEN_PASSWORD) {
        *out=s->password_sounds;
        memset(&s->password_sounds,0,sizeof s->password_sounds);
        s->password_sounds.managed=1;
        return OD_OK;
    }
    if((s->track>=CASTLE_APPEARS_W1 && s->track<=CASTLE_APPEARS_W10) ||
       s->track==INTRO || s->track==INTRO_REPEAT || s->track==CONVERSATION ||
       s->track==TITLE || s->track==TITLE_SKIP)
        return od_cinematic_take_sounds(&s->cinematic,out);
    memset(out,0,sizeof *out);
    return OD_OK;
}

/* C4.2 PISTAS-A: NULL marks a scene drawn by logic over a static layout of the
 * package (screens/layouts.json): the title, the "Where to begin from?" menu and
 * its close, the WORLD map, the round select (the map logic) and the level entry
 * (the fade of the accepted map). Nothing is loaded for them; the recorded clips
 * they replaced are reference captures, private since C6.18. */
static const char *const track_names[OD_SCREEN_TRACKS] = {
    NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, "boss",
    "ending", NULL,
    "castle-1", "castle-2", "castle-3", "castle-4", "castle-5", "castle-6",
    "castle-7", "castle-8", "castle-9", "goodnight", "hidden-intro",
    "world-clear-1", "world-clear-2", "world-clear-3", "world-clear-4",
    "world-clear-5", "world-clear-6", "world-clear-7", "world-clear-8",
    "world-clear-9", "castle-open",
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static od_status load_track_named(od_scene_track *t,const char *name)
{
    char rel[128],*bytes=NULL; size_t size=0;
    int n=snprintf(rel,sizeof rel,"screens/tracks/%s.track",name);
    if(n<0 || n>=(int)sizeof rel) return OD_ERR_IO;
    od_status status=od_gfx_content_read(rel,&bytes,&size);
    /* C6.18: a genuinely absent legacy clip leaves the track zeroed. The native
     * cinematic families (boss/ending/goodnight/hidden/world-clear/castle-round10/
     * castle-appears/post-ending) supply every scene that these 23 recorded tracks
     * used to supply. A clip that IS present must still parse: `parse_track_text`
     * errors stay errors, including its own OOM. */
    if(status==OD_ERR_IO) { free(bytes); return OD_OK; }
    if(status==OD_OK) status=parse_track_text(t,bytes,size);
    free(bytes); return status;
}

/* The captured castle/world-clear descriptors are optional only when the
 * package genuinely lacks the file: the native families replace those scenes.
 * A descriptor that IS present but does not provide every world/clip is still
 * a hard error (the getter stays NULL for the missing entries). */
static int descriptor_absent(const char *rel)
{
    char path[1200];
    return od_gfx_resolve(rel,path,sizeof path)==OD_ERR_IO;
}

/* Optional when absent for the same reason as the tracks: the imported
 * package omits it. Absence leaves `level_fade` zeroed (no fabricated fade);
 * a present but malformed fade is still an error. */
static od_status load_fade(od_screens *s)
{
    char *bytes=NULL; size_t size=0;
    od_status status=od_gfx_content_read("screens/tracks/level-entry.fade",&bytes,&size);
    if(status==OD_ERR_IO) { free(bytes); return OD_OK; }
    if(status==OD_OK) status=parse_fade_text(s,bytes,size);
    free(bytes); return status;
}

static void world_clear_begin(od_screens *s)
{
    if(s->world_clear_world>=9||s->world_clear_part>1) {
        s->world_clear_phase=4;s->world_clear_status=OD_ERR_FORMAT;s->status=OD_ERR_FORMAT;return;
    }
    od_cinematic_begin(&s->cinematic,s->world_clear_data,s->world_clear_part?9:s->world_clear_world);
    /* Entry comes from the actual live publisher, not a captured first frame. */
    for(unsigned i=0;i<2048;++i)s->cinematic.tiles[i]=(uint8_t)s->display[32+i];
    memcpy(s->cinematic.pending_oam,s->display+2124,sizeof s->cinematic.pending_oam);
    for(unsigned i=0;i<4;++i){s->cinematic.bg_banks[i]=(uint8_t)(s->display[2080+i]/64);s->cinematic.split_banks[i]=(uint8_t)(s->display[2084+i]/64);}
    for(unsigned i=0;i<2;++i)s->cinematic.sprite_banks[i]=(uint8_t)(s->display[2088+i]/64);
    s->cinematic.sounds.managed=1;
    s->world_clear_phase=1;s->world_clear_status=OD_OK;
}
static void enter(od_screens *s, od_screen_id scene, unsigned track)
{
    uint16_t live_display[OD_SCENE_WORDS];
    int night=scene==OD_SCREEN_GOODNIGHT||scene==OD_SCREEN_HIDDEN_INTRO;
    int boss=scene==OD_SCREEN_BOSS||scene==OD_SCREEN_ENDING;
    int static_logic=(scene==OD_SCREEN_MAGICWORD&&od_gfx_static_magicword())||
        ((scene==OD_SCREEN_MAP||scene==OD_SCREEN_ROUND_SELECT)&&od_gfx_static_layouts());
    if(night||boss||static_logic)memcpy(live_display,s->display,sizeof live_display);
    s->boss_ending_phase=0;s->boss_ending_kind=0;s->boss_ending_status=OD_OK;
    s->post_ending_phase=0;s->post_ending_kind=0;s->post_ending_status=OD_OK;
    s->goodnight_hidden_phase=0;s->goodnight_hidden_kind=0;s->goodnight_hidden_status=OD_OK;
    s->castle_round10_phase=0;s->castle_round10_status=OD_OK;
    s->editor_phase=ED_OFF; s->editor_record=s->editor_row=s->editor_start=0;
    s->world_clear_phase=0;s->world_clear_status=OD_OK;
    s->editor_status=OD_OK; memset(&s->editor_sounds,0,sizeof s->editor_sounds);
    if(scene==OD_SCREEN_EDITOR) {
        memcpy(s->editor_next,s->display,sizeof s->display);
        s->editor_phase=s->hearth_close?ED_CLOSE:ED_PROGRAM;
        s->editor_brightness=14; s->editor_sounds.managed=1;
        od_editor ed; od_editor_init(&ed);
        for(unsigned r=0;r<12;++r) for(unsigned c=0;c<14;++c)
            s->editor_board[r][c]=(uint16_t)od_editor_cell_draw(&ed,OD_EDITOR_AREA_Y+r,OD_EDITOR_AREA_X+c);
        od_cinematic_begin(&s->cinematic,s->editor_cinematic_data,0);
    }
    s->password_phase=PW_OFF;
    s->password_record=0;
    memset(&s->password_sounds,0,sizeof s->password_sounds);
    if(scene==OD_SCREEN_PASSWORD) {
        memcpy(s->password_next,s->display,sizeof s->display);
        s->password_phase=s->hearth_close?PW_CLOSE:PW_CLEAR;
        s->password_brightness=14;
        s->password_sounds.managed=1;
    }
    s->scene = scene;
    s->track = track;
    s->local_tick = s->track_tick = s->event = s->age = 0;
    s->pending = 0;
    s->hearth.calls = 0; /* the new scene has drawn no hearth OAM yet */
    s->title_exit = 0;
    s->title_colour_valid = 0; /* the loop palette's entry 15 until painted */
    s->hidden_skip = 0;
    /* `level_castle` is staged by the round-focus `A` and consumed by the
     * LEVEL_ENTRY scene; it is owned by that schedule, not cleared here. */
    memset(s->display, 0, sizeof s->display);
    if(static_logic)memcpy(s->display,live_display,sizeof s->display);
    if(scene==OD_SCREEN_EDITOR) memcpy(s->display,s->editor_next,sizeof s->display);
    if(scene==OD_SCREEN_WORLD_CLEAR&&s->world_clear_part<2)world_clear_begin(s);
    if(scene==OD_SCREEN_PASSWORD)
        memcpy(s->display,s->password_next,sizeof s->display);
    if(night) {
        unsigned hidden=scene==OD_SCREEN_HIDDEN_INTRO;
        od_cinematic_begin(&s->cinematic,s->goodnight_hidden_data,hidden);
        for(unsigned i=0;i<2048;++i)s->cinematic.tiles[i]=(uint8_t)live_display[32+i];
        memcpy(s->cinematic.pending_oam,live_display+2124,sizeof s->cinematic.pending_oam);
        for(unsigned i=0;i<4;++i){s->cinematic.bg_banks[i]=(uint8_t)(live_display[2080+i]/64);s->cinematic.split_banks[i]=(uint8_t)(live_display[2084+i]/64);}
        for(unsigned i=0;i<2;++i)s->cinematic.sprite_banks[i]=(uint8_t)(live_display[2088+i]/64);
        s->cinematic.sounds.managed=1;
        s->goodnight_hidden_phase=1;s->goodnight_hidden_kind=hidden;
    }
    if(boss) {
        unsigned ending=scene==OD_SCREEN_ENDING;
        od_cinematic_begin(&s->cinematic,s->boss_ending_data,ending);
        for(unsigned i=0;i<2048;++i)s->cinematic.tiles[i]=(uint8_t)live_display[32+i];
        memcpy(s->cinematic.pending_oam,live_display+2124,sizeof s->cinematic.pending_oam);
        for(unsigned i=0;i<4;++i){s->cinematic.bg_banks[i]=(uint8_t)(live_display[2080+i]/64);s->cinematic.split_banks[i]=(uint8_t)(live_display[2084+i]/64);}
        for(unsigned i=0;i<2;++i)s->cinematic.sprite_banks[i]=(uint8_t)(live_display[2088+i]/64);
        s->cinematic.sounds.managed=1;s->boss_ending_phase=1;s->boss_ending_kind=ending;
    }
    if(track>=CASTLE_APPEARS_W1 && track<=CASTLE_APPEARS_W10)
        od_cinematic_begin(&s->cinematic,s->cinematic_data,track-CASTLE_APPEARS_W1);
    else if(track==INTRO)
        od_cinematic_begin_reset(&s->cinematic,s->bootstrap_data);
    else if(track==INTRO_REPEAT || track==CONVERSATION || track==TITLE || track==TITLE_SKIP) {
        /* The title's last script frame launches the repeat's own engine (its init
         * request) before the scene hands over, so the repeat's first image is the
         * frame after the title's last. The title case begins it a frame early (one
         * init tick); entering the scene must continue that run instead of restarting
         * it. */
        int repeat_lead=track==INTRO_REPEAT && s->cinematic.data==s->bootstrap_data &&
            s->cinematic.scene==0 && s->cinematic.tick==1;
        if(!repeat_lead)
            od_cinematic_begin(&s->cinematic,s->bootstrap_data,track==CONVERSATION?1:(track==TITLE || track==TITLE_SKIP)?2:0);
    }
}

static od_status load_cinematics(od_screens *s)
{
    const char *rel="screens/cinematics/castle-appears.json";
    char *bytes=NULL; size_t size=0;
    od_status status=od_gfx_content_read(rel,&bytes,&size);
    if(status==OD_OK) status=od_cinematic_load_text(&s->cinematic_data,bytes,size);
    free(bytes); bytes=NULL;
    if(status!=OD_OK) return status;
    bytes=NULL;
    status=od_gfx_content_read("screens/cinematics/castle-round10.json",&bytes,&size);
    if(status==OD_OK)status=od_cinematic_load_text(&s->castle_round10_data,bytes,size);
    free(bytes);bytes=NULL;
    if(status!=OD_OK)return status;
    for(unsigned w=0;w<9;++w) {
        unsigned index=0,duration=0;
        if(od_cinematic_scene_index(s->castle_round10_data,w,&index)!=OD_OK || index!=8+2*w)return OD_ERR_FORMAT;
        /* Eager executable closure only. Duration is NOT stored/used as an
         * agenda, caller deadline or skip window. Unknown operations fail. */
        status=od_cinematic_duration(s->castle_round10_data,w,&duration);
        if(status!=OD_OK)return status;
    }
    bytes=NULL;
    status=od_gfx_content_read("screens/cinematics/world-clear.json",&bytes,&size);
    if(status==OD_OK)status=od_cinematic_load_text(&s->world_clear_data,bytes,size);
    free(bytes);bytes=NULL;
    if(status!=OD_OK)return status;
    for(unsigned scene=0;scene<10;++scene) {
        unsigned index=0;
        if(od_cinematic_scene_index(s->world_clear_data,scene,&index)!=OD_OK||index!=(scene<9?9+2*scene:42))return OD_ERR_FORMAT;
        od_cinematic c;uint16_t d[OD_SCENE_WORDS];od_cinematic_begin(&c,s->world_clear_data,scene);
        for(unsigned tick=0;(!c.finished||c.cleanup_stage!=3)&&tick<65535;++tick) {
            status=od_cinematic_tick(&c,d);
            if(status!=OD_OK||od_cinematic_validate_candidate(&c,s->world_clear_data)!=OD_OK)return OD_ERR_FORMAT;
        }
    if(!c.finished||c.cleanup_stage!=3)return OD_ERR_FORMAT; /* no stored duration or cue agenda */
    }
    status=od_gfx_content_read("screens/cinematics/post-ending.json",&bytes,&size);
    if(status==OD_OK)status=od_cinematic_load_text(&s->post_ending_data,bytes,size);
    free(bytes);bytes=NULL;if(status!=OD_OK)return status;
    {
        unsigned index=0;if(od_cinematic_scene_index(s->post_ending_data,0,&index)||index!=7)return OD_ERR_FORMAT;
        od_cinematic c;uint16_t display[OD_SCENE_WORDS];od_cinematic_begin(&c,s->post_ending_data,0);
        for(unsigned step=0;step<65535&&!c.finished;++step) {
            if(od_cinematic_tick(&c,display)||od_cinematic_validate_candidate(&c,s->post_ending_data))return OD_ERR_FORMAT;
        }
        if(!c.finished||c.cleanup_stage)return OD_ERR_FORMAT;
    }
    status=od_gfx_content_read("screens/cinematics/boss-ending.json",&bytes,&size);
    if(status==OD_OK)status=od_cinematic_load_text(&s->boss_ending_data,bytes,size);
    free(bytes);bytes=NULL;
    if(status!=OD_OK)return status;
    for(unsigned root=0;root<2;++root) {
        unsigned index=0;if(od_cinematic_scene_index(s->boss_ending_data,root,&index)||index!=26+root)return OD_ERR_FORMAT;
        od_cinematic c;uint16_t display[OD_SCENE_WORDS];od_cinematic_begin(&c,s->boss_ending_data,root);
        for(unsigned step=0;step<65535&&(!c.finished||c.cleanup_stage!=3);++step) {
            if(od_cinematic_tick(&c,display)||od_cinematic_validate_candidate(&c,s->boss_ending_data))return OD_ERR_FORMAT;
        }
        if(!c.finished||c.cleanup_stage!=3)return OD_ERR_FORMAT;
    }
    status=od_gfx_content_read("screens/cinematics/goodnight-hidden.json",&bytes,&size);
    if(status==OD_OK)status=od_cinematic_load_text(&s->goodnight_hidden_data,bytes,size);
    free(bytes);bytes=NULL;
    if(status!=OD_OK)return status;
    for(unsigned root=0;root<2;++root) {
        unsigned index=0;if(od_cinematic_scene_index(s->goodnight_hidden_data,root,&index)||index!=(root?5:31))return OD_ERR_FORMAT;
        od_cinematic c;uint16_t display[OD_SCENE_WORDS];od_cinematic_begin(&c,s->goodnight_hidden_data,root);
        for(unsigned step=0;step<65535&&(!c.finished||(root&&c.cleanup_stage!=3));++step) {
            if(od_cinematic_tick(&c,display)||od_cinematic_validate_candidate(&c,s->goodnight_hidden_data))return OD_ERR_FORMAT;
        }
        if(!c.finished||(root&&c.cleanup_stage!=3))return OD_ERR_FORMAT;
    }
    bytes=NULL;
    status=od_gfx_content_read("screens/cinematics/editor-entry.json",&bytes,&size);
    if(status==OD_OK) status=od_cinematic_load_text(&s->editor_cinematic_data,bytes,size);
    free(bytes);
    if(status!=OD_OK) return status;
    unsigned editor_index=0;
    if(od_cinematic_scene_index(s->editor_cinematic_data,0,&editor_index)!=OD_OK || editor_index!=29) return OD_ERR_FORMAT;
    unsigned editor_duration=0;
    status=od_cinematic_duration(s->editor_cinematic_data,0,&editor_duration);
    if(status!=OD_OK) return status;
    bytes=NULL;
    status=od_gfx_content_read("screens/cinematics/bootstrap.json",&bytes,&size);
    if(status==OD_OK) status=od_cinematic_load_text(&s->bootstrap_data,bytes,size);
    free(bytes);
    if(status!=OD_OK) return status;
    for(unsigned scene=0;scene<3;++scene) {
        unsigned track=scene==1?CONVERSATION:scene==2?TITLE:INTRO;
        status=od_cinematic_duration(s->bootstrap_data,scene,&s->tracks[track].duration);
        if(status!=OD_OK) return status;
        s->tracks[track].loop=-1;
        if(!scene) s->tracks[INTRO_REPEAT]=s->tracks[INTRO];
        if(scene==2) s->tracks[TITLE_SKIP]=s->tracks[TITLE];
    }
    status=od_cinematic_reset_duration(s->bootstrap_data,&s->tracks[INTRO].duration);
    if(status!=OD_OK) return status;
    /* The old flow API reads a timing record. Derive it by running the native
     * program once: no captured timeline or frame-number table is loaded. */
    for(unsigned w=0;w<10;++w) {
        od_cinematic c; uint16_t d[OD_SCENE_WORDS];
        od_cinematic_begin(&c,s->cinematic_data,w);
        od_castle_info *info=&s->cinematic_info[w];
        info->loaded=1; info->skip_from=65535;
        for(unsigned t=0;t<65535;++t) {
            if(c.flush==2 && info->frame_tick_lost_count<4)
                info->frame_tick_lost[info->frame_tick_lost_count++]=(uint16_t)t;
            status=od_cinematic_tick(&c,d);
            if(status!=OD_OK) return status;
            if(c.sound) {
                if(info->cue_count>=OD_CASTLE_CUES_MAX) return OD_ERR_FORMAT;
                info->cues[info->cue_count++]=(od_castle_cue){(uint16_t)t,(uint8_t)c.sound};
            }
            if(c.skip_enabled && info->skip_from==65535) info->skip_from=t;
            if(c.finished) {
                info->duration=t+2; /* cleanup flush before the caller resumes */
                if(info->skip_from==65535) info->skip_from=info->duration;
                s->tracks[CASTLE_APPEARS_W1+w].duration=info->duration;
                s->tracks[CASTLE_APPEARS_W1+w].loop=-1;
                break;
            }
        }
        if(!info->duration) return OD_ERR_FORMAT;
    }
    return OD_OK;
}

od_status od_screens_init(od_screens *s, const char *assets_dir)
{
    (void)assets_dir; /* Compatibility argument, never a disk fallback. */
    memset(s, 0, sizeof *s);
    od_hidden_scene_reset(&s->hidden);
    for (unsigned i = 0; i < OD_SCREEN_TRACKS; ++i) {
        if (!track_names[i])
            continue; /* a logic scene: no clip */
        /* An absent clip stays zeroed (native family covers the scene); a
         * present but malformed clip returned an error above. */
        od_status status = load_track_named(&s->tracks[i], track_names[i]);
        if (status != OD_OK) { od_screens_free(s); return status; }
    }
    od_status cinematic_status=load_cinematics(s);
    if(cinematic_status!=OD_OK) { od_screens_free(s); return cinematic_status; }
    /* The WORLD map background: the package layout (PISTAS-A). */
    {
        const od_screen_layouts *l = od_gfx_layouts();
        if(l)memcpy(s->map_maqueta,l->map,sizeof s->map_maqueta);
        else if(od_gfx_static_layouts()) {
            od_scene_map m={0};
            if(od_scene_map_static_base(&m,s->map_maqueta,OD_SCENE_WORDS)) {od_screens_free(s);return OD_ERR_FORMAT;}
            map_apply_static(od_gfx_map_art(),s->map_maqueta);
        } else {od_screens_free(s);return OD_ERR_IO;}
    }
    od_status status=load_fade(s);
    /* An absent fade leaves level_fade zero, never fabricated. */
    if(status==OD_OK && (!od_gfx_loaded() || !od_gfx_boss_arena() ||
       !od_gfx_retry_tiles() || !od_gfx_retry_palette() || !od_gfx_retry_cursor_cols() ||
       !od_gfx_map_art() || !od_gfx_hearth_art() || (!od_gfx_magicword_art()&&!od_gfx_static_magicword()) ||
       !od_gfx_password_art() || !od_interactive_setup_get())) status=OD_ERR_IO;
    /* The captured castles.json/world-clears.json descriptors are absent from the
     * imported package; the native castle-appears/world-clear families supply those
     * scenes. A descriptor that is present but incomplete still fails closed
     * through the getters below. */
    if(status==OD_OK && s->cinematic_data && descriptor_absent("screens/castles.json"))
        ; /* native castle family covers the 9 castle windows */
    else
        for(unsigned i=0;status==OD_OK && i<9;++i)
            if(!od_gfx_castle((int)i)) status=OD_ERR_IO;
    if(status==OD_OK && s->world_clear_data && descriptor_absent("screens/world-clears.json"))
        ; /* native world-clear family covers the 10 clips */
    else
        for(unsigned i=0;status==OD_OK && i<10;++i)
            if(!od_gfx_world_clear((int)i)) status=OD_ERR_IO;
    if(status==OD_OK && !editor_setup_complete(od_interactive_setup_get())) status=OD_ERR_FORMAT;
    if(status==OD_OK) status=od_gfx_scene_resources_complete();
    if(status!=OD_OK) { od_screens_free(s); return status; }
    enter(s, OD_SCREEN_INTRO, INTRO);
    return OD_OK;
}

void od_screens_free(od_screens *s)
{
    od_cinematic_free(s->world_clear_data);s->world_clear_data=NULL;
    od_cinematic_free(s->boss_ending_data);s->boss_ending_data=NULL;
    od_cinematic_free(s->post_ending_data);s->post_ending_data=NULL;
    od_cinematic_free(s->goodnight_hidden_data);s->goodnight_hidden_data=NULL;
    od_cinematic_free(s->castle_round10_data);s->castle_round10_data=NULL;
    od_cinematic_free(s->editor_cinematic_data);
    s->editor_cinematic_data=NULL;
    od_cinematic_free(s->cinematic_data);
    s->cinematic_data=NULL;
    od_cinematic_free(s->bootstrap_data);
    s->bootstrap_data=NULL;
    for (unsigned i = 0; i < OD_SCREEN_TRACKS; ++i) {
        free(s->tracks[i].changes);
        s->tracks[i].changes = NULL;
    }
}

/* Console RESET button: back to the opening scene. The loaded tracks and the
 * save file are reused; the caller reloads the session progress. */
void od_screens_boot(od_screens *s)
{
    s->selection = s->visible_selection = 0;
    s->map_focus = 0;
    s->map_ready = 0;
    s->world = s->round = 0;
    s->pending = 0;
    s->menu_init_wait = 0;
    od_hidden_scene_reset(&s->hidden);
    enter(s, OD_SCREEN_INTRO, INTRO);
    s->level_castle = 0;
}

void od_screens_power_on(od_screens *s)
{
    /* What od_screens_init loads before its enter(INTRO); everything else is
     * the zeroed power-on state. The copy is static: the struct is large. */
    static od_screens keep;
    keep = *s;
    memset(s, 0, sizeof *s);
    memcpy(s->tracks, keep.tracks, sizeof s->tracks);
    s->cinematic_data = keep.cinematic_data;
    s->bootstrap_data = keep.bootstrap_data;
    s->editor_cinematic_data = keep.editor_cinematic_data;
    s->castle_round10_data = keep.castle_round10_data;
    s->world_clear_data = keep.world_clear_data;
    s->goodnight_hidden_data = keep.goodnight_hidden_data;
    s->boss_ending_data = keep.boss_ending_data;
    s->post_ending_data = keep.post_ending_data;
    memcpy(s->cinematic_info, keep.cinematic_info, sizeof s->cinematic_info);
    s->level_fade_frames = keep.level_fade_frames;
    memcpy(s->level_fade, keep.level_fade, sizeof s->level_fade);
    memcpy(s->map_maqueta, keep.map_maqueta, sizeof s->map_maqueta);
    s->frame_tick_by_flow = keep.frame_tick_by_flow;   /* the host's wiring, not RAM */
    memset(&keep, 0, sizeof keep);
    od_hidden_scene_reset(&s->hidden);
    enter(s, OD_SCREEN_INTRO, INTRO);
}

void od_screens_menu(od_screens *s)
{
    s->selection = s->visible_selection = 0;
    enter(s, OD_SCREEN_MENU, MENU);
}

static void map_begin(od_screens *s);
static void map_refresh(od_screens *s);

void od_screens_map(od_screens *s)
{
    s->world = s->round = 0;
    memset(s->map_words, 0, sizeof s->map_words);
    s->map_ready = OD_MAP_REVEAL_FRAMES;
    enter(s, OD_SCREEN_MAP, MAP);
    map_begin(s);
    map_refresh(s);
    s->map_reveal = 0;
}

/* C3.2: the WORLD map is logic (scene_map.c). The extracted `map.track` is
 * read only once, at init, to freeze its steady layout as the background
 * maqueta; the world/round focus, the progress marks, the `WORLD n` number and
 * the Hero cursor are all driven from the session progress. */
/* C6.18: apply the map's own palette and PPU setup (screens/map.json "static")
 * over the v2 CIRAM/banks. Without it the map inherits the previous scene's
 * display (menu close: black), the defect the genuine package showed. The v1
 * layouts.json:map path carries these words directly and never calls this. */
static void map_apply_static(const od_map_art *art, uint16_t *display)
{
    if (!art || !art->loaded || !art->static_loaded)
        return;
    for (unsigned i = 0; i < 32; ++i) display[i] = art->palette[i];
    for (unsigned i = 0; i < 4; ++i) {
        display[2084 + i] = art->split_banks[i];
        display[2088 + i] = art->sprite_banks[i];
    }
    display[2092] = art->mask;
    display[2093] = art->bands;
    for (unsigned i = 0; i < 12; ++i) display[2094 + i] = art->band_table[i];
}

static void map_begin(od_screens *s)
{
    /* The map init clears the hearth counters along with the rest of its state. */
    od_hearth_reset(&s->hearth);
    s->hearth_close = 0;
    s->map_next_valid = 0;
    s->map_resume = 0;
    od_scene_map_open(&s->map, s->map_words, (int)s->world, s->map_ready);
    if(od_gfx_static_layouts()) {
        /* The frozen maqueta is the v2 CIRAM/banks plus the map's own palette
         * and PPU setup, not the previous scene's live display. */
        if(od_scene_map_static_base(&s->map,s->map_maqueta,OD_SCENE_WORDS)){s->status=OD_ERR_FORMAT;return;}
        map_apply_static(od_gfx_map_art(),s->map_maqueta);
    } else memcpy(s->display,s->map_maqueta,sizeof s->display);
    s->world = (unsigned)s->map.world;
    s->map_focus = (unsigned)s->map.focus;
    s->round = (unsigned)s->map.sel.round;
}

/* Rebuild the map display from the frozen maqueta and the logic panel. Called
 * every tick of the two map scenes, so B never clears the layout (A7: no gray
 * frame). */
static void map_refresh(od_screens *s)
{
    const od_map_art *art = od_gfx_map_art();
    memcpy(s->display, s->map_maqueta, sizeof s->display);
    /* MAPA-MUNDOS: the castle map once world 9's castle is cleared, the panel
     * marks, then the sprites (Hero, the landmark frame, the WORLD number, the
     * objects and the panel castle) in place of the ones the extracted world-1
     * layout carries. */
    if(od_gfx_static_layouts()) {
        if(od_scene_map_static_base(&s->map,s->display,OD_SCENE_WORDS)){s->status=OD_ERR_FORMAT;return;}
        map_apply_static(art, s->display);
    } else od_scene_map_castle(&s->map, art, s->display, OD_SCENE_WORDS);
    od_scene_map_panel(&s->map, s->display, OD_SCENE_WORDS);
    od_scene_map_sprites(&s->map, art, s->frame_cycle, s->display,
                          OD_SCENE_WORDS);
    s->world = (unsigned)s->map.world;
    s->round = (unsigned)s->map.sel.round;
    s->map_focus = (unsigned)s->map.focus;
}

void od_screens_map_progress(od_screens *s, const uint16_t words[10],
                              unsigned ready)
{
    od_screens_map_progress_world(s, words, 0, ready);
}

void od_screens_map_progress_world(od_screens *s, const uint16_t words[10],
                                    int world, unsigned ready)
{
    for (int w = 0; w < 10; ++w) s->map_words[w] = words[w];
    s->map_ready = ready;
    s->map_focus = 0;
    s->round = 0;
    s->world = world < 0 ? 0u : (unsigned)world;
    enter(s, OD_SCREEN_MAP, MAP);
    map_begin(s);
    map_refresh(s);
    s->map_reveal = 0;
}

/* A map opened with the resume flag (set when a solved round hands back to the
 * map, reference resume flag set at f3991) starts in ROUND focus on the first
 * uncleared round of the world (flag changed, round 1 at f4040 after 1-1):
 * `od_scene_map_resume_round`, which also keeps the WORLD focus on round 10
 * without the castle latch. With all ten rounds cleared the search runs out and
 * falls to the WORLD focus. There is no scene change in the original (mode 5
 * throughout), so none here. */
/* The opening's lead in frames. Once world 9's castle is cleared the castle map
 * script runs instead of the ordinary one, and its reveal, loop init and first
 * read input all come one frame earlier (arena NEW GAME: castle script f26380, map
 * f26423, init flag set f26425; the ordinary opening of the same route frame
 * by frame is the castle one shifted by one, 0-23 px). */
static unsigned map_reveal_lead(const od_screens *s)
{
    return s->map_reveal && od_scene_map_castle_open(&s->map) ? 1u : 0u;
}

unsigned od_screens_map_lead(const od_screens *s)
{
    return map_reveal_lead(s);
}

/* With all ten rounds cleared the search runs out to the WORLD focus. Returns 1
 * when the ROUND focus was resumed. */
static int map_resume_round_focus(od_screens *s)
{
    if ((s->map.words[s->map.world] & 0x03ffu) == 0x03ffu)
        return 0;
    od_scene_map_resume_round(&s->map);
    if (s->map.focus != 1)
        return 0;
    s->scene = OD_SCREEN_ROUND_SELECT;
    return 1;
}

void od_screens_map_round_focus_after_clear(od_screens *s)
{
    if (s->scene != OD_SCREEN_MAP)
        return;
    if (s->map_reveal) {
        s->map_resume = 1;   /* the loop init applies it (map age 46) */
        return;
    }
    if (!map_resume_round_focus(s))
        return;
    map_refresh(s);
    s->map_next_valid = 0;
}

int od_screens_map_level(const od_screens *s)
{
    /* C3.6 integration: the hidden worlds 11-15 sit above the map's 0..99
     * range. The hidden menu (scene_hidden.c) sets `s->world` 10..14 and
     * `s->round` 0..9 directly; the ordinary map keeps its focus in `s->map`. */
    if (s->world >= OD_MAP_WORLD_COUNT)
        return (int)(s->world * 10u + s->round);
    return od_scene_map_level(&s->map);
}

int od_screens_map_world(const od_screens *s)
{
    return s->map.world;
}

void od_screens_editor(od_screens *s)
{
    enter(s, OD_SCREEN_EDITOR, EDITOR);
}

/* EE0A/E3E4 and EE0D/E1BF: clear both NT, attributes, scroll and OAM.
 * This is the real cleanup, not a retained last cinematic frame. */
static void editor_clear_job(od_screens *s)
{
    memset(s->editor_next,0,sizeof s->editor_next);
    for(unsigned i=0;i<32;++i) s->editor_next[i]=(i>=16 && !(i&3u))?0:15;
    s->editor_next[2093]=1;
    memcpy(s->display,s->editor_next,sizeof s->display);
    s->editor_phase=ED_CHR;
}

void od_screens_editor_return(od_screens *s, unsigned tick)
{
    /* The editor mode bypasses the clear and then executes the same real init
     * jobs. The resumed board is copied by prepare_board, never recovered from a
     * clip. */
    enter(s, OD_SCREEN_EDITOR, EDITOR);
    (void)tick; /* compatibility argument: the editor entry never seeks. */
    /* C6.31 (D6): the editor entry's first job, the clear, already runs on the frame
     * the editor is entered from the level (solved: reference R7 f3914; NEW GAME:
     * f3216): that frame is black, not the editor kept from before PLAY, and
     * every later init job lands one frame earlier (the fade from f3924, the
     * twelve board rows f3940..f3951). */
    editor_clear_job(s);
}

void od_screens_password(od_screens *s)
{
    s->password_error = 0;
    s->password_error_delay = 0;
    s->password_error_blank = 0;
    enter(s, OD_SCREEN_PASSWORD, PASSWORD);
}

void od_screens_whatnext(od_screens *s)
{
    od_scene_whatnext_begin(&s->wn);
    enter(s, OD_SCREEN_WHATNEXT, 0);
    /* PISTAS-B: the settled screen is built from the imported data on the same
     * tick (the caller draws right after entering). */
    od_hearth_screen(od_gfx_hearth_art(), s->display, OD_SCENE_WORDS);
}

void od_screens_boss(od_screens *s)
{
    enter(s, OD_SCREEN_BOSS, BOSS);
}

void od_screens_ending(od_screens *s)
{
    enter(s, OD_SCREEN_ENDING, ENDING);
}

/* ---- Boss combat scene (T1) -------------------------------------------
 * The game enters the combat mode when a WORLD 10 round is cleared with the
 * whole world's bit pattern set. The entry then clears the two object tables,
 * loads the arena, puts Hero in her start cell and the objective counter at 1, and
 * arms Hero's entry sequence. The two object controllers
 * (`od_boss`/`od_boss_attack`) are the verified translation. The entry hold is
 * 21 ticks in combat (the reference's first spawn pose lands at f25770, 21
 * frames after f25749), one more than a normal level load, because the entry
 * spends an extra frame arming the sequence. */
#define OD_COMBAT_ENTRY_HOLD 21
#define OD_COMBAT_LEVEL 99 /* ROUND 10-10 */

void od_combat_load_board(od_game *g)
{
    const uint8_t *board = boss_arena();
    for (unsigned y = 0; y < OD_GRID_H; ++y)
        for (unsigned x = 0; x < OD_GRID_W; ++x)
            g->cells[y][x] = board[y * 16 + x];
}

void od_combat_init(od_combat *c, od_game *g)
{
    const uint8_t *board = boss_arena();
    od_level arena;
    memset(&arena, 0, sizeof arena);
    arena.player_position = 0x17; /* Hero's start cell (7,1) */
    for (unsigned y = 0; y < OD_GRID_H; ++y)
        for (unsigned x = 0; x < OD_GRID_W; ++x)
            arena.grid[y][x] = board[y * 16 + x];

    od_game_init_ex(g, &arena, NULL, OD_COMBAT_LEVEL);
    /* The entry sets the arena counter to one; the puzzle's `recite_fires` would see
     * zero and immediately "clear" the room. The combat room is not solved by
     * counting fires, so the latch is kept at the initialised value. */
    g->fires = 1;
    g->start_fires = 1;
    g->cleared = 0;
    g->exit_open = 0;
    g->entry_hold = OD_COMBAT_ENTRY_HOLD; /* combat's one extra frame */

    memcpy(c->grid, board, sizeof c->grid);
    memset(&c->boss, 0, sizeof c->boss);
    memset(&c->attack, 0, sizeof c->attack);
    c->have_attack = 0;
    c->active = 1;
    c->defeated = 0;
    c->victory = 0;
    /* The arena re-runs Hero's entry program; its start cue belongs to
     * the core here (the flow's level-start agenda does not cover combat). */
    g->entry_cue = 1;
}

void od_combat_tick(od_combat *c, od_game *g, unsigned buttons)
{
    /* Original intra-frame order: the attack slot 1 runs before the main slot 0,
     * and the main controller's spawn opcode registers the attack in the same
     * frame it raises the spawn event; the attack's own init then runs on the
     * following frame.
     *
     * The attack carves a tile into the live grid, the very board Hero walks on.
     * The native `od_game` keeps the 15 play rows; the 256-byte view of the grid
     * is rebuilt here so the original's row-15 access stays valid. */
    uint8_t hero_flags = 0, sound = 0;
    uint8_t grid[256];
    memcpy(grid, g->cells, OD_GRID_CELLS);
    memset(grid + OD_GRID_CELLS, 0, sizeof grid - OD_GRID_CELLS);
    /* The object slots (Boss and its attack) run only while no player-state flag
     * (death, pause, block in flight) is set: Hero's death freezes both, measured
     * on the arena route (attack state unchanged from f25960 and from f27925). The
     * START pause is in the same test. After the final boss falls the slots are empty:
     * nothing runs any more. */
    uint8_t game03 = (uint8_t)((g->dead ? 0x08 : 0) | (g->objects_paused ? 0x02 : 0));
    int objects = !game03 && !c->defeated;
    int was_active = (c->have_attack && c->attack.alive) ? 1 : 0;
    if (c->have_attack && objects)
        od_boss_attack_tick(&c->attack, grid, (uint8_t)g->hero_px,
                             (uint8_t)g->hero_py, &hero_flags, &sound);
    /* The attack (slot 1) runs first: the main controller (slot 0) reads the
     * attack's state *after* it has run, so the frame the attack retires the boss
     * resumes immediately instead of waiting one more tick. */
    uint8_t active = (c->have_attack && c->attack.alive) ? 1 : 0;
    memcpy(g->cells, grid, OD_GRID_CELLS);
    memcpy(c->grid, grid, sizeof c->grid);
    int retired = was_active && !active;
    /* Bit 0 of the object flag is set by the attack's init (state[0]=1) and cleared
     * by its retirement, which raises the round-clear flag instead. The object loop
     * runs before this frame's decision task: an A on the init frame f26191 starts
     * nothing and the HUD clock stops that same frame. The retirement frame keeps
     * the gate, as the round-clear flag holds the decision there. The first frame
     * after closing the pause runs with bit 0 clear (`r90_resume`: reference
     * f26301). */
    g->attack_lock = (active && c->attack.state[0] && !g->r90_resume) || retired;
    /* After the defeat the slots are empty: no object runs any more and the last
     * poses stay drawn until the ending (reference f26479..f26622). */
    uint8_t boss_sound = 0;
    if (!c->defeated) {
        od_boss_tick(&c->boss, game03, active);
        /* The main controller's own requests: a hit sound on hits 1..4 and the defeat
         * sequence's crumbling cue - reference f25800/25920/26040/
         * 26160 and f26371 on the injected route. */
        boss_sound = c->boss.sound;
        if (c->boss.spawn_attack) {
            od_boss_attack_spawn(&c->attack);
            c->have_attack = 1;
        }
        if (c->boss.defeated) {
            /* Clear the object table and decrement the objective. With the objective at
             * zero Hero's decision of this same frame starts the celebration
             * (reference: objective 0 and the celebration state both at f26479). */
            c->defeated = 1;
            c->have_attack = 0;
            g->fires = 0;
            od_game_update_clear(g);
        }
    }
    /* Bit 4 of the object flag, from the attack's marker, is a lethal contact
     * for Hero. The object loop runs before this frame's decision task, which
     * starts the death program from it: reference f26210 shows the carve, the
     * flag set and the death state in the same frame, Hero still at her pre-step
     * pixel. `lethal` is a latch the step does not clear, so it is published here,
     * before the step. */
    if (hero_flags & 0x10)
        od_game_lethal_contact(g);
    /* The retirement raises the round-clear flag in this object pass, before the
     * decision task: on the retirement frame a latched lethal contact cannot start
     * the death (it needs the flag clear). The spans themselves enter the support
     * queue after the step (below), where the cascade keeps the gate shut. */
    g->retire_freeze = retired;
    /* The attack's spawn cue reaches the shared sound queue after the
     * gameplay step, because that step clears the queue. */
    od_game_tick(g, buttons);
    g->retire_freeze = 0;
    /* The retirement's spans enter the support queue after this frame's support
     * step: the reference shows them, with the cascade state set, at the end of
     * the retirement frame f26378, clears that state on f26379 and drops the first
     * candidate on f26380. Queued before the gameplay step, the cascade ran one
     * tick early. The spans are not the cells the attack touched: the retirement
     * appends five fixed spans, in order (reference queue = 78 01 7E 01 56 01 47 02
     * 4B 03). */
    if (retired) {
        uint8_t cells[OD_BOSS_RETIRE_SPANS], widths[OD_BOSS_RETIRE_SPANS];
        od_boss_attack_retire_spans(cells, widths);
        for (unsigned i = 0; i < OD_BOSS_RETIRE_SPANS; ++i)
            od_game_support_changed_now(g, cells[i], widths[i]);
    }
    /* The ice engine sets the attack-hit marker in the frame of the hit,
     * after the object task, so the end-of-frame RAM already holds it (reference
     * f27784/27839/27884/27919/28887 on the played route); the main controller's
     * phase 1 reads and clears it at the top of the next frame. Nothing reads it in
     * between (the attack slot runs first and does not touch it). */
    if (g->boss_hit)
        c->boss.state[1] |= 0x80;
    od_game_sound_event(g, sound);
    od_game_sound_event(g, boss_sound);
    if (!c->defeated && g->fires != 1) {
        g->fires = 1;
        g->cleared = 0;
        g->exit_open = 0;
    }
    /* The fifth ice hit on the boss runs the defeat sequence; its end empties the objects
     * and the objective (above), Hero celebrates like a solved round and the same
     * exit tail hands over: the ending scene takes the screen at reference f26622,
     * 143 frames after the defeat (H4.11: f24339 -> f24482). */
    if (c->defeated && g->level_exit)
        c->victory = 1;
}

void od_retry_reset(od_retry_menu *m)
{
    memset(m, 0, sizeof *m);
}

/* One bit per button: the press edge sets the bit and loads the repeat counter
 * with 24; while held, the bit is clear until the counter runs out, then set
 * with the counter reloaded to 4 (measured: LEFT pressed at reference f3300
 * sets the LEFT bit at f3300, f3324, f3328, ...). */
void od_retry_input(od_retry_menu *m, unsigned held, unsigned pressed)
{
    for (unsigned i = 0; i < 8; ++i) {
        unsigned bit = 1u << i;
        if (pressed & bit) {
            m->repeat |= bit;
            m->counters[i] = 0x18;
        } else if (held & bit) {
            if (m->counters[i] == 0 || --m->counters[i] == 0) {
                m->repeat |= bit;
                m->counters[i] = 4;
            } else {
                m->repeat &= ~bit;
            }
        } else {
            m->repeat &= ~bit;
        }
    }
}

/* A first, then RIGHT before LEFT; each move requests the menu sound and wraps
 * over the two options. */
int od_retry_select(od_retry_menu *m, int *new_game, int *moved)
{
    if (moved) *moved = 0;
    if (m->repeat & OD_BTN_A) {
        if (new_game) *new_game = (int)m->selection;
        return 1;
    }
    if (m->repeat & OD_BTN_RIGHT) {
        m->selection = m->selection ? 0u : 1u;
        if (moved) *moved = 1;
    } else if (m->repeat & OD_BTN_LEFT) {
        m->selection = m->selection ? 0u : 1u;
        if (moved) *moved = 1;
    }
    return 0;
}

int od_retry_tick(od_retry_menu *m, unsigned buttons, int *new_game)
{
    /* Publish the previous selection first: the original's PPU write shows one
     * tick late, so the frame the direction is read still draws the old cursor
     * (measured: RIGHT at reference f6300 moves the arrow at f6301). */
    m->visible_selection = m->selection;
    od_retry_input(m, buttons, buttons & ~m->held);
    m->held = buttons;
    return od_retry_select(m, new_game, NULL);
}

/* Measured level-entry chain (D1). The reference raises its level dispatch 15
 * ticks after the round-focus `A` on every route (menu f2215, password 1-2
 * f14515, 5-6 f16515, 10-10 f21353). The scene clock publishes `play` one tick
 * after it enters the scene, so the LEVEL_ENTRY scene releases at this frame. */
#define OD_LEVEL_ENTRY_FRAMES 13u
/* The round-10 castle cinematic of worlds 1..9 before the level
 * dispatch. The reference keeps the WORLD map for 14 ticks after the
 * round-focus `A` on every world (8-10: f18001..f18014, castle clip at f18015), so
 * the native plays the ordinary entry clip (the measured map fade-out) for these
 * lead-in ticks and then switches to the world's recorded clip
 * (content/base/screens/tracks/castle-<N>.track, the clip window plus its
 * map frame). Replaces the old 1.267-tick retention. */
#define OD_CASTLE_ENTRY_DELAY 14u
/* The title's input loop starts after the 129-tick setup and lasts while the
 * countdown loaded with 660 runs. */
#define OD_TITLE_LOOP_AGE 129u
#define OD_TITLE_LOOP_FRAMES 660u

static void schedule(od_screens *s, od_screen_id next)
{
    s->next = next;
    s->pending = 1;
}

void od_screens_hidden_return(od_screens *s, int level)
{
    enter(s, OD_SCREEN_HIDDEN, TITLE);
    /* The flow calls this on the frame the hidden menu starts, outside the scene
     * tick: keep the scheduled entries' convention (age one past the frame
     * when it is drawn) so the menu draws and cues on the same frames. */
    s->age = 1;
    s->hidden_partial = 0;
    od_hidden_scene_reset(&s->hidden);
    s->hidden.open = 1;
    s->hidden.menu.active = 1;
    s->hidden.menu.level = level;
}

void od_screens_hidden_solved(od_screens *s, int level)
{
    int next = level + 1;
    if (next < OD_HIDDEN_FIRST || next >= OD_LEVEL_COUNT)
        next = OD_HIDDEN_FIRST;   /* 15-10: not measured; the menu's A wrap */
    od_screens_hidden_return(s, next);
    s->hidden_partial = 2;
    s->hidden_stale_level = level;
}

/* The colour counter step shared with the editor: 0, 1, 2, then back to 0. The
 * editor's colour cycle leaves it anywhere in 0..6 (wrap at 7), so a value of 3
 * or more goes back to 0 here, not to its remainder. */
static uint8_t frame_cycle_next(uint8_t x)
{
    return (uint8_t)(x + 1u >= 3u ? 0u : x + 1u);
}

/* The menu init (mode 3) on the frame it is requested: B on the map's world
 * focus and the editor's EXIT. Its cursor starts on the first option; the scene
 * follows on the next tick with the hearth one frame ahead (`menu_lead`),
 * "What next?" with progress, "Where to begin from?" otherwise. */
void od_screens_world_clear(od_screens *s, int world_index)
{
    s->world_clear_world = world_index >= 0 && world_index <= 8 ? (unsigned)world_index : 9u;
    s->world_clear_part = 0;
    s->world_clear_skip = 0;
    s->world_clear_seen = 0;
    s->world_clear_drawn = -1;
    s->world_clear_to_map = 0;
    schedule(s, OD_SCREEN_WORLD_CLEAR);
}

void od_screens_castle_appears(od_screens *s, int world_index)
{
    s->world_clear_world = world_index >= 0 && world_index <= 9 ? (unsigned)world_index : 0u;
    s->world_clear_part = 2;
    s->world_clear_skip = 0;
    s->world_clear_seen = 0;
    s->world_clear_drawn = -1;
    s->world_clear_to_map = 0;
    schedule(s, OD_SCREEN_WORLD_CLEAR);
}

const od_castle_info *od_screens_world_clear_info(const od_screens *s)
{
    if (s->world_clear_part == 2)
        return &s->cinematic_info[s->world_clear_world<=9?s->world_clear_world:0];
    return od_gfx_world_clear(s->world_clear_part ? OD_WORLD_CLEAR_CASTLE_OPEN
                                                   : (int)s->world_clear_world);
}

void od_screens_menu_init(od_screens *s, int progress)
{
    s->selection = s->visible_selection = 0;
    s->menu_lead = 1;
    /* "Where to begin from?" is the title route's MENU clip, whose mode 3 is later
     * in it: after the map's B and after EXIT alike the picture lines up two clip
     * ticks in (B: reference fade 1..15 at f1806..f1820 after the B of f1800). */
    if (!progress)
        s->menu_skip = OD_MENU_CLIP_SKIP;
    schedule(s, progress ? OD_SCREEN_WHATNEXT : OD_SCREEN_MENU);
}

static void apply(od_screens *s)
{
    const od_scene_track *t = &s->tracks[s->track];
    if((s->track>=CASTLE_APPEARS_W1 && s->track<=CASTLE_APPEARS_W10) ||
       s->track==INTRO || s->track==INTRO_REPEAT || s->track==CONVERSATION) {
        if(s->track_tick<t->duration) {
            if(od_cinematic_tick(&s->cinematic,s->display)!=OD_OK)
                memset(s->display,0,sizeof s->display);
            ++s->track_tick;
        }
        return;
    }
    if (!t->duration) {
        ++s->track_tick; /* a logic scene: only its clock advances */
        return;
    }
    if (s->track_tick >= t->duration) {
        if (t->loop < 0) return;
        /* Replay the sparse prefix to reconstruct the loop's initial layout;
         * this is a scene-local loop, not a global frame-number lookup. */
        s->track_tick = (unsigned)t->loop;
        s->event = 0;
        memset(s->display,0,sizeof s->display);
    }
    while (s->event < t->count && t->changes[s->event].tick <= s->track_tick) {
        const od_scene_change *c = &t->changes[s->event++];
        s->display[c->field] = c->value;
    }
    ++s->track_tick;
}

/* ---- C4.2 PISTAS-A: the interactive screens as logic over their layouts ---
 *
 * The clips these screens used to replay are kept as reference captures, private
 * since C6.18; an extraction step reduces them to the static picture each one
 * settles on (screens/layouts.json). What changes on screen is computed here,
 * frame by frame, from the original's rules. `track_tick` stays the scene
 * clock: on each tick the display shows its frame `T`, then the clock advances
 * (the clips' own timeline). */

/* Brightness `b` (0..15) over a display palette. The sprite palettes' entry 0
 * (16, 20, 24, 28) is never drawn; it keeps the layout's value as the
 * reference's palette RAM does. */
static void layout_fade(uint16_t *display, int b)
{
    const uint8_t *lut = od_palette_fade_lut();
    if (!lut || b >= 15)
        return;
    if (b < 0)
        b = 0;
    for (unsigned i = 0; i < 32; ++i) {
        if (i >= 16 && (i & 3u) == 0)
            continue;
        unsigned base = display[i] & 0x3fu;
        display[i] = (uint16_t)(b == 0 ? 0x0fu
                                       : ((base & 15u) | lut[(base & 0x30u) + (unsigned)b]));
    }
}

/* An actual staged upload producer. Every phase performs its operation;
 * publication never comes from track_tick or an assigned reference age.
 * Existing fade/hearth mechanisms are reused, while the 14 record batches are
 * derived from their byte lengths. */
static void password_record_write(const od_menu_record *r,uint16_t *display)
{
    /* E145/E186 destination semantics span BOTH nametables, unlike the
     * hearth menu overlay helper restricted to NT0. C6.2 validates ranges. */
    unsigned address=r->addr,step=(r->count&0x80u)?32u:1u;
    for(unsigned i=0;i<(r->count&63u);++i,address+=step)
        display[32+address-0x2000u]=r->data[i];
}

static void password_oam_allocate(uint16_t *display,unsigned mode)
{
    /* The field is submitted, then the choice; the slot allocator picks
     * forward/backward physical slots, including the half-page start. Compact
     * visible sprites by ascending slot, not a captured swap table. */
    uint16_t widgets[2][4]; memcpy(widgets,display+2125,sizeof widgets);
    int slots[64]; for(unsigned i=0;i<64;++i) slots[i]=-1;
    unsigned cursor=(mode&2u)?32u:0u;
    for(unsigned i=0;i<2;++i) {
        unsigned slot=(mode&1u)?(--cursor&63u):(cursor++&63u);
        slots[slot]=(int)i;
    }
    memset(display+2125,0,256*sizeof(uint16_t)); display[2124]=0;
    for(unsigned slot=0;slot<64;++slot) if(slots[slot]>=0) {
        memcpy(display+2125+4*display[2124],widgets[slots[slot]],4*sizeof(uint16_t));
        ++display[2124];
    }
}

static void password_task(od_screens *s)
{
    const od_interactive_setup *art=od_interactive_setup_get();
    const od_password_art *messages=od_gfx_password_art();
    memset(&s->password_sounds,0,sizeof s->password_sounds);
    s->password_sounds.managed=1;
    if(!art || !messages) { s->password_phase=PW_FAILED; s->display[2092]=0; return; }
    ++s->track_tick; /* informational clock, never a task/input condition */
    switch(s->password_phase) {
    case PW_CLOSE:
        od_hearth_patch(&s->hearth,od_gfx_hearth_art(),s->display,OD_SCENE_WORDS);
        if(!s->hearth_close) {
            memcpy(s->password_next,s->display,sizeof s->display);
            s->password_phase=PW_FADE;
        }
        break;
    case PW_FADE:
        memcpy(s->display,s->password_next,sizeof s->display);
        layout_fade(s->display,(int)s->password_brightness);
        if(!s->password_brightness) s->password_phase=PW_CLEAR;
        else --s->password_brightness;
        break;
    case PW_CLEAR:
        /* E3E4 clears both NT/attributes/scroll; E1BF hides all OAM.
         * F2DE(0) disables split and clears CHR-mode D3, not a band capture. */
        memset(s->password_next,0,sizeof s->password_next);
        for(unsigned i=0;i<16;++i) s->password_next[i]=15;
        for(unsigned i=16;i<32;++i) s->password_next[i]=(i&3u)?15:0;
        s->password_next[2093]=1;
        memcpy(s->display,s->password_next,sizeof s->display);
        s->password_phase=PW_CHR;
        break;
    case PW_CHR:
        for(unsigned i=0;i<4;++i)
            s->password_next[2080+i]=s->password_next[2084+i]=(uint16_t)art->password_chr[i+2]*64;
        for(unsigned i=0;i<2;++i)
            s->password_next[2088+i]=s->password_next[2090+i]=(uint16_t)art->password_chr[i]*64;
        memcpy(s->display,s->password_next,sizeof s->display);
        s->password_phase=PW_PALETTE;
        break;
    case PW_PALETTE:
        for(unsigned i=0;i<32;++i) s->password_next[i]=art->password_palette_buffer[i];
        for(unsigned i=0;i<16;i+=4) s->password_next[i]=art->password_palette_buffer[16];
        for(unsigned i=16;i<32;i+=4) s->password_next[i]=0;
        s->password_next[2092]=30;
        /* EC58: request1B immediately before E67F full-brightness palette.
         * No invented fade-in, fallback sound or elapsed78 agenda. */
        s->password_sounds.ids[0]=27; s->password_sounds.count=1;
        memcpy(s->display,s->password_next,sizeof s->display);
        s->password_phase=PW_UPLOAD;
        break;
    case PW_UPLOAD: {
        memcpy(s->display,s->password_next,sizeof s->display);
        unsigned queued=0;
        while(s->password_record<14) {
            unsigned i=s->password_record;
            const od_menu_record *r=i<4?&art->password_field_frame[i]:
                i<13 && i>4?&art->password_choice_panel[i-5]:&messages->prompt;
            unsigned n=i==4?26u:(r->count&63u);
            if(queued+n>=60) break; /* E186 checks before the 3 header bytes */
            if(i==4) {
                od_scene_password_panel(NULL,0,0,s->password_next,OD_SCENE_WORDS);
                s->password_next[2124]=0; memset(s->password_next+2125,0,256*sizeof(uint16_t));
            } else password_record_write(r,s->password_next);
            queued+=3+n; ++s->password_record;
        }
        if(s->password_record==14) s->password_phase=PW_PUBLISH;
        break;
    }
    case PW_PUBLISH:
        /* EC7A E803/E870 after 804B setup returns advances allocation once;
         * the next 8308 input pass produces the first cursor OAM. */
        s->hearth.rotation=(s->hearth.rotation+1u)&3u;
        s->hearth.drawn_rotation=s->hearth.rotation;
        od_scene_password_panel(NULL,0,0,s->password_next,OD_SCENE_WORDS);
        s->password_next[2125]=art->password_field_cursor_x[0];
        s->password_next[2129]=art->password_palette_cursor_oam_yx[0][1];
        s->password_next[2130]=art->password_palette_cursor_oam_yx[0][0]+1;
        password_oam_allocate(s->password_next,s->hearth.drawn_rotation);
        memcpy(s->display,s->password_next,sizeof s->display);
        s->hearth.rotation=(s->hearth.rotation+1u)&3u; /* E803 cursor pass return */
        s->password_phase=PW_READY;
        break;
    case PW_READY:
        s->hearth.drawn_rotation=s->hearth.rotation;
        s->hearth.rotation=(s->hearth.rotation+1u)&3u;
        break;
    default: break;
    }
}

/* Native init jobs advance only after their own publication; no age/track
 * schedule is consulted, including the editor-mode return from a level. */
static void editor_task(od_screens *s,unsigned pressed)
{
    const od_interactive_setup *art=od_interactive_setup_get();
    memset(&s->editor_sounds,0,sizeof s->editor_sounds); s->editor_sounds.managed=1;
    if(!editor_setup_complete(art) || s->editor_status!=OD_OK) goto failed;
    ++s->track_tick; /* diagnostic clock only */
    switch(s->editor_phase) {
    case ED_CLOSE:
        od_hearth_patch(&s->hearth,od_gfx_hearth_art(),s->display,OD_SCENE_WORDS);
        if(s->hearth_close) break;
        memcpy(s->editor_next,s->display,sizeof s->display);
        s->editor_phase=ED_FADE_OUT;
        /* C6.31: the fade's first step is drawn on the frame the close ends
         * (reference R7 f1764 is the product's f1765 before; the hearth close
         * itself was already frame-exact). */
        /* fall through */
    case ED_FADE_OUT:
        memcpy(s->display,s->editor_next,sizeof s->display);
        layout_fade(s->display,(int)s->editor_brightness);
        if(s->editor_brightness) { --s->editor_brightness; break; }
        s->editor_phase=ED_PROGRAM;
        /* C6.31: the editor entry starts on the fade's black frame, A+75 on both routes
         * (reference f1775 / f12275: its `1` there, song 57 two frames later, the
         * first batch counter = 31 at f1777 / f12277). */
        /* fall through */
    case ED_PROGRAM: {
        if(od_cinematic_tick(&s->cinematic,s->display)!=OD_OK) goto failed;
        if((pressed&OD_BTN_START) && s->cinematic.skip_enabled &&
           od_cinematic_skip(&s->cinematic)!=OD_OK) goto failed;
        if(od_cinematic_take_sounds(&s->cinematic,&s->editor_sounds)!=OD_OK) goto failed;
        if(!s->cinematic.finished) break;
        /* C6.31: the program's last batch (counter at its end value, reference f12559) is followed
         * by two editor-exit frames (f12560, f12561), and the clear lands on the second;
         * the next mode from f12562. The first fade step of the init then shows at f12571
         * like the reference. */
        s->editor_phase=ED_EXIT;
        break;
    }
    case ED_EXIT:
        s->editor_phase=ED_CLEAR;
        break;
    case ED_CLEAR:
        editor_clear_job(s);
        break;
    case ED_CHR:
        for(unsigned i=0;i<4;++i) s->editor_next[2080+i]=s->editor_next[2084+i]=(uint16_t)art->editor_chr[i+2]*64;
        for(unsigned i=0;i<2;++i) s->editor_next[2088+i]=s->editor_next[2090+i]=(uint16_t)art->editor_chr[i]*64;
        memcpy(s->display,s->editor_next,sizeof s->display);
        s->editor_phase=ED_UPLOAD;
        /* EE21 branches directly to EE29/E5BE. CHR has no separate yield. */
        /* fall through */
    case ED_UPLOAD: {
        memcpy(s->display,s->editor_next,sizeof s->display);
        unsigned queued=0;
        while(s->editor_record<27) {
            const od_menu_record *r=&art->editor_initial_layout[s->editor_record];
            unsigned n=r->count&63u;
            if(queued+n>=60) break; /* E186 comparison BEFORE 3 header bytes */
            password_record_write(r,s->editor_next); queued+=3+n; ++s->editor_record;
        }
        if(s->editor_record==27) s->editor_phase=ED_LAYOUT_PUBLISH;
        break;
    }
    case ED_LAYOUT_PUBLISH:
        memcpy(s->display,s->editor_next,sizeof s->display);
        /* EE3A's final upload yield has completed; EE3F requests1E once. */
        s->editor_sounds.ids[0]=30; s->editor_sounds.count=1;
        for(unsigned i=0;i<32;++i) s->editor_next[i]=art->editor_palette_buffer[i];
        for(unsigned i=0;i<16;i+=4) s->editor_next[i]=art->editor_palette_buffer[16];
        for(unsigned i=16;i<32;i+=4) s->editor_next[i]=0;
        /* E5EA..E5FA increments D6/D7 before queuing the first fade step. */
        s->editor_next[2092]=30; s->editor_brightness=1;
        s->editor_phase=ED_FADE_IN;
        break;
    case ED_FADE_IN:
        memcpy(s->display,s->editor_next,sizeof s->display);
        layout_fade(s->display,(int)s->editor_brightness);
        if(s->editor_brightness<15) { ++s->editor_brightness; break; }
        /* The final palette has now published. E60B returns directly to
         * EE55 marker calls; their next real wait is EE67, not a task switch. */
        s->editor_phase=ED_MARKERS;
        /* fall through */
    case ED_MARKERS:
        /* EE55/827C/82B2: FIELD A and BLOCK B. Real marker addresses from
         * editor-data, not a captured rectangle or a pre-rendered viewport. */
        for(unsigned i=0;i<2;++i) {
            unsigned addr=od_editor_marker_addr[i][0]|(unsigned)od_editor_marker_addr[i][1]<<8;
            s->editor_next[32+addr-0x2000]=(uint16_t)(0xC1+i);
        }
        s->editor_phase=ED_ROWS;
        break;
    case ED_ROWS:
        memcpy(s->display,s->editor_next,sizeof s->display);
        if(s->editor_row<12) {
            memcpy(s->editor_next+32+0x102+32*s->editor_row,
                   s->editor_board[s->editor_row],14*sizeof(uint16_t));
            ++s->editor_row;
            break;
        }
        /* Last row's E0DD has returned: finish marker/OAM preparation in this
         * caller pass, rather than inserting an empty phase-only event. */
        s->editor_phase=ED_OAM;
        /* fall through */
    case ED_OAM:
        if(s->editor_start) {
            unsigned r=(s->editor_start>>4)-1,c=(s->editor_start&15)-1;
            s->editor_next[32+0x102+32*r+c]=0x9E; /* EU8471..84B6 marker after all12rows */
        }
        /* C6.31 (D6): the marker pass is its own upload frame. With a start
         * cell the marker shows on it (reference R7 f3952); without one the rows
         * just stay on screen (first-menu entry f2099, "What next?" f12599). */
        s->editor_phase=ED_LAST_PUBLISH;
        break;
    case ED_LAST_PUBLISH:
        memcpy(s->display,s->editor_next,sizeof s->display);
        /* EE85/8511 round0, EE92/E7CA/85FA first toolbar cursor. Compact
         * one physical OAM sprite; the allocation mode cannot reorder one. */
        /* The round number is two digits: tens (blank for 0), then ones. The tens
         * digit is uploaded at base+1 FIRST; the ones digit at base SECOND. Preserve
         * that queue destination/value pairing, not left-to-right. */
        s->editor_next[32+0xAC]=0x80; s->editor_next[32+0xAB]=0xB1;
        memset(s->editor_next+2125,0,256*sizeof(uint16_t));
        s->editor_next[2124]=1; s->editor_next[2125]=od_editor_toolbar_x[0];
        s->editor_next[2126]=24; s->editor_next[2127]=0x7E;
        /* C6.31: the round digit and the first toolbar cursor are written and the
         * editor loop reads its first input on this same frame (reference f2099 /
         * f12599 / R7 return f3952); the display shows those writes one frame later
         * (f2100 / f12600 / f3953), which the flow draws from the editor's own state
         * (flow.c, `evw.primed`). */
        s->editor_phase=ED_CURSOR;
        break;
    case ED_CURSOR:
        memcpy(s->display,s->editor_next,sizeof s->display);
        s->editor_phase=ED_READY;
        break;
    case ED_READY: break;
    default: goto failed;
    }
    return;
failed:
    s->editor_phase=ED_FAILED; s->editor_status=OD_ERR_FORMAT;
    s->editor_sounds.count=0; s->editor_sounds.overflow=1; s->display[2092]=0;
}

/* The title: the settled picture with the setup's palette keyframes (black, the
 * fade-in, the loop palette at tick 129), the sprite banks the setup writes at
 * tick 127, the banks of its last tick, and palette entry 15 as the loop last
 * painted it (shown the frame after the paint; see the TITLE case). The hidden
 * menu's scene keeps the same display behind its own drawing. */
static void title_frame(od_screens *s)
{
    /* screens already counted this frame; cinematic_tick advances its standalone
     * counter before the title loop's `counter & 3` test. */
    s->cinematic.frame_tick=s->frame_tick-1u;
    (void)od_cinematic_tick(&s->cinematic,s->display);
    ++s->track_tick;
}

/* C6.18 genuine package: `screens/layouts.json` v2 carries only the two map
 * bases, so `layouts()` is NULL and there is no captured menu display. The menu
 * art is nevertheless game data already in the package: `screens/hearth.json`
 * "screen" is the setup rectangle (the illustration, its attribute block, the
 * palettes and the screen's PPU words) and `layouts.begin` is the "Where to
 * begin from?" panel. Rebuild the settled menu from them exactly as
 * `od_hearth_screen` does for "What next?"; the option cursor (arrow tile at nametable
 * row 19, column 17, as the capture had it) is the first option, the only one
 * whose A leads to the close. */
#define OD_MENU_CURSOR_WORD (32u + 19u * 32u + 17u)
static int menu_base_from_rom(const od_hearth_art *hart, uint16_t *out)
{
    if (!hart || !hart->has_screen || !out)
        return 0;
    for (unsigned i = 0; i < 32; ++i)
        out[i] = hart->palette[i];
    for (unsigned r = 0; r < 16; ++r)
        for (unsigned c = 0; c < 16; ++c) {
            unsigned y = hart->illus_y + r, x = hart->illus_x + c;
            if (y < 30 && x < 32)
                out[32 + y * 32 + x] = hart->illus[r][c];
        }
    /* Both nametables' attribute tables start filled; the illustration's own
     * 4x4 block goes into the first one (as od_hearth_screen). */
    for (unsigned nt = 0; nt < 2; ++nt)
        for (unsigned i = 0; i < 64; ++i)
            out[32 + nt * 1024 + 960 + i] = hart->attr_fill;
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c) {
            unsigned y = hart->attr_y + r, x = hart->attr_x + c;
            if (y < 8 && x < 8)
                out[32 + 960 + y * 8 + x] = hart->attr[r][c];
        }
    for (unsigned i = 0; i < hart->ppu_count; ++i)
        if (hart->ppu_field[i] < OD_SCENE_WORDS)
            out[hart->ppu_field[i]] = hart->ppu_value[i];
    const od_menu_layout *l = &hart->layouts[OD_MENU_LAYOUT_BEGIN];
    for (unsigned i = 0; i < l->record_count; ++i)
        od_menu_record_write(&l->records[i], out, OD_SCENE_WORDS);
    out[OD_MENU_CURSOR_WORD] = 0xfeu;
    return 1;
}

/* The static base of the menu: the captured v1 layout when the package has it,
 * otherwise the imported one. Returns 0 when neither exists. */
static int menu_base(od_screens *s)
{
    const od_screen_layouts *l = layouts();
    if (l) {
        memcpy(s->display, l->menu, sizeof s->display);
        return 1;
    }
    return menu_base_from_rom(od_gfx_hearth_art(), s->display);
}

/* The static base of the close: the menu's last picture. The v1 capture has
 * the cursor already; the imported base draws it too, so both are equivalent. */
static int menu_close_base(od_screens *s)
{
    const od_screen_layouts *l = layouts();
    if (l) {
        memcpy(s->display, l->menu_close, sizeof s->display);
        return 1;
    }
    return menu_base_from_rom(od_gfx_hearth_art(), s->display);
}

/* "Where to begin from?" (the loop shared with "What next?"): the settled menu,
 * faded in from the reference's mode-3 frame + 6, its text uploaded through the
 * VRAM queue once the fade is done, and the hearth's last animation call (OAM
 * and the sprite bank). The clip tick `T` is the menu frame + 1 (`menu_skip`
 * lines the B and EXIT entries up two ticks in, exactly as the clip did). */
static void menu_frame(od_screens *s)
{
    const int frame = (int)s->track_tick++ - 1;
    const od_hearth_art *hart = od_gfx_hearth_art();
    if (!menu_base(s)) {
        memset(s->display, 0, sizeof s->display);
        return;
    }
    if (hart)
        od_menu_layout_draw(&hart->layouts[OD_MENU_LAYOUT_BEGIN],
                             hart->queue_capacity, frame, s->display,
                             OD_SCENE_WORDS);
    layout_fade(s->display, od_menu_entry_brightness(frame));
    od_hearth_patch(&s->hearth, hart, s->display, OD_SCENE_WORDS);
}

/* The menu's close after "beginning" (60 frames of the hearth's close
 * animation, the faces changed, then the fade): the menu's last picture, the
 * hearth's close calls and the 15-frame fade to black that ends on the close's
 * last frame, whose length the layout carries (75: the map opens on the next).
 * The 75 is 60 calls plus the 15-frame fade, not a capture. */
#define OD_MENU_CLOSE_FRAMES 75u
static unsigned menu_close_frames(void);
static void menu_close_frame(od_screens *s)
{
    const unsigned t = s->track_tick++;
    if (!menu_close_base(s)) {
        memset(s->display, 0, sizeof s->display);
        return;
    }
    od_hearth_patch(&s->hearth, od_gfx_hearth_art(), s->display, OD_SCENE_WORDS);
    const int left = (int)menu_close_frames() - 1 - (int)t;
    layout_fade(s->display, left < 0 ? 0 : left);
}

static unsigned menu_close_frames(void)
{
    const od_screen_layouts *l = layouts();
    return l ? l->menu_close_frames : OD_MENU_CLOSE_FRAMES;
}

/* The fade of the accepted map: palette brightness `b` through the brightness
 * table, 14 - t on the entry's frame t. */
static void entry_fade(const od_screens *s, uint16_t *display, int b)
{
    const uint8_t *lut = od_palette_fade_lut();
    if (b < 0)
        b = 0;
    memcpy(display, s->entry_display, sizeof s->entry_display);
    for (unsigned i = 0; i < 32; ++i) {
        unsigned base = s->entry_display[i] & 0x3fu;
        display[i] = (uint16_t)(b == 0 ? 0x0fu
                                       : ((base & 15u) | lut[(base & 0x30u) + (unsigned)b]));
    }
}

void od_screens_load_hold(od_screens *s)
{
    /* The round load starts on the entry's frame 13, the dispatch one frame later:
     * it shows the fade's next step, brightness 14 - 13 = 1 (reference R1 f2214,
     * password 1-2 f14514), black from the next frame. A castle or the boss
     * sequence ends black, which stays on screen. */
    if (s->hold_entry_map && od_palette_fade_lut())
        entry_fade(s, s->hold_display, 14 - (int)OD_LEVEL_ENTRY_FRAMES);
}

void od_screens_draw_load_hold(const od_screens *s, od_index_frame *fb,
                                const uint8_t *chr)
{
    compose_display(s->hold_display, s->hold_scene, s->hold_age,
                    s->hold_selection, fb, chr);
}

void od_screens_tick(od_screens *s, unsigned buttons)
{
    unsigned pressed = buttons & ~s->previous_buttons;
    s->previous_buttons = buttons;
    /* Without the flow (scene tests and tools that drive the screens alone)
     * the frame counters are plain counters; the flow advances it itself with the
     * original's lost NMIs (flow_frame_tick). */
    if (!s->frame_tick_by_flow)
        ++s->frame_tick;
    /* SONIDO-MAPA: no castle clip frame is drawn unless the castle path below says
     * so. The play and map scenes return before it, and a stale tick kept
     * audio_castle re-requesting that tick's cue (`1`) on every map frame after NEW
     * GAME from an x-10. */
    s->castle_drawn = -1;
    if (s->pending) {
        od_screen_id next = s->next;
        unsigned track = INTRO;
        switch (next) {
        case OD_SCREEN_TITLE: track = s->status ? TITLE_SKIP : TITLE; break;
        case OD_SCREEN_INTRO: track = INTRO_REPEAT; break;
        case OD_SCREEN_CONVERSATION: track = CONVERSATION; break;
        case OD_SCREEN_MENU: track = MENU; break;
        case OD_SCREEN_MENU_CLOSE: track = MENU_CLOSE; break;
        case OD_SCREEN_MAP: track = MAP; break;
        case OD_SCREEN_ROUND_SELECT: track = ROUND_SELECT; break;
        case OD_SCREEN_LEVEL_ENTRY: track = LEVEL_ENTRY; break;
        case OD_SCREEN_PASSWORD: track = PASSWORD; break;
        case OD_SCREEN_EDITOR: track = EDITOR; break;
        case OD_SCREEN_BOSS: track = BOSS; break;
        case OD_SCREEN_ENDING: track = ENDING; break;
        case OD_SCREEN_WHATNEXT: /* PISTAS-B: built, no clip */
        case OD_SCREEN_HIDDEN: track = TITLE; break;
        case OD_SCREEN_GOODNIGHT: track = GOODNIGHT; break;
        case OD_SCREEN_HIDDEN_INTRO: track = HIDDEN_INTRO; break;
        case OD_SCREEN_WORLD_CLEAR:
            if (s->world_clear_part == 2)
                track = CASTLE_APPEARS_W1 + (s->world_clear_world <= 9 ? s->world_clear_world : 0u);
            else
                track = WORLD_CLEAR_W1 + (s->world_clear_world <= 8 ? s->world_clear_world : 0u);
            break;
        case OD_SCREEN_MAGICWORD: /* logic only: no clip */
        case OD_SCREEN_PLAY: case OD_SCREEN_UNVERIFIED: break;
        }
        /* MAPA-MUNDOS: the ROUND focus `A` fades the map it was accepted on, so keep
         * that display for the LEVEL_ENTRY scene. */
        /* CARGA-RONDA: the round load's first frame still shows the frame
         * this scene last drew (od_screens_load_hold). */
        if (next == OD_SCREEN_PLAY) {
            memcpy(s->hold_display, s->display, sizeof s->hold_display);
            s->hold_scene = s->scene;
            s->hold_age = s->age;
            s->hold_selection = s->visible_selection;
            /* A castle keeps the entry's mark but ends on its own sequence,
             * whose last frame is what stays on screen. */
            s->hold_entry_map = s->scene == OD_SCREEN_LEVEL_ENTRY &&
                                s->entry_from_map && !s->level_castle &&
                                s->track == LEVEL_ENTRY;
        }
        s->entry_from_map = 0;
        if (next == OD_SCREEN_LEVEL_ENTRY &&
            (s->scene == OD_SCREEN_MAP || s->scene == OD_SCREEN_ROUND_SELECT) &&
            od_gfx_map_art() && od_palette_fade_lut()) {
            memcpy(s->entry_display, s->display, sizeof s->entry_display);
            s->entry_from_map = 1;
        }
        /* PISTAS-A: the close continues the menu's hearth: its first frame
         * shows the call of the A frame (reference R1 f1401), so the menu's
         * calls survive the scene change. */
        const unsigned hearth_calls = s->hearth.calls;
        enter(s,next,track);
        if (next == OD_SCREEN_MENU_CLOSE)
            s->hearth.calls = hearth_calls;
        s->menu_blank = 0;
        if (next == OD_SCREEN_WHATNEXT) {
            od_scene_whatnext_begin(&s->wn); /* the map's B: cursor on cont.story */
            od_hearth_screen(od_gfx_hearth_art(), s->display, OD_SCENE_WORDS);
        }
        if (next == OD_SCREEN_MAGICWORD) {
            if(od_gfx_static_magicword()) {
                if(od_scene_magicword_begin_static(&s->mw,s->display)!=OD_OK){s->status=OD_ERR_FORMAT;return;}
            } else od_scene_magicword_begin(&s->mw,od_gfx_magicword_art());
        }
        if (next == OD_SCREEN_MENU && s->menu_skip) {
            s->track_tick = s->menu_skip;
            s->menu_skip = 0;
        }
        if (next == OD_SCREEN_EDITOR && s->editor_skip) {
            /* What next has already closed its own menu. Native entry starts
             * its program, not the recorded tick75. */
            s->editor_skip = 0;
        }
        if (next == OD_SCREEN_MAP) {
            if (!s->map_ready)
                s->map_ready = OD_MAP_REVEAL_FRAMES; /* the opening */
            map_begin(s);
            s->map_reveal = 1;
        }
    }
    if (s->scene == OD_SCREEN_PLAY || s->scene == OD_SCREEN_UNVERIFIED) {
        ++s->tick;
        return;
    }
    if (s->scene == OD_SCREEN_MAP || s->scene == OD_SCREEN_ROUND_SELECT) {
        /* C3.2: the WORLD map is logic, not a recorded clip. The WORLD->ROUND
         * focus change keeps the one-tick scene latch (the extracted
         * round-select scene starts one tick after the `A`); the return (B) is
         * immediate so it never clears the layout into a gray frame. */
        const int map_lag = od_gfx_map_art() != NULL;
        if (map_lag && !s->map_next_valid) {
            memcpy(s->map_next, s->display, sizeof s->map_next);
            s->map_next_valid = 1;
        }
        /* The opening reads no input through its loop init (the init runs in place of
         * the handlers): an A on the init frame is ignored and the next one is taken
         * (menu route f1475: A f1521 ignored, f1522 taken). */
        const unsigned lead = map_reveal_lead(s);
        if (s->map_reveal)
            s->map.ready = OD_MAP_REVEAL_FRAMES + 1u - lead;
        od_map_event ev = od_scene_map_tick(&s->map, buttons);
        /* MAPA-MUNDOS: the map loop's init (state 0) runs on the last frame of the
         * opening and draws its first sprite frame there; the OAM shows it from the next
         * one (R1: init f4037 = map age 46, sprites from f4038). A direct opening (no
         * reveal) starts when its input hold ends. */
        if (s->map.age + lead >= (s->map_reveal ? OD_MAP_REVEAL_PANEL_AGE - 1u
                                                : s->map.ready)) {
            /* The loop init (its first sprite frame) is where a map opened with the resume
             * flag resumes the ROUND focus. */
            if (s->map_resume && s->map.loop_calls == 0) {
                s->map_resume = 0;
                map_resume_round_focus(s);
            }
            if (od_scene_map_step_sprites(&s->map, od_gfx_map_art(), s->frame_tick))
                s->frame_cycle = frame_cycle_next(s->frame_cycle);
        }
        if (ev == OD_MAP_EVENT_ENTER) {
            s->level_castle =
                od_scene_round_castle(s->map.words, s->map.world,
                                       s->map.sel.round) ? 1u : 0u;
            s->castle_world = (unsigned)s->map.world;
            schedule(s, OD_SCREEN_LEVEL_ENTRY);
        } else if (ev == OD_MAP_EVENT_MENU) {
            /* Flag cleared and mode 3, the menu init; its cursor starts on the first option
             * (reference f1830 after the B of f1800). The init draws "What next?" when any
             * progress byte is set (1-1 solved, B f4400 -> "What next?" f4600) and
             * "Where to begin from?" otherwise. */
            int progress = 0;
            for (int w = 0; w < OD_MAP_WORLD_COUNT; ++w)
                progress |= s->map.words[w] != 0;
            od_screens_menu_init(s, progress);
            /* The B's frame is mode 3: the init blanks it (reference f1800,
             * f4400: black, only a stale sprite of the map lingers). */
            s->menu_blank = 1;
        } else if (s->map.focus == 1) {
            if (s->scene == OD_SCREEN_MAP)
                schedule(s, OD_SCREEN_ROUND_SELECT);
        } else {
            s->scene = OD_SCREEN_MAP;
        }
        map_refresh(s);
        if (map_lag) {
            /* Present the state of the previous tick; keep this one for the next display
             * pass (see `map_next`). */
            for (unsigned i = 0; i < OD_SCENE_WORDS; ++i) {
                uint16_t t = s->display[i];
                s->display[i] = s->map_next[i];
                s->map_next[i] = t;
            }
        }
        ++s->age;
        ++s->local_tick;
        ++s->tick;
        return;
    }
    /* C3.2/A8 CASTILLOS: a round-10 castle plays the ordinary entry clip for
     * the measured lead-in (the reference's fading map, 14 ticks on every world),
     * then switches to the world's recorded clip. The switch happens before
     * `apply()` so the clip's first frame is drawn on the same tick the reference
     * first raises the castle mode. */
    if (s->scene == OD_SCREEN_LEVEL_ENTRY && s->level_castle &&
        s->track == LEVEL_ENTRY && s->track_tick >= OD_CASTLE_ENTRY_DELAY) {
        if(s->castle_world>=9) {s->castle_round10_status=OD_ERR_FORMAT;s->status=OD_ERR_FORMAT;return;}
        s->track = CASTLE_W1 + s->castle_world;
        s->track_tick = 0;
        s->event = 0;
        od_cinematic_begin(&s->cinematic,s->castle_round10_data,s->castle_world);
        /* The cinematic enters from the just-faded live map. Its init has not yet
         * cleared NT/OAM/CHR: retain our actual publisher, never a captured viewport or
         * a prebuilt initial frame. ENTRY_PALETTE clears next. */
        for(unsigned i=0;i<2048;++i)s->cinematic.tiles[i]=(uint8_t)s->display[32+i];
        memcpy(s->cinematic.pending_oam,s->display+2124,sizeof s->cinematic.pending_oam);
        for(unsigned i=0;i<4;++i){s->cinematic.bg_banks[i]=(uint8_t)(s->display[2080+i]/64);s->cinematic.split_banks[i]=(uint8_t)(s->display[2084+i]/64);}
        for(unsigned i=0;i<2;++i)s->cinematic.sprite_banks[i]=(uint8_t)(s->display[2088+i]/64);
        s->castle_round10_phase=1;s->castle_round10_status=OD_OK;
        memset(s->display, 0, sizeof s->display);
    }
    /* FIN-DE-MUNDO: world 9's end runs into the castle opening on
     * the frame after its own last map frame (reference f24251 -> clip again
     * f24252). */
    if (s->scene == OD_SCREEN_WORLD_CLEAR && s->world_clear_part == 0 &&
        s->world_clear_world == 8 && !s->world_clear_skip && s->world_clear_phase==3) {
        s->world_clear_part = 1;
        s->world_clear_seen = 0;
        s->track = CASTLE_OPEN;
        s->track_tick = 0;
        s->event = 0;
        world_clear_begin(s);
    }
    if (s->scene == OD_SCREEN_WORLD_CLEAR && s->world_clear_skip && s->world_clear_part==2) {
        /* After a START skip the clip stops: the last map frame and the menu
         * frame are black (reference START f18000, f18001-f18002 black). */
    } else if (s->scene == OD_SCREEN_LEVEL_ENTRY && s->track == LEVEL_ENTRY &&
        s->entry_from_map) {
        /* MAPA-MUNDOS: the fade on the accepted map (any world, the castle map
         * included) instead of the recorded world-1 clip. The clip's only changes after
         * its first frame are this fade: palette brightness 14 - t through the
         * brightness table (clip ticks 3, 7, 9 and 11 are the steps), the sprites
         * unchanged. */
        entry_fade(s, s->display, 14 - (int)s->track_tick);
        ++s->track_tick;
    } else if (s->scene != OD_SCREEN_MAGICWORD &&
               s->scene != OD_SCREEN_WHATNEXT) {
        /* "Today's magicword" draws its own display below; "What next?" was
         * built on entry (PISTAS-B). */
        if (s->scene == OD_SCREEN_TITLE || s->scene == OD_SCREEN_HIDDEN)
            title_frame(s);
        else if (s->scene == OD_SCREEN_MENU)
            menu_frame(s);
        else if (s->scene == OD_SCREEN_MENU_CLOSE)
            menu_close_frame(s);
        else if(s->scene==OD_SCREEN_PASSWORD)
            password_task(s);
        else if(s->scene==OD_SCREEN_EDITOR)
              editor_task(s,pressed);
        else if(s->post_ending_phase) {
            if(s->post_ending_phase==4){s->status=OD_ERR_FORMAT;return;}
            if(od_cinematic_tick(&s->cinematic,s->display)!=OD_OK) {
                s->post_ending_status=OD_ERR_FORMAT;s->post_ending_phase=4;s->status=OD_ERR_FORMAT;return;
            }
            ++s->track_tick;
            if(s->cinematic.finished)s->post_ending_phase=3;
        }
        else if(s->boss_ending_phase) {
            if(s->boss_ending_phase==4){s->status=OD_ERR_FORMAT;return;}
            if(od_cinematic_tick(&s->cinematic,s->display)!=OD_OK) {
                s->boss_ending_status=OD_ERR_FORMAT;s->boss_ending_phase=4;s->status=OD_ERR_FORMAT;return;
            }
            ++s->track_tick;
            if(s->boss_ending_phase==2&&s->cinematic.cleanup_stage==3)s->boss_ending_phase=3;
            else if(s->cinematic.finished&&s->boss_ending_phase!=3)s->boss_ending_phase=2;
        }
        else if(s->goodnight_hidden_phase) {
            if(s->goodnight_hidden_phase==4){s->status=OD_ERR_FORMAT;return;}
            if(s->goodnight_hidden_phase==3&&!s->goodnight_hidden_kind) {
                /* The palette was queued after the engine returned. Now publish it, then run
                 * the hearth step every eighth live frame. Never age. */
                for(unsigned i=0;i<32;++i)s->display[i]=s->cinematic.pending_palette[i];
                const od_magicword_art *art=od_gfx_magicword_art();
                uint16_t next[OD_SCENE_WORDS];memcpy(next,s->display,sizeof next);
                if(od_gfx_static_magicword())od_magicword_night_cycle_static((uint16_t)(s->frame_tick-1u),&s->frame_cycle,next);
                else od_magicword_night_cycle(art,(uint16_t)(s->frame_tick-1u),&s->frame_cycle,next);
                for(unsigned i=0;i<32;++i)s->cinematic.pending_palette[i]=(uint8_t)next[i];
                s->cinematic.sounds.count=0;s->cinematic.sounds.managed=1;
            } else {
                if(od_cinematic_tick(&s->cinematic,s->display)!=OD_OK) {
                    s->goodnight_hidden_status=OD_ERR_FORMAT;s->goodnight_hidden_phase=4;s->status=OD_ERR_FORMAT;return;
                }
                ++s->track_tick;
                if((s->goodnight_hidden_phase==2&&s->goodnight_hidden_kind&&s->cinematic.cleanup_stage==3)||
                   (!s->goodnight_hidden_kind&&s->cinematic.finished)) {
                    s->goodnight_hidden_phase=3;
                    if(!s->goodnight_hidden_kind) {
                        const od_magicword_art *art=od_gfx_magicword_art();
                        const od_static_magicword *stat=od_gfx_static_magicword();
                        if(stat) {
                            const uint8_t *lut=od_palette_fade_lut();
                            if(!lut){s->goodnight_hidden_status=OD_ERR_FORMAT;s->goodnight_hidden_phase=4;s->status=OD_ERR_FORMAT;return;}
                            for(unsigned i=0;i<32;++i) {
                                unsigned brightness=i<16?stat->post_goodnight.brightness_bg:stat->post_goodnight.brightness_sprite;
                                unsigned base=stat->post_goodnight.palette_buffer[i];
                                s->cinematic.pending_palette[i]=brightness?((base&15u)|lut[(base&48u)+brightness]):15;
                            }
                        } else {
                            if(!art||!art->loaded){s->goodnight_hidden_status=OD_ERR_FORMAT;s->goodnight_hidden_phase=4;s->status=OD_ERR_FORMAT;return;}
                            memcpy(s->cinematic.pending_palette,art->night_palette,32);
                        }
                    }
                } else if(s->cinematic.finished)s->goodnight_hidden_phase=2;
            }
        }
        else if(s->world_clear_phase) {
            if(s->world_clear_phase==4){s->status=OD_ERR_FORMAT;return;}
            if(od_cinematic_tick(&s->cinematic,s->display)!=OD_OK) {
                s->world_clear_status=OD_ERR_FORMAT;s->world_clear_phase=4;s->status=OD_ERR_FORMAT;return;
            }
            ++s->track_tick;
            if(s->world_clear_phase==2&&s->cinematic.cleanup_stage==3)s->world_clear_phase=3;
            else if(s->cinematic.finished)s->world_clear_phase=2;
        }
        else if(s->castle_round10_phase) {
            if(s->castle_round10_phase==4) {s->status=OD_ERR_FORMAT;return;}
            if(od_cinematic_tick(&s->cinematic,s->display)!=OD_OK) {
                s->castle_round10_status=OD_ERR_FORMAT;s->castle_round10_phase=4;s->status=OD_ERR_FORMAT;return;
            }
            ++s->track_tick;
            if(s->castle_round10_phase==2)s->castle_round10_phase=3;
            else if(s->cinematic.finished)s->castle_round10_phase=2;
        }
        else
            apply(s);
    }
    s->visible_selection = s->selection; /* queued menu write: next display pass */
    /* WHATNEXT: the menu's close loop (60 frames of the hearth's close animation)
     * runs in the first frames of the scene its A selected (password f1501-f1560). */
    if (s->hearth_close && s->scene != OD_SCREEN_MENU) {
        od_hearth_call(&s->hearth, od_gfx_hearth_art(), 1);
        --s->hearth_close;
    }
    /* CASTILLOS: the castle clip tick drawn on this step (its sound cues are
     * keyed on it, flow.c) and the START skip of this step. */
    s->castle_skip = 0;
    s->castle_drawn = s->scene == OD_SCREEN_LEVEL_ENTRY && castle_track(s->track)
                          ? (int)s->track_tick - 1 : -1;
    /* FIN-DE-MUNDO: the clip tick drawn on this step (cues and lost NMIs are
     * keyed on it, flow.c), -1 when no clip frame was drawn. */
    s->world_clear_drawn = -1;
    if (s->scene == OD_SCREEN_WORLD_CLEAR && !s->world_clear_skip &&
        s->track_tick != s->world_clear_seen) {
        s->world_clear_drawn = (int)s->track_tick - 1;
        s->world_clear_seen = s->track_tick;
    }
    switch (s->scene) {
    case OD_SCREEN_INTRO:
        /* START is ignored before the script enables it. */
        if ((s->display[2381]&1) && (pressed&OD_BTN_START) &&
            s->age >= (s->track == INTRO ? 7u : 2u)) {
            s->status = 1;
            schedule(s,OD_SCREEN_TITLE);
        } else if (s->cinematic.finished ||
                   s->track_tick >= s->tracks[s->track].duration) {
            /* The program's own `finish` is the scene boundary: `measure`
             * returns the publication tick + 1, so ending on the finish tick
             * (not one past it) lines the chained title up with the native
             * (reference: intro image f0..f4376, title f4377). The duration is
             * kept only as a bound in case a program never reaches `finish`. */
            s->status = 0;
            schedule(s,OD_SCREEN_TITLE);
        }
        break;
    case OD_SCREEN_TITLE: {
        /* A13 / C3.6: the original's hidden access is one of the title input loop's
         * calls: it only runs from the loop's first pass (after the 129-tick setup)
         * until START is taken or the countdown ends. Reference, START at f300: the arm
         * flag set on the first SELECT frame of the loop (f430), the B edges before
         * it are ignored, and the tenth counted edge (f565) opens the hidden sequence.
         * Outside the loop only the press edge is tracked. The trigger takes precedence
         * over the START countdown. */
        int title_loop = !s->title_exit && s->age >= OD_TITLE_LOOP_AGE &&
                         s->age < OD_TITLE_LOOP_AGE + OD_TITLE_LOOP_FRAMES;
        if (!title_loop)
            s->hidden.previous = buttons;
        else if (od_hidden_scene_title_tick(&s->hidden, buttons)) {
            /* arm flag set: the title frame of the trigger is still shown and the
             * cinematic starts on the next one (f566). */
            s->title_exit = 0;
            schedule(s, OD_SCREEN_HIDDEN_INTRO);
            break;
        }
        /* Each pass of the title's input loop runs the pad and colour steps before it
         * tests START; the colour step advances its counter on the frames whose frame
         * counter & 3 == 0. The loop runs from the 129-tick setup until START is taken
         * or the 660 countdown runs out (reference R1: steps f430..f698, START f700;
         * START f250: f382..f1038). */
        if (!s->title_exit && s->age >= OD_TITLE_LOOP_AGE &&
            s->age < OD_TITLE_LOOP_AGE + OD_TITLE_LOOP_FRAMES &&
            (s->frame_tick & 3u) == 0) {
            /* PISTAS-A: that call reads the colour table by the colour counter and writes
             * palette entry 15, then steps the counter and uploads the palette, so the
             * colour reaches the screen on the next frame. */
            const od_screen_layouts *l = layouts();
            if (l && s->frame_cycle < l->colour_count) {
                s->title_colour = (uint16_t)(l->colour_table[s->frame_cycle] & 0x3fu);
                s->title_colour_valid = 1;
            }
            s->frame_cycle = frame_cycle_next(s->frame_cycle);
        }
        /* Title setup, 660-tick wait, START delay=30. The script setup consumes 129
         * ticks before this input loop. */
        if (s->title_exit) {
            if (--s->title_exit == 0) schedule(s,OD_SCREEN_CONVERSATION);
        } else if (s->age >= OD_TITLE_LOOP_AGE && (pressed&OD_BTN_START)) {
            s->title_exit = 30;
        } else if (s->age == 796) {
            /* The title's last script frame launches the repeat's engine (init request)
             * before the scene hands over. Begin it and run its init tick now, keeping the
             * title's own picture on this frame; the repeat draws from the next one. */
            uint16_t saved[OD_SCENE_WORDS];
            memcpy(saved, s->display, sizeof saved);
            od_cinematic_begin(&s->cinematic, s->bootstrap_data, 0);
            if(od_cinematic_tick(&s->cinematic, s->display)!=OD_OK) {
                s->status=OD_ERR_FORMAT; return;
            }
            memcpy(s->display, saved, sizeof saved);
            schedule(s,OD_SCREEN_INTRO);
        }
        break;
    }
    case OD_SCREEN_CONVERSATION:
        if (s->menu_init_wait) {
            /* With progress the title route's mode 3 (f1102 on the START route) is the same
             * init as B and EXIT: it draws "What next?" (R10). Its frame is blank. */
            if (--s->menu_init_wait == 0) {
                od_screens_menu_init(s, 1);
                s->menu_blank = 1;
            }
        } else if (((s->display[2381]&1) && (pressed&OD_BTN_START)) ||
            s->track_tick == s->tracks[s->track].duration) {
            if (s->boot_progress) {
                s->menu_init_wait = 2;   /* the scene tick, then mode 3 */
            } else {
                s->menu_lead = -1; /* mode 3 one frame after the scene (f1102) */
                schedule(s,OD_SCREEN_MENU);
            }
        }
        break;
    case OD_SCREEN_MENU:
        /* WHATNEXT: the menu loop calls the hearth animation from mode 3 + 6 for 17
         * frames, skips the 3 frames of the "Where to begin from?" text upload and then
         * calls it every frame, the A frame included (title route: mode 3 f1102, calls
         * f1108-f1124, f1128-f1500; map B: mode 3 f1800, calls f1806-f1822,
         * f1826-f2000). The picture is the recorded clip; only the counters are kept
         * for "What next?". */
        /* WHATNEXT-FADE: after the map's B or EXIT the clip's sprites are the title
         * route's fire; the hearth shows the last call of its own counters instead, as
         * in "What next?". */
        /* PISTAS-A: the hearth's last call is in the display (menu_frame). */
        if (od_hearth_menu_frame((int)s->age + s->menu_lead, OD_HEARTH_TEXT_BEGIN))
            od_hearth_call(&s->hearth, od_gfx_hearth_art(), 0);
        if (s->age >= 29) {
            /* DOWN then UP; START is not confirm. */
            if (pressed&OD_BTN_DOWN) s->selection = (s->selection+1)%3;
            if (pressed&OD_BTN_UP) s->selection = (s->selection+2)%3;
            if (pressed&OD_BTN_A) {
                s->hearth_close = 60; /* 60 frames of the hearth's close animation */
                schedule(s,s->selection == 0 ? OD_SCREEN_MENU_CLOSE :
                           s->selection == 1 ? OD_SCREEN_PASSWORD : OD_SCREEN_EDITOR);
            }
        }
        break;
    case OD_SCREEN_MENU_CLOSE:
        if (s->track_tick == menu_close_frames()) schedule(s,OD_SCREEN_MAP);
        break;
    case OD_SCREEN_MAP:
    case OD_SCREEN_ROUND_SELECT:
        /* Handled above by od_scene_map (C3.2). */
        break;
    case OD_SCREEN_LEVEL_ENTRY:
        if (castle_track(s->track)) {
            if(s->castle_round10_phase) {
                if(s->castle_round10_phase==1 && s->cinematic.skip_enabled && (pressed&OD_BTN_START)) {
                    if(od_cinematic_skip(&s->cinematic)!=OD_OK) {
                        s->castle_round10_status=OD_ERR_FORMAT;s->castle_round10_phase=4;s->status=OD_ERR_FORMAT;break;
                    }
                    s->castle_skip=1;s->castle_round10_phase=2;
                }
                if(s->castle_round10_phase==3)schedule(s,OD_SCREEN_PLAY);
                break;
            }
            /* A round10 track number without its native task is corrupt.
             * Mandatory legacy bytes remain inventory only, never fallback. */
            s->status=OD_ERR_FORMAT;
            break;
        }
        if (s->level_castle) {
            /* Castle lead-in: the entry clip is still playing; the switch above
             * takes over at OD_CASTLE_ENTRY_DELAY and releases from there. */
            break;
        }
        /* The reference raises its level dispatch exactly 15 ticks after the round-focus
         * `A` on every route (menu f2215, password 1-2 f14515, 5-6 f16515, 10-10
         * f21353). The scene clock publishes `play` one tick after it enters the scene,
         * so the entry must release at frame `OD_LEVEL_ENTRY_FRAMES` (13), not at the
         * extracted 19-frame clip's end: the clip's last six frames are the first
         * mode-7 loading frames (grid at +2, Hero at +5), reproduced by the staged play
         * entry in main.c. */
        if (s->track_tick >= OD_LEVEL_ENTRY_FRAMES) {
            /* The WORLD 10 castle (ROUND 10-10) plays the extracted queen
             * briefing / boss room / Boss dialogue (H4.10) before the level
             * loads; every other round goes straight to play. The boss mode
             * is raised one frame after the other routes' dispatch, so
             * its release is held one frame past `OD_LEVEL_ENTRY_FRAMES`. */
            if (s->world == 9 && s->round == 9) {
                if (s->track_tick > OD_LEVEL_ENTRY_FRAMES)
                    schedule(s, OD_SCREEN_BOSS);
            } else
                schedule(s, OD_SCREEN_PLAY);
        }
        break;
    case OD_SCREEN_PASSWORD: case OD_SCREEN_EDITOR:
        /* The password and editor scenes are driven by their modules (H5/C3.3);
         * screens.c only plays their extracted display. */
        break;
    case OD_SCREEN_WHATNEXT: {
        /* C3.3: the "What next?" menu as logic over the extracted maqueta. `cont.story`
         * opens the map with the progress the magicword (or the session) left in
         * `map_words`; `edit mode` enters the editor. `quit` ("Today's magicword") is
         * not reconstructed in C3.3. */
        /* WHATNEXT: the hearth OAM and bank the last animation call wrote reach the
         * screen on this frame; then this frame's call, if the loop makes one: frames
         * 6-22 and 25.. until the A, then the 60-frame close (the faces change) before
         * the 15-frame fade. */
        /* The scene is entered on the reference's mode-3 frame (by the flow
         * after END, or by the map's B), so this tick is frame age + 1 of the
         * menu (f11760 + age + 1; map B f4400 + age + 1). */
        const od_hearth_art *hart = od_gfx_hearth_art();
        od_hearth_patch(&s->hearth, hart, s->display, OD_SCENE_WORDS);
        int closing = s->wn.leaving > OD_WHATNEXT_EXIT_TICKS - 60u;
        if (closing ||
            (!s->wn.leaving &&
             od_hearth_menu_frame((int)s->age + 1, OD_HEARTH_TEXT_WHATNEXT)))
            od_hearth_call(&s->hearth, hart, closing);
        od_whatnext_event wev = od_scene_whatnext_tick(&s->wn, buttons);
        od_scene_whatnext_panel(&s->wn, s->display, OD_SCENE_WORDS);
        unsigned return_world = s->whatnext_world;
        if (wev != OD_WHATNEXT_NONE)
            s->whatnext_world = 0;
        if (wev == OD_WHATNEXT_CONT_STORY) {
            /* C6.23 (CORE-2): cont.story reopens the map on the world the player was on.
             * The map's B, edit mode -> EXIT, the magicword's YES and a password entered
             * after the map's B all keep it (reference: B on WORLD 5 f15800 -> cont.story
             * -> WORLD 5 at f16700; the same through EXIT f16750 and YES f16700; B on
             * WORLD 3 -> password -> WORLD 3 at f13300). `s->world` already holds it: the
             * map refreshes it every tick and the menu, editor, magicword and password
             * scenes leave it alone (boot and RESET set 0, the password routes' world 1).
             * Only an end-of-world's world counter increment moves it (`whatnext_world`).
             * A hidden-world session (10..14) is not a map world: the map's world 1, as
             * before (not measured). */
            if (return_world)
                s->world = return_world;
            else if (s->world >= OD_MAP_WORLD_COUNT)
                s->world = 0;
            s->round = 0;
            s->map_focus = 0;
            s->map_ready = 0; /* the map is already open after cont.story */
            schedule(s, OD_SCREEN_MAP);
        } else if (wev == OD_WHATNEXT_EDIT_MODE) {
            /* C6.31: like `quit` below, the editor entry starts on the frame the close ends (A +
             * 75, reference f12275: its `1` there, counter = 31 at f12277), so the editor
             * entry's program runs its first frame on this tick, not on the next one.
             * "What next?" has already closed its own menu: the program starts directly
             * (no hearth close). */
            enter(s, OD_SCREEN_EDITOR, EDITOR);
            editor_task(s, pressed);
        } else if (wev == OD_WHATNEXT_QUIT && (od_gfx_magicword_art()||od_gfx_static_magicword())) {
            /* The magicword scene's init is the frame the close ends on (A + 75: reference
             * f12275 is already its first, black, frame), so the scene starts
             * on this tick instead of the next one. */
            enter(s, OD_SCREEN_MAGICWORD, 0);
            if(od_gfx_static_magicword()) {
                if(od_scene_magicword_begin_static(&s->mw,s->display)!=OD_OK){s->status=OD_ERR_FORMAT;return;}
            } else od_scene_magicword_begin(&s->mw,od_gfx_magicword_art());
            goto magicword;
        }
        break;
    }
    case OD_SCREEN_MAGICWORD: magicword: {
        /* MAGICWORD: one CPU frame per tick; the magicword is the progress's
         * (map_words). */
        od_magicword_event mev;
        if(od_gfx_static_magicword()) {
            if(od_scene_magicword_validate_static(&s->mw)){s->status=OD_ERR_FORMAT;return;}
            mev=od_scene_magicword_tick_static(&s->mw,buttons,s->frame_tick,s->map_words,&s->frame_cycle,&s->cycle_6f,s->display);
        }
        else mev=od_scene_magicword_tick(&s->mw,od_gfx_magicword_art(),buttons,s->frame_tick,s->map_words,&s->frame_cycle,&s->cycle_6f,s->display);
        if (mev == OD_MAGICWORD_YES) {
            /* Mode 3 from this frame (reference A f12700); "What next?" counts its age from
             * the frame after, as after END (f11760 + 1) and the map's B (f4400 + 1). The
             * init blanks this frame (reference f12700 black). */
            schedule(s, OD_SCREEN_WHATNEXT);
            s->menu_blank = 1;
        } else if (mev == OD_MAGICWORD_NO)
            schedule(s, OD_SCREEN_GOODNIGHT); /* good-night cinematic script */
        break;
    }
    case OD_SCREEN_GOODNIGHT: {
        /* Native highbit return followed by the original's noninteractive loop.
         * No captured duration/palette tick or recorded hold controls it. */
        break;
    }
    case OD_SCREEN_WORLD_CLEAR: {
        if(s->world_clear_phase) {
            /* World9's root completes its own cleanup publication before the
             * caller launches42 at the next step. Opening42 never reads START. */
            if(od_screens_world_clear_complete(s)) {
                schedule(s,OD_SCREEN_WHATNEXT);s->menu_blank=1;
                s->whatnext_world=s->world_clear_world+1u;
            } else if(s->world_clear_phase==1&&s->world_clear_part==0&&
                      (pressed&OD_BTN_START)&&s->cinematic.skip_enabled) {
                if(od_cinematic_skip(&s->cinematic)!=OD_OK) {
                    s->world_clear_status=OD_ERR_FORMAT;s->world_clear_phase=4;s->status=OD_ERR_FORMAT;return;
                }
                s->world_clear_skip=1;s->world_clear_phase=2;
            }
            if(s->world_clear_phase==3&&s->world_clear_skip)s->world_clear_skip=2;
            break;
        }
        /* FIN-DE-MUNDO: the recorded end-of-world cinematic
         * frame for frame; its last clip frame is the one map frame after the
         * clip window. The next frame is the menu init (black, "What next?"
         * from the frame after; reference 1-10: clip f17632..f18707, map frame f18708, menu
         * f18709). World 9's runs into the castle opening (above). START skips once the
         * script has enabled it: the START frame still shows the cinematic and requests
         * `1`, the next one (map frame) and the menu frame are black (reference 1-10: START
         * f18000, menu f18002). The castle opening never reads START.
         * CASTILLO-APARECE: the castle appearance returns from
         * the clear handler to the map instead: the step after its last map frame raises
         * `world_clear_to_map` and the flow opens the map on it (reference 1-1 last:
         * map frame f18306, map opening f18307). */
        const od_castle_info *ci = od_screens_world_clear_info(s);
        if (s->world_clear_skip == 1) {
            s->world_clear_skip = 2;              /* the black map frame */
        } else if (s->world_clear_part == 2 &&
                   (s->world_clear_skip == 2 || s->world_clear_drawn < 0)) {
            s->world_clear_to_map = 1;
        } else if (s->world_clear_skip == 2 || s->world_clear_drawn < 0) {
            schedule(s, OD_SCREEN_WHATNEXT);     /* the mode-3 frame */
            s->menu_blank = 1;
            /* The world counter goes up per celebrated world: the next one (world 9's leads
             * to world 10, the castle). */
            s->whatnext_world = s->world_clear_world + 1u;
        } else if (ci && (pressed & OD_BTN_START) &&
                   (unsigned)s->world_clear_drawn >= ci->skip_from) {
            s->world_clear_skip = 1;
        }
        break;
    }
    case OD_SCREEN_BOSS:
        /* The queen briefing, boss room and Boss dialogue run automatically
         * (no input on the PAL route); the clip ends as ROUND 10-10 loads. */
        if(od_screens_boss_ending_complete(s))schedule(s,OD_SCREEN_PLAY);
        else if(od_screens_boss_ending_skip_ready(s)&&(pressed&OD_BTN_START)) {
            if(od_cinematic_skip(&s->cinematic)!=OD_OK){s->boss_ending_status=OD_ERR_FORMAT;s->boss_ending_phase=4;s->status=OD_ERR_FORMAT;return;}
            s->boss_ending_phase=2;
        }
        break;
    case OD_SCREEN_HIDDEN_INTRO:
        /* The recorded cinematic after the trigger (reference f566-f850,
         * the hidden-rounds announcement); the menu follows on the next frame (f851). From its second frame the
         * script enables START: the frame that reads it requests `1` (audio), the next
         * one is black and the menu starts after it (reference START f700: `1` f700,
         * black f701, menu f702). The press edge keeps being tracked for the menu. */
        s->hidden.previous = buttons;
        if(od_screens_goodnight_hidden_complete(s)) {
            if(s->hidden_skip)s->hidden_skip=2;
            schedule(s, OD_SCREEN_HIDDEN);
        } else if(od_screens_goodnight_hidden_skip_ready(s)&&(pressed&OD_BTN_START)) {
            if(od_cinematic_skip(&s->cinematic)!=OD_OK) {
                s->goodnight_hidden_status=OD_ERR_FORMAT;s->goodnight_hidden_phase=4;s->status=OD_ERR_FORMAT;return;
            }
            s->hidden_skip=1;s->goodnight_hidden_phase=2;
        }
        s->hidden_partial = 1;
        break;
    case OD_SCREEN_HIDDEN: {
        /* The hidden-menu selector: A/B step the level, SELECT+A/B a whole world, START
         * confirms. The chosen 100..149 level maps to world 10..14 / round 0..9 so the
         * ordinary entry path loads it. */
        int level = od_hidden_scene_menu_tick(&s->hidden, buttons);
        if (level >= 0) {
            int w = 0, r = 0;
            od_hidden_scene_level_world_round(level, &w, &r);
            s->world = (unsigned)w;
            s->round = (unsigned)r;
            s->map_focus = 1;
            s->level_castle = 0;
            od_hidden_menu shown = s->hidden.menu;
            od_hidden_scene_reset(&s->hidden);
            /* C6.31: the START frame still shows the menu as chosen (reference
             * f4600 "ROUND 11-2" after 11-1 solved; the reset showed 11-1). */
            s->hidden.menu.level = shown.level;
            s->hidden.menu.sound = shown.sound;
            /* START sets mode 7 on its own frame (reference f960): the load follows
             * directly, no map round entry. */
            s->hidden_dispatch = 1;
            schedule(s, OD_SCREEN_PLAY);
        }
        break;
    }
    case OD_SCREEN_ENDING:
        /* Native27 completes its causal engine cleanup. Caller9D24's palette4,
         * D3=0/script7 infinite loop is NOT root27 or a recorded-track fallback;
         * completion is exposed to its separate caller owner. No free START. */
        break;
    case OD_SCREEN_PLAY: case OD_SCREEN_UNVERIFIED: break;
    }
    ++s->age;
    ++s->local_tick;
    ++s->tick;
}

int od_screens_castle_round10_active(const od_screens *s)
{return s && s->scene==OD_SCREEN_LEVEL_ENTRY && s->castle_round10_phase!=0;}
int od_screens_castle_round10_complete(const od_screens *s)
{return od_screens_castle_round10_active(s) && s->castle_round10_phase==3 && s->castle_round10_status==OD_OK;}
int od_screens_castle_round10_skip_ready(const od_screens *s)
{return od_screens_castle_round10_active(s) && s->castle_round10_phase==1 && s->cinematic.skip_enabled && s->castle_round10_status==OD_OK;}
od_status od_screens_castle_round10_status(const od_screens *s)
{return s?s->castle_round10_status:OD_ERR_FORMAT;}
int od_screens_castle_round10_frame_tick_enabled(const od_screens *s)
{return od_screens_castle_round10_active(s) && s->castle_round10_status==OD_OK && s->cinematic.render_enabled;}
int od_screens_world_clear_active(const od_screens *s)
{return s&&s->scene==OD_SCREEN_WORLD_CLEAR&&s->world_clear_part<2&&s->world_clear_phase!=0;}
int od_screens_goodnight_hidden_active(const od_screens *s)
{return s&&s->goodnight_hidden_phase&&(s->scene==OD_SCREEN_GOODNIGHT||s->scene==OD_SCREEN_HIDDEN_INTRO);}
int od_screens_boss_ending_active(const od_screens *s)
{return s&&s->boss_ending_phase&&(s->scene==OD_SCREEN_BOSS||s->scene==OD_SCREEN_ENDING);}
od_status od_screens_post_ending_begin(od_screens *s)
{
    if(!s||!s->post_ending_data||s->scene!=OD_SCREEN_ENDING||!od_screens_boss_ending_complete(s)||!s->boss_ending_kind)return OD_ERR_FORMAT;
    /* Caller9D24 palette4/D3 publication remains caller-owned. Root07 then
     * performs its OWN normal palette/NT/OAM entry. No root87 alias or hold. */
    od_cinematic_begin(&s->cinematic,s->post_ending_data,0);
    for(unsigned i=0;i<2048;++i)s->cinematic.tiles[i]=(uint8_t)s->display[32+i];
    memcpy(s->cinematic.pending_oam,s->display+2124,sizeof s->cinematic.pending_oam);
    for(unsigned i=0;i<4;++i){s->cinematic.bg_banks[i]=(uint8_t)(s->display[2080+i]/64);s->cinematic.split_banks[i]=(uint8_t)(s->display[2084+i]/64);}
    for(unsigned i=0;i<2;++i)s->cinematic.sprite_banks[i]=(uint8_t)(s->display[2088+i]/64);
    s->boss_ending_phase=0;s->boss_ending_kind=0;s->boss_ending_status=OD_OK;
    s->post_ending_phase=1;s->post_ending_kind=0;s->post_ending_status=OD_OK;s->track_tick=0;
    s->cinematic.sounds.managed=1;return OD_OK;
}
int od_screens_post_ending_active(const od_screens *s)
{return s&&s->post_ending_phase&&s->scene==OD_SCREEN_ENDING;}
int od_screens_post_ending_program_finished(const od_screens *s)
{return od_screens_post_ending_active(s)&&s->post_ending_phase==3&&s->post_ending_status==OD_OK;}
int od_screens_post_ending_complete(const od_screens *s)
{(void)s;return 0;}
int od_screens_post_ending_kind(const od_screens *s)
{return od_screens_post_ending_active(s)?0:-1;}
int od_screens_post_ending_skip_ready(const od_screens *s)
{(void)s;return 0;}
int od_screens_post_ending_frame_tick_enabled(const od_screens *s)
{return !od_screens_post_ending_active(s)||s->cinematic.render_enabled;}
od_status od_screens_post_ending_status(const od_screens *s)
{return s?s->post_ending_status:OD_ERR_FORMAT;}
int od_screens_boss_ending_complete(const od_screens *s)
{return od_screens_boss_ending_active(s)&&s->boss_ending_phase==3&&s->boss_ending_status==OD_OK;}
int od_screens_boss_ending_kind(const od_screens *s)
{return od_screens_boss_ending_active(s)?(int)s->boss_ending_kind:-1;}
int od_screens_boss_ending_skip_ready(const od_screens *s)
{return od_screens_boss_ending_active(s)&&!s->boss_ending_kind&&s->boss_ending_phase==1&&s->boss_ending_status==OD_OK&&s->cinematic.skip_enabled;}
int od_screens_boss_ending_frame_tick_enabled(const od_screens *s)
{return !od_screens_boss_ending_active(s)||s->cinematic.render_enabled;}
od_status od_screens_boss_ending_status(const od_screens *s)
{return s?s->boss_ending_status:OD_ERR_FORMAT;}
int od_screens_goodnight_hidden_complete(const od_screens *s)
{return od_screens_goodnight_hidden_active(s)&&s->goodnight_hidden_phase==3&&s->goodnight_hidden_status==OD_OK;}
int od_screens_goodnight_hidden_kind(const od_screens *s)
{return od_screens_goodnight_hidden_active(s)?(int)s->goodnight_hidden_kind:-1;}
int od_screens_goodnight_hidden_skip_ready(const od_screens *s)
{return od_screens_goodnight_hidden_active(s)&&s->goodnight_hidden_kind==1&&s->goodnight_hidden_phase==1&&s->goodnight_hidden_status==OD_OK&&s->cinematic.skip_enabled;}
int od_screens_goodnight_hidden_frame_tick_enabled(const od_screens *s)
{return !od_screens_goodnight_hidden_active(s)||s->cinematic.render_enabled;}
od_status od_screens_goodnight_hidden_status(const od_screens *s)
{return s?s->goodnight_hidden_status:OD_ERR_FORMAT;}
int od_screens_world_clear_complete(const od_screens *s)
{return od_screens_world_clear_active(s)&&s->world_clear_phase==3&&s->world_clear_status==OD_OK&&
    !(s->world_clear_world==8&&s->world_clear_part==0&&!s->world_clear_skip);}
int od_screens_world_clear_skip_ready(const od_screens *s)
{return od_screens_world_clear_active(s)&&s->world_clear_part==0&&s->world_clear_phase==1&&s->world_clear_status==OD_OK&&s->cinematic.skip_enabled;}
int od_screens_world_clear_frame_tick_enabled(const od_screens *s)
{return od_screens_world_clear_active(s)&&s->world_clear_status==OD_OK&&s->cinematic.render_enabled;}
od_status od_screens_world_clear_status(const od_screens *s)
{return s?s->world_clear_status:OD_ERR_FORMAT;}

static unsigned pixel(const uint8_t *chr, unsigned tile, unsigned x, unsigned y)
{
    const uint8_t *p = chr + (tile&8191)*16;
    unsigned bit = 7-(x&7);
    return ((p[y&7]>>bit)&1) | (((p[(y&7)+8]>>bit)&1)<<1);
}

/* Compose one scene display vector. Shared by the plain scene renderer and the
 * password/editor overlays, which patch a copy first. */
static void compose_display(const uint16_t *d, od_screen_id scene, unsigned age,
                            unsigned visible_selection, od_index_frame *fb,
                            const uint8_t *chr)
{
    unsigned band = 0, bands = d[2093] > 5 ? 5 : d[2093];
    unsigned sprite_count = d[2124] > 64 ? 64 : d[2124];
    for (unsigned y = 0; y < 240; ++y) {
        while (band+1 < bands && y >= d[2094+(band+1)*6]) ++band;
        const uint16_t *b = d+2094+band*6;
        unsigned row_sprites[8], count = 0;
        for (unsigned i = 0; i < sprite_count && count < 8; ++i) {
            unsigned sy = d[2125+i*4+1];
            if (y >= sy && y < sy+8) row_sprites[count++] = i;
        }
        for (unsigned x = 0; x < 256; ++x) {
            unsigned color = d[0], opaque = 0;
            if ((d[2092]&8) && (x >= 8 || (d[2092]&2))) {
                unsigned sx = x+b[1], sy = y+b[2];
                unsigned nt = b[3] ^ ((sx/256)&1) ^ (((sy/240)&1)<<1);
                sx %= 256; sy %= 240;
                unsigned base = (d[2381]&2) ? (nt&2)*512 : (nt&1)*1024;
                unsigned tx = sx/8, ty = sy/8;
                unsigned tile = d[32+base+ty*32+tx];
                if (scene == OD_SCREEN_MENU && age > 29 && ty >= 19 &&
                    ty <= 23 && (ty&1) && tx == 17)
                    tile = ty == 19+visible_selection*2 ? 254 : 0;
                unsigned bank = d[2080+(b[4]&1)*4+tile/64];
                opaque = pixel(chr,bank+tile%64,sx,sy);
                unsigned attr = d[32+base+960+(ty/4)*8+tx/4];
                unsigned palette = (attr >> (((ty&2)<<1)|(tx&2))) & 3;
                if (opaque) color = d[palette*4+opaque];
            }
            if ((d[2092]&16) && (x >= 8 || (d[2092]&4))) {
                for (unsigned k = 0; k < count; ++k) {
                    const uint16_t *sp = d+2125+row_sprites[k]*4;
                    if (x < sp[0] || x >= (unsigned)sp[0]+8) continue;
                    unsigned sx = x-sp[0], sy = y-sp[1], attr = sp[3];
                    if (attr&64) sx ^= 7;
                    if (attr&128) sy ^= 7;
                    unsigned tile = d[2088+(b[5]&1)*2+sp[2]/128]+sp[2]%128;
                    unsigned value = pixel(chr,tile,sx,sy);
                    if (!value) continue;
                    if (!(attr&32) || !opaque) color = d[16+(attr&3)*4+value];
                    break;
                }
            }
            fb->index[y*256+x] = (uint8_t)(color&63);
        }
    }
}

/* The WORLD map opens behind the scanline split handler, which shows the map
 * only inside the regions of its table; the rest of the screen is the blank
 * nametable. Measured on the PAL reference (R1, f3991..f4037, and the menu
 * route f1475..f1520), with `t` the map age of the table the frame shows (it
 * lags one frame):
 *   - t 5..32: one window, first line 96 - 2(t-6) from t 6 on, height
 *     8 + 4(t-6); the picture only appears from age 8 (f3999);
 *   - t 33..42: the map region runs from 40 - 2(t-33) to line 160 and the
 *     panel region from line 161 grows by 4 a frame (last line 166 + h2,
 *     h2 = 8 + 4(t-33));
 *   - from t 43 the table is the full screen and from age 45 it is cleared.
 * Rows outside the regions are black; the partial row a split lands in is
 * the split artefact (MJ4) and is drawn hidden. */
#define MAP_SPRITE_COUNT_WORD 2124u /* compose_display's sprite count word */

static void map_reveal_mask(const od_screens *s, od_index_frame *fb)
{
    unsigned a = s->map.age + map_reveal_lead(s);
    if (!s->map_reveal || a >= OD_MAP_REVEAL_FRAMES ||
        (s->scene != OD_SCREEN_MAP && s->scene != OD_SCREEN_ROUND_SELECT))
        return;
    int lo1 = 1, hi1 = 0, lo2 = 1, hi2 = 0;      /* empty regions */
    if (a >= 8) {
        int t = (int)a - 1;
        if (t < 33) {
            int k = t - 6, top = 0x60 - 2 * k;
            lo1 = top + 1;
            hi1 = top + (8 + 4 * k) + 1;
        } else if (t < 43) {
            int k = t - 33;
            lo1 = 0x28 - 2 * k + 1;
            hi1 = 160;
            lo2 = 161;
            hi2 = 166 + 8 + 4 * k;
            /* The panel region's own PPU address shows the layout one line
             * lower: screen line y is panel line y + 1 (reference f4025..f4034
             * equal to the steady panel shifted by one line). */
            for (int y = lo2; y <= hi2 && y + 1 < OD_SCREEN_H; ++y)
                memcpy(&fb->index[y * OD_SCREEN_W],
                       &fb->index[(y + 1) * OD_SCREEN_W], OD_SCREEN_W);
        } else {
            return;
        }
    }
    for (int y = 0; y < OD_SCREEN_H; ++y) {
        if ((y >= lo1 && y <= hi1) || (y >= lo2 && y <= hi2))
            continue;
        memset(&fb->index[y * OD_SCREEN_W], 0x0f, OD_SCREEN_W);
    }
}

/* WHATNEXT-FADE: "What next?" is still entering (mode-3 init, fade-in and the
 * panel's layout upload) on its first frames; every entry (END, the map's B,
 * `Yes`, EXIT) counts its age from mode 3. */
#define OD_MENU_ENTRY_FRAMES 32u
static int whatnext_entry(const od_screens *s)
{
    const od_hearth_art *a = od_gfx_hearth_art();
    return a && a->queue_capacity && !s->wn.leaving &&
           s->age <= OD_MENU_ENTRY_FRAMES;
}

void od_screens_draw_pal(const od_screens *s, od_index_frame *fb,
                          const uint8_t *chr, const od_palette_ram *pal)
{
    if (s->scene == OD_SCREEN_HIDDEN) {
        /* The hidden menu is not an extracted scene layout: it is CHR text
         * drawn through the presentation palette. Without a palette (a caller
         * that only has the plain draw path) keep the frame black rather than
         * dereference NULL. */
        /* The hidden menu's entry (reference f851 or, after a START skip, f702): two
         * black frames, then the frame whose VRAM upload has not yet written
         * the round value's first character, then the full menu. The scene's
         * age is one past its frame when it is drawn. */
        if (!pal || s->age <= 2u) {
            for (size_t i = 0; i < OD_SCREEN_W * OD_SCREEN_H; ++i)
                fb->index[i] = 0x0f;
            return;
        }
        if (s->age == 3u && s->hidden_partial == 2) {
            /* C6.31: the solved round's value is still in VRAM (f4444). */
            od_hidden_scene stale = s->hidden;
            stale.menu.level = s->hidden_stale_level;
            od_hidden_scene_draw(&stale, fb, chr, pal);
            return;
        }
        od_hidden_scene_draw(&s->hidden, fb, chr, pal);
        if (s->age == 3u && s->hidden_partial) {
            for (int y = OD_HIDDEN_ROW0_Y; y < OD_HIDDEN_ROW0_Y + 8; ++y)
                for (int x = OD_HIDDEN_ROUND_X; x < OD_HIDDEN_ROUND_X + 8; ++x)
                    fb->index[y * OD_SCREEN_W + x] = 0x0f;
        }
        return;
    }
    /* CIERRE-C3.6: the black frame between a START skip of the
     * hidden-access cinematic and the hidden menu (reference f701). */
    if (s->scene == OD_SCREEN_HIDDEN_INTRO && s->hidden_skip == 2) {
        for (size_t i = 0; i < OD_SCREEN_W * OD_SCREEN_H; ++i)
            fb->index[i] = 0x0f;
        return;
    }
    /* WHATNEXT-FADE: the mode-3 frame of the map's B and of the magicword's
     * `Yes` is already the menu init's black screen (the stale sprites of the
     * scene left behind are not modelled: reference f4400 "1", f12700 a dot). */
    if ((s->pending && s->menu_blank) ||
        (s->scene == OD_SCREEN_WORLD_CLEAR && s->world_clear_skip >= 2)) {
        for (size_t i = 0; i < OD_SCREEN_W * OD_SCREEN_H; ++i)
            fb->index[i] = 0x0f;
        return;
    }
    int map = s->scene == OD_SCREEN_MAP || s->scene == OD_SCREEN_ROUND_SELECT;
    /* Until the map loop's init has run, the screen is the bare layout: no
     * sprites (the WORLD number is one), the marks in their layout palette and
     * no cursor (reference f4025..f4037); the init's panel shows from map age 47
     * (f4038). */
    int bare = map && s->map_reveal &&
               s->map.age + map_reveal_lead(s) < OD_MAP_REVEAL_PANEL_AGE;
    if (bare) {
        static uint16_t layout[OD_SCENE_WORDS];
        memcpy(layout, s->map_maqueta, sizeof layout);
        /* The castle map's own script draws the castle during the
         * opening (w1010 route: whole castle from f11578, opening f11575). */
        od_scene_map_castle(&s->map, od_gfx_map_art(), layout, OD_SCENE_WORDS);
        layout[MAP_SPRITE_COUNT_WORD] = 0;
        compose_display(layout, s->scene, s->age, s->visible_selection, fb, chr);
    } else if (s->scene == OD_SCREEN_WHATNEXT && whatnext_entry(s)) {
        /* WHATNEXT-FADE: the panel as the layout stands on this frame of the menu
         * (frame = scene age from the reference's mode-3 frame). */
        static uint16_t entry[OD_SCENE_WORDS];
        const od_hearth_art *hart = od_gfx_hearth_art();
        memcpy(entry, s->display, sizeof entry);
        od_menu_layout_draw(&hart->layouts[OD_MENU_LAYOUT_WHATNEXT],
                             hart->queue_capacity, (int)s->age, entry,
                             OD_SCENE_WORDS);
        compose_display(entry, s->scene, s->age, s->visible_selection, fb, chr);
    } else {
        compose_display(s->display, s->scene, s->age, s->visible_selection, fb,
                        chr);
    }
    if (map) {
        /* C3.2: the logic panel is already patched into `display`. Without the
         * package's map sprites (MAPA-MUNDOS), draw the Hero cursor on top with
         * the panel's own sprite palettes; with them, Hero is in the OAM. */
        od_palette_ram p;
        for (unsigned i = 0; i < 32; ++i)
            p.ram[i] = (uint8_t)s->display[i];
        if (!bare && !od_gfx_map_art())
            od_scene_map_draw_cursor(&s->map, fb, chr, &p);
        map_reveal_mask(s, fb);
    }
    /* "What next?" -> cont.story leaves through the map fade: the brightness drops
     * from 14 to 0 over the last 15 frames of the exit hold and the palette lands
     * one display pass later, so the shown brightness is the hold left, 0 on the
     * frame the map starts (10-10 password route: A f11500, brightness 14 at f11560
     * .. 0 at f11574, map starts at f11575). `edit mode` leaves through the same fade
     * (A f12200, brightness 14 at f12260 .. 0 at f12274, editor entry at f12275). */
    const uint8_t *lut = od_palette_fade_lut();
    if (s->scene == OD_SCREEN_WHATNEXT && lut &&
        (s->wn.pending == OD_WHATNEXT_CONT_STORY ||
         s->wn.pending == OD_WHATNEXT_EDIT_MODE) && s->wn.leaving < 15u) {
        unsigned b = s->wn.leaving;
        for (unsigned i = 0; i < OD_SCREEN_W * OD_SCREEN_H; ++i) {
            uint8_t c = fb->index[i];
            fb->index[i] = (uint8_t)((c & 15) | lut[(c & 0x30) + b]);
        }
    }
    /* WHATNEXT-FADE: the entry's fade-in, through the same brightness table (black
     * before it; reference brightness 1..15 at f11766..f11780). */
    if (s->scene == OD_SCREEN_WHATNEXT && lut && whatnext_entry(s)) {
        int b = od_menu_entry_brightness((int)s->age);
        if (b < 15)
            for (unsigned i = 0; i < OD_SCREEN_W * OD_SCREEN_H; ++i) {
                uint8_t c = fb->index[i];
                fb->index[i] = (uint8_t)((c & 15) | lut[(c & 0x30) + (unsigned)b]);
            }
    }
}

void od_screens_draw(const od_screens *s, od_index_frame *fb, const uint8_t *chr)
{
    od_screens_draw_pal(s, fb, chr, NULL);
}

unsigned od_screens_level_fade(const od_screens *s, const uint8_t **palettes)
{
    if (palettes) *palettes = s->level_fade[0];
    return s->level_fade_frames;
}

/* Password screen overlay (H5/C3.3). The geometry, the 24 field glyphs and the
 * two cursors live in scene_password.c; this only copies the scene display,
 * patches the drawn state and composites it. */
void od_screens_draw_password(const od_screens *s,
                               const uint8_t field_tiles[24], unsigned position,
                               unsigned cursor, od_index_frame *fb,
                               const uint8_t *chr)
{
    if (!od_screens_password_ready(s)) {
        od_screens_draw(s, fb, chr);
        return;
    }
    uint16_t d[OD_SCENE_WORDS];
    memcpy(d, s->display, sizeof d);
    /* PISTAS-B: the rejected-magicword message is the game's own record
     * ("Error! try again.") written over the prompt row. */
    if (s->password_error && od_gfx_password_art())
        od_menu_record_write(&od_gfx_password_art()->error, d, OD_SCENE_WORDS);
    od_scene_password_panel(field_tiles, position, cursor, d,
                             OD_SCENE_WORDS);
    const od_interactive_setup *art=od_interactive_setup_get();
    if(art) {
        d[2125]=art->password_field_cursor_x[position%24];
        d[2129]=art->password_palette_cursor_oam_yx[cursor%35][1];
        d[2130]=art->password_palette_cursor_oam_yx[cursor%35][0]+1;
    }
    password_oam_allocate(d,s->hearth.drawn_rotation);
    if (s->password_error_blank)
        od_scene_password_no_cursors(d, OD_SCENE_WORDS);
    compose_display(d, s->scene, s->age, s->visible_selection, fb, chr);
}

void od_screens_draw_editor(const od_screens *s, unsigned toolbar,
                             int show_cursor, od_index_frame *fb,
                             const uint8_t *chr)
{
    /* The cursor cannot precede publication of the real init jobs. Rendering
     * and caller input use this same query, never an extracted clip age. */
    if (!od_screens_editor_ready(s)) {
        od_screens_draw(s, fb, chr);
        return;
    }
    uint16_t d[OD_SCENE_WORDS];
    memcpy(d, s->display, sizeof d);
    if (!show_cursor) {
        d[2124] = 0;
    } else {
        d[2124] = 1;
        d[2125] = od_editor_toolbar_x[toolbar % 3]; d[2126] = 24;
        d[2127] = 0x7e; d[2128] = 0;
    }
    compose_display(d, s->scene, s->age, s->visible_selection, fb, chr);
}

/* C3.7: the real editor overlay. The board, the A/B tool markers and the
 * toolbar/palette/grid cursors come from scene_editor.c; this only patches the
 * scene display and composites it (the H5 `od_screens_draw_editor` above is
 * kept for the H5 acceptance test, which draws the toolbar cursor alone). */
void od_screens_draw_editor_full(const od_screens *s, const od_editor *ed,
                                  od_index_frame *fb, const uint8_t *chr)
{
    od_screens_draw_editor_cycle(s, ed, NULL, fb, chr);
}

/* The caller owns the shared-clock colour cycle after native init publication.
 * While blocked, the publisher's pending/visible buffers are authoritative. */
void od_screens_draw_editor_cycle(const od_screens *s, const od_editor *ed,
                                   const od_editor_cycle *cyc,
                                   od_index_frame *fb, const uint8_t *chr)
{
    /* EE77/83DE row jobs and marker publication run in editor_task, not in a
     * render-time age patch. The resumed board must be staged by the caller. */
    if (!od_screens_editor_ready(s) || !ed) {
        od_screens_draw(s, fb, chr);
        return;
    }
    uint16_t d[OD_SCENE_WORDS];
    memcpy(d, s->display, sizeof d);
    od_scene_editor_panel(ed, d, OD_SCENE_WORDS);
    if (cyc) {
        od_scene_editor_cycle_apply(cyc, d);
        od_scene_editor_toolbar_redraw(d, OD_SCENE_WORDS, cyc->toolbar_phase);
    }
    compose_display(d, s->scene, s->age, s->visible_selection, fb, chr);
}

void od_screens_retry(od_index_frame *fb, const uint8_t *chr, unsigned selection)
{
    selection &= 1;
    const uint8_t *tiles = od_gfx_retry_tiles();
    const uint8_t *palette = od_gfx_retry_palette();
    const uint8_t *cols = od_gfx_retry_cursor_cols();
    unsigned cursor = od_gfx_retry_cursor_tile();
    for (unsigned y = 216; y < 240; ++y)
        for (unsigned x = 0; x < 256; ++x) {
            unsigned tile = tiles[(y/8-27)*32+x/8];
            if (y/8 == 28 && (x/8 == cols[0] || x/8 == cols[1]))
                tile = x/8 == cols[selection] ? cursor : 0;
            unsigned value = pixel(chr,tile,x,y);
            fb->index[y*256+x] = palette[value & 3];
        }
}

const char *od_screens_name(const od_screens *s)
{
    static const char *const names[] = {"intro","title","conversation","menu",
        "menu-close","map","round-select","level-entry","play","password",
        "editor","boss","ending","whatnext","hidden","magicword","goodnight","hidden-intro",
        "world-clear","unverified"};
    return s->scene <= OD_SCREEN_UNVERIFIED ? names[s->scene] : "invalid";
}
