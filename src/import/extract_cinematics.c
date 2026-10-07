/* Static EU scene-data extraction: the scene tables are read at fixed
 * positions of the image. No captures are opened here. */
#include "extract_cinematics.h"
#include "fsutil.h"
#include "opendana/platform.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const od_rom *rom; int bad; unsigned bad_bank,bad_addr; } reader;
static unsigned byte(reader *r, unsigned bank, unsigned a)
{
    size_t p=(a>=0xC000?(a<0xE000?14:15):(a<0xA000?8:bank))*8192u+(a&8191);
    if(a<0x8000 || a>0xFFFF || p>=r->rom->prg_size) {
        if(!r->bad) { r->bad_bank=bank; r->bad_addr=a; }
        r->bad=1; return 0;
    }
    return r->rom->prg[p];
}
static unsigned word(reader *r,unsigned bank,unsigned a) { return byte(r,bank,a)|(byte(r,bank,a+1)<<8); }
/* Serial staged importer: every JSON construction failure aborts publication,
 * including empty nested arrays and failed object-key allocations. */
static int construction_failed;
static cJSON *failed_items;
/* Retain failed transfers until construction ends: callers may still borrow
 * arrays (program operands/scenes) after an attempted object insertion. */
static void retain_failed(cJSON *v)
{
    if(v) { v->next=failed_items; v->prev=NULL; failed_items=v; }
}
static cJSON *array(void) { cJSON *j=cJSON_CreateArray(); if(!j) construction_failed=1; return j; }
static cJSON *object(void) { cJSON *j=cJSON_CreateObject(); if(!j) construction_failed=1; return j; }
static void add(cJSON *j,cJSON *v)
{
    if(!v || !cJSON_AddItemToArray(j,v)) { construction_failed=1; retain_failed(v); }
}
static void put(cJSON *j,const char *key,cJSON *v)
{
    if(!v || !cJSON_AddItemToObject(j,key,v)) { construction_failed=1; retain_failed(v); }
}
static void integer(cJSON *j,const char *key,unsigned n) { put(j,key,cJSON_CreateNumber(n)); }
static void string(cJSON *j,const char *key,const char *s) { put(j,key,cJSON_CreateString(s)); }
static void push(cJSON *j,unsigned n) { add(j,cJSON_CreateNumber(n)); }
static unsigned num(const cJSON *j,unsigned i) { const cJSON *v=cJSON_GetArrayItem(j,(int)i); return v?(unsigned)v->valueint:0; }
static cJSON *bytes(reader *r,unsigned bank,unsigned a,unsigned n)
{
    cJSON *j=array();
    for(unsigned i=0;i<n;++i) push(j,byte(r,bank,a+i));
    return j;
}
static cJSON *bg_banks(reader *r,unsigned i)
{
    if(i&128) return bytes(r,9,0x946A+((i<<2)&255),4);
    cJSON *j=array(); for(unsigned k=0;k<4;++k) push(j,i+k); return j;
}
static cJSON *background(reader *r,const cJSON *blocks,unsigned mode)
{
    unsigned nt[2048]={0},attrs[128];
    for(unsigned i=0;i<128;++i) attrs[i]=255;
    for(int i=0;i<cJSON_GetArraySize(blocks);++i) {
        const cJSON *block=cJSON_GetArrayItem(blocks,i);
        unsigned address=word(r,9,0x9CDC+2*num(block,0));
        unsigned p=word(r,7,0xA008+2*num(block,1));
        unsigned set=byte(r,7,p),dims=byte(r,7,p+1); p+=2;
        unsigned tiles=word(r,7,0xA000+2*set),palettes=word(r,7,0xA004+2*set);
        unsigned cols=((dims>>4)&7)+1,rows=(dims&7)+1;
        for(unsigned y=0;y<rows;++y) for(unsigned x=0;x<cols;++x) {
            unsigned id=byte(r,7,p++),a=address+y*128+x*4;
            for(unsigned yy=0;yy<4;++yy) for(unsigned xx=0;xx<4;++xx) {
                unsigned address_tile=a+yy*32+xx;
                unsigned physical=mode?(address_tile&1023)|((address_tile&2048)>>1):(address_tile-0x2000)&2047;
                nt[physical]=byte(r,7,tiles+16*id+yy*4+xx);
            }
            unsigned lo=a&255,hi=a>>8;
            unsigned physical=mode?(a&1023)|((a&2048)>>1):(a-0x2000)&2047;
            unsigned ai=((lo>>2)&7)|(lo&128?8:0)|((hi&3)<<4)|(physical&1024?64:0);
            attrs[ai]=byte(r,7,palettes+id);
        }
    }
    for(unsigned i=0;i<64;++i) { nt[960+i]=attrs[i]; nt[1984+i]=attrs[64+i]; }
    cJSON *j=array(); for(unsigned i=0;i<2048;++i) push(j,nt[i]); return j;
}
static cJSON *upload(reader *r,unsigned bank,unsigned p)
{
    cJSON *out=array();
    for(unsigned budget=0;budget<256;++budget) {
        unsigned count=byte(r,bank,p++); if(!count) return out;
        int repeat=count==255; if(repeat) count=byte(r,bank,p++);
        unsigned lo=byte(r,bank,p++),hi=byte(r,bank,p++),inc=count&128?32:1;
        count&=127;
        cJSON *wr=array(),*v=array();
        push(wr,hi*256+lo); push(wr,inc);
        for(unsigned k=0;k<count;++k) push(v,byte(r,bank,p+(repeat?0:k)));
        p+=repeat?1:count; add(wr,v); add(out,wr);
    }
    r->bad=1; return out;
}

/* Scene program reader. Calls enter at an opcode; a return resumes there,
 * while FF closes a group or terminates as a delay. */
static cJSON *program(reader *r,unsigned index)
{
    static const char *const names[]={"text","shake-stop","flush","clear","skip-toggle",
        "bands","velocity","fade","chr","sprite-banks","vram","backdrop","palettes",
        "black","palette-cycle","image-cycle","scroll","sprite-split","object","sound",
        "scene","image","call","delay","shake"};
    cJSON *out=array();
    unsigned bank=index<28?9:13,p=word(r,9,0x9D14+2*index),depth=0,mode=0;
    unsigned return_bank[16],return_pos[16]; int need_delay=1;
    for(unsigned budget=0;budget<20000 && !r->bad;++budget) {
        if(need_delay) {
            unsigned addr=p,delay=byte(r,bank,p++);
            cJSON *c=object(),*args=array();
            string(c,"op",delay==255?"finish":"step"); if(delay!=255) push(args,delay);
            put(c,"args",args); integer(c,"bank",bank); integer(c,"addr",addr);
            add(out,c);
            if(delay==255) return out;
            need_delay=0;
        }
        unsigned addr=p,raw=byte(r,bank,p++),n=raw&63,a=0;
        cJSON *c=object(),*args=array();
        const char *name=NULL; unsigned source_bank=bank;
        if(raw==255) { name="end-step"; need_delay=1; }
        else if(raw&64) {
            name="background";
            if(n) for(unsigned i=0;i<5;++i) push(args,byte(r,bank,p++));
            cJSON *blocks=array();
            for(unsigned i=0;i<n;++i) { add(blocks,bytes(r,bank,p,2)); p+=2; }
            put(c,"blocks",blocks);
        } else if(raw&128) {
            name="objects";
            if(n) for(unsigned i=0;i<5;++i) push(args,byte(r,bank,p++));
            cJSON *objects=array();
            for(unsigned i=0;i<n;++i) {
                cJSON *obj=bytes(r,bank,p,4); p+=4;
                unsigned t=num(obj,3)&15,extra=0;
                if(t && t!=8) {
                    extra=1;
                    if(t==2) ++extra;
                    else if(t==3) extra+=2;
                    else if(t>=4) extra+=1+!!((t-4)&2);
                }
                for(unsigned k=0;k<extra;++k) push(obj,byte(r,bank,p++));
                add(objects,obj);
            }
            put(c,"objects",objects);
        } else {
            if(n>=25) { r->bad=1; cJSON_Delete(c); cJSON_Delete(args); break; }
            name=names[n]; if(n>=5) { a=byte(r,bank,p++); push(args,a); }
            unsigned extra=0;
            if(n==6) extra=(a&128 && !(a&64))?4:2;
            else if(n==9 || n==23) extra=1;
            else if(n==12) extra=(a&15)?(a&15):256;
            else if(n==14 || n==15) extra=a!=0;
            else if(n==16) extra=a&128?1:2;
            else if(n==18) {
                unsigned t=a&15;
                if(t && t!=8) extra=t==3?2:1+(t==2?1:t>=4?1+!!((t-4)&2):0);
            } else if(n==21) extra=!(a&128);
            for(unsigned k=0;k<extra;++k) push(args,byte(r,bank,p++));
            if(n==22) {
                if(a&128) {
                    name="return"; if(!depth) r->bad=1;
                    else { --depth; bank=return_bank[depth]; p=return_pos[depth]; }
                } else if(depth<16) {
                    return_bank[depth]=bank; return_pos[depth++]=p;
                    bank=9; p=word(r,9,0x9CF4+2*a);
                } else r->bad=1;
            }
        }
        /* Consistent key ordering matches the Python extraction exactly. */
        cJSON *ordered=object();
        string(ordered,"op",name?name:""); put(ordered,"args",args);
        integer(ordered,"bank",source_bank); integer(ordered,"addr",addr);
        cJSON *blocks=cJSON_DetachItemFromObjectCaseSensitive(c,"blocks");
        cJSON *objects=cJSON_DetachItemFromObjectCaseSensitive(c,"objects");
        if(name && !strcmp(name,"chr")) mode=num(args,0);
        if(blocks) {
            put(ordered,"blocks",blocks); put(ordered,"layout",background(r,blocks,mode));
            put(ordered,"banks",bg_banks(r,num(args,0)));
        }
        if(objects) {
            put(ordered,"objects",objects); unsigned v=num(args,4);
            cJSON *banks=array();
            if(cJSON_GetArraySize(args)) {
                if(v&128) { cJSON_Delete(banks); banks=bytes(r,9,0x952A+((v<<1)&255),2); }
                else { push(banks,v); push(banks,v+2); }
            }
            put(ordered,"banks",banks);
        }
        if(name && !strcmp(name,"vram")) put(ordered,"writes",upload(r,source_bank,word(r,9,0x9820+num(args,0))));
        cJSON_Delete(c); add(out,ordered);
    }
    r->bad=1; return out;
}

/* Text bytecode. Dictionary calls expand statically, controls stay semantic.
 * Not text scraped from frames, and not display writes computed ahead. */
static cJSON *text_program(reader *r,unsigned index,unsigned stops)
{
    static const char *names[]={"line","pause","return","upload","erase","font-next","font-reset","stop"};
    cJSON *out=array();
    if(!stops) return out;
    unsigned p=word(r,9,0xB0D9+2*index),stack[32],depth=0;
    for(unsigned budget=0;budget<20000 && !r->bad;++budget) {
        unsigned raw=byte(r,9,p++); const char *name=NULL;
        cJSON *args=array();
        if(raw<128) { name="glyph"; push(args,raw); }
        else if(raw<144) { name="speed"; push(args,raw&15); }
        else if(raw<160) { name="pause-short"; push(args,raw&15); }
        else if(raw<248) {
            if(depth==32) r->bad=1;
            else { stack[depth++]=p; p=word(r,9,0xB119+2*(raw-160)); }
        } else if(raw==250) {
            if(!depth) r->bad=1; else p=stack[--depth];
        } else {
            name=names[raw-248];
            if(raw==249) push(args,byte(r,9,p++));
            else if(raw==251) {
                unsigned count=byte(r,9,p++); push(args,count);
                for(unsigned k=0;k<count+2;++k) push(args,byte(r,9,p++));
            }
        }
        if(name) {
            cJSON *c=object(); string(c,"op",name); put(c,"args",args); add(out,c);
            if(raw==255 && !--stops) return out;
        } else cJSON_Delete(args);
    }
    r->bad=1; return out;
}

static od_status import_family(const od_rom *rom,const char *directory,char *err,size_t cap,unsigned family)
{
    if(!rom || !rom->prg || rom->prg_size!=131072) { snprintf(err,cap,"invalid EU cinematic ROM data"); return OD_ERR_FORMAT; }
    const int bootstrap=family==1,editor=family==2,round10=family==3,worldclear=family==4,goodnight=family==5,bossending=family==6,postending=family==7;
    const char *id=postending?"post-ending":bossending?"boss-ending":goodnight?"goodnight-hidden":worldclear?"world-clear":round10?"castle-round10":editor?"editor-entry":bootstrap?"bootstrap":"castle-appears";
    construction_failed=0;
    failed_items=NULL;
    reader r={rom,0,0,0}; cJSON *root=object(),*source=object();
    integer(root,"schema_version",1); string(root,"id",id);
    string(source,"rom_sha256","853646c50889dfb7a74c2acf7cf3b76f42d044745433f706dca33f47d826cd1d");
    put(root,"source",source);
    cJSON *scenes=array();
    for(unsigned slot=0;slot<(postending?1u:(goodnight || bossending)?2u:round10?9u:editor?1u:bootstrap?3u:10u);++slot) {
        /* The scene index (root) follows the family: world*2+8 for the
         * round-10 castles, world*2+9 for the world clears (root 42 after the
         * ninth world), 31 and 5 for the good-night scenes. */
        unsigned i=postending?7:bossending?26+slot:goodnight?(slot?5:31):worldclear?(slot==9?42:9+2*slot):round10?8+2*slot:editor?29:bootstrap?(slot==2?30:slot):32+slot;
        cJSON *scene=object(); integer(scene,"index",i); put(scene,"program",program(&r,i)); add(scenes,scene);
    }
    put(root,"scenes",scenes);
    cJSON *poses=array();
    for(unsigned i=0;i<208;++i) {
        unsigned p=byte(&r,13,0xA074+i)|(byte(&r,13,0xA144+i)<<8),count=byte(&r,13,p++);
        cJSON *pose=array();
        for(unsigned k=0;k<count;++k) {
            unsigned tile=byte(&r,13,p++),x=byte(&r,13,p++),y=byte(&r,13,p++);
            cJSON *piece=array();
            push(piece,byte(&r,13,0xA000+(x&63))); push(piece,byte(&r,13,0xA038+(y&63)));
            push(piece,tile); push(piece,(x>>6)|(y&64)); add(pose,piece);
            if(y&128) p=word(&r,13,p);
        }
        add(poses,pose);
    }
    put(root,"poses",poses);
    cJSON *animations=array();
    /* Animation pointers are indexed by animation number. World-clear nested
     * talking actors in roots 23/25 consume entries 44/45; the first target
     * delimits all 46 pointers. Preserve previous families' output shapes. */
    /* Goodnight-hidden only consumes animation16: retain dense prefix0..16. */
    for(unsigned i=0;i<(goodnight?17u:(worldclear || bossending || postending)?46u:round10?44u:32u);++i) {
        unsigned p=word(&r,9,0x923F+2*i),n=byte(&r,9,p); cJSON *anim=array();
        for(unsigned k=0;k<n;++k) add(anim,bytes(&r,9,p+1+2*k,2));
        add(animations,anim);
    }
    put(root,"animations",animations);
    cJSON *bands=array();
    for(unsigned i=0;i<14;++i) {
        unsigned p=word(&r,9,0x955A+2*i),n=byte(&r,9,p+2)&7;
        add(bands,bytes(&r,9,p,3+3*n+4+2*n+1));
    }
    put(root,"bands",bands);
    cJSON *cycles=array();
    for(unsigned i=0;i<20;++i) {
        cJSON *cycle=object(); put(cycle,"values",bytes(&r,9,word(&r,9,0x9609+2*i),64));
        put(cycle,"indices",bytes(&r,9,word(&r,9,0x9631+2*i),8)); add(cycles,cycle);
    }
    put(root,"cycles",cycles);
    cJSON *velocities=array();
    for(unsigned i=0;i<27;++i) add(velocities,bytes(&r,9,0x93FE + 4*i,4));
    put(root,"velocities",velocities);
    cJSON *bank_sets=array();
    for(unsigned i=0;i<256;++i) add(bank_sets,bg_banks(&r,i));
    put(root,"bg_bank_sets",bank_sets);
    cJSON *addresses=array();
    for(unsigned i=0;i<12;++i) push(addresses,word(&r,9,0x9CDC+2*i));
    put(root,"band_addresses",addresses);
    cJSON *colours=array();
    for(unsigned i=0;i<256;++i) add(colours,bytes(&r,9,0x90A1+3*i,3));
    put(root,"colours",colours);
    if(bootstrap || editor || round10 || worldclear || goodnight || bossending || postending) {
        cJSON *scene=NULL;
        cJSON_ArrayForEach(scene,scenes) {
            unsigned stops=0; cJSON *command=NULL;
            cJSON_ArrayForEach(command,cJSON_GetObjectItemCaseSensitive(scene,"program")) {
                const cJSON *op=cJSON_GetObjectItemCaseSensitive(command,"op");
                if(cJSON_IsString(op) && !strcmp(op->valuestring,"text")) ++stops;
            }
            const cJSON *index=cJSON_GetObjectItemCaseSensitive(scene,"index");
            if(!index) { construction_failed=1; break; }
            put(scene,"text",text_program(&r,(unsigned)index->valueint,stops));
        }
        cJSON *addresses_text=array();
        for(unsigned i=0;i<4;++i) push(addresses_text,byte(&r,9,0x9099+i)|(byte(&r,9,0x909D+i)<<8));
        put(root,"text_addresses",addresses_text);
    }
    if(postending) {
        /* EU853E doubles selector into061F; next operand sets0620.
         * EU9021 reads two bytes at pointer97D0[selector]+2*0622,
         * increments0622 and wraps at0620. Root7 image3/2 and4/2
         * require exactly two pairs each. Empty slots are UNUSED, no padding. */
        unsigned counts[8]={0}; cJSON *scene=NULL;
        cJSON_ArrayForEach(scene,scenes) {
            cJSON *command=NULL;
            cJSON_ArrayForEach(command,cJSON_GetObjectItemCaseSensitive(scene,"program")) {
                const cJSON *op=cJSON_GetObjectItemCaseSensitive(command,"op");
                if(cJSON_IsString(op) && !strcmp(op->valuestring,"image")) {
                    const cJSON *args=cJSON_GetObjectItemCaseSensitive(command,"args");
                    const cJSON *first=cJSON_GetArrayItem(args,0);
                    if(!first) { r.bad=1; continue; }
                    unsigned selector=(unsigned)first->valueint;
                    if(selector&128u) continue;
                    const cJSON *count=cJSON_GetArrayItem(args,1);
                    if(selector>=8 || !count || count->valueint<1 || count->valueint>32) { r.bad=1; continue; }
                    if(counts[selector]<(unsigned)count->valueint) counts[selector]=(unsigned)count->valueint;
                }
            }
        }
        cJSON *images=array();
        for(unsigned i=0;i<8;++i) add(images,counts[i]?bytes(&r,9,word(&r,9,0x97D0+2*i),2*counts[i]):array());
        put(root,"image_cycles",images);
    }
    if(round10 || worldclear || bossending) {
        cJSON *shakes=array(),*images=bossending?NULL:array();
        /* Fixed bank8: first destinations delimit 2 and 8 pointers, NOT the
         * distance to the next named table (which includes inline data). */
        for(unsigned i=0;i<2;++i) add(shakes,bytes(&r,9,word(&r,9,0x980A+2*i),16));
        if(!bossending) for(unsigned i=0;i<8;++i) add(images,bytes(&r,9,word(&r,9,0x97D0+2*i),64));
        put(root,"shake_patterns",shakes);
        if(!bossending) put(root,"image_cycles",images);
    }
    if(bootstrap || round10 || worldclear || bossending) {
        cJSON *palette_cycles=array();
        for(unsigned i=0;i<3;++i) add(palette_cycles,bytes(&r,9,word(&r,9,0x97BE + 2*i),4));
        put(root,"palette_cycles",palette_cycles);
    }
    if(bootstrap) {
        cJSON *palette=bytes(&r,2,word(&r,15,0xE71E)+16*0x10,16);
        unsigned sprite=word(&r,15,0xE720)+16*0x0C;
        for(unsigned i=0;i<16;++i) push(palette,byte(&r,2,sprite+i));
        put(root,"title_palette",palette);
        cJSON *colours_title=array();
        for(unsigned i=0;i<7;++i) push(colours_title,rom->prg[0xA07+i]);
        put(root,"title_colours",colours_title);
        integer(root,"title_loop_ticks",byte(&r,15,0xEB63)|(byte(&r,15,0xEB65)<<8));
    }
    char *text=(r.bad || construction_failed)?NULL:cJSON_PrintUnformatted(root); cJSON_Delete(root);
    cJSON_Delete(failed_items); failed_items=NULL;
    if(construction_failed || (!r.bad && !text)) { snprintf(err,cap,"cannot allocate cinematic content"); return OD_ERR_NOMEM; }
    if(!text) { snprintf(err,cap,"invalid cinematic table at bank %u address %04x",r.bad_bank,r.bad_addr); return OD_ERR_FORMAT; }
    char relative[128],path[4096]; snprintf(relative,sizeof relative,"screens/cinematics/%s.json",id);
    od_platform_join(path,sizeof path,directory,relative);
    size_t n=strlen(text); char *output=malloc(n+2);
    if(!output) { cJSON_free(text); return OD_ERR_NOMEM; }
    memcpy(output,text,n); output[n]='\n'; output[n+1]=0; cJSON_free(text);
    int result=od_fs_write(path,output,n+1); free(output);
    if(result) snprintf(err,cap,"cannot write cinematic content");
    return result?OD_ERR_IO:OD_OK;
}

/* Static menu data uses bank6/2, unlike the cinematic bank8/9 pair. */
static unsigned extra_byte(reader *r,unsigned a)
{
    unsigned bank=a<0xA000?6:a<0xC000?2:a<0xE000?14:15;
    size_t p=bank*8192u+(a&8191u);
    if(a<0x8000 || a>0xFFFF || p>=r->rom->prg_size) { r->bad=1; return 0; }
    return r->rom->prg[p];
}
static unsigned extra_word(reader *r,unsigned a) { return extra_byte(r,a)|(extra_byte(r,a+1)<<8); }
static cJSON *extra_bytes(reader *r,unsigned a,unsigned n)
{
    cJSON *out=array(); if((a&8191u)+n>8192u) r->bad=1;
    for(unsigned i=0;i<n;++i) push(out,extra_byte(r,a+i));
    return out;
}
/* E145 records preserve control bits, order, duplicate destinations and stride.
 * These are static VRAM writes, NOT frames or a captured event agenda. */
static cJSON *extra_records(reader *r,unsigned p,unsigned relative)
{
    cJSON *out=array();
    for(unsigned budget=0;budget<128 && !r->bad;++budget) {
        unsigned ctl=extra_byte(r,p),n=ctl&63u;
        if(!ctl) return out;
        unsigned a=extra_word(r,p+1)+relative,stride=ctl&128u?32u:1u;
        if(!n || n>=60 || (ctl&64u) || a<0x2000 || a+(n-1)*stride>=0x3000 ||
           p<0xA000 || p+3+n>=0xC000) { r->bad=1; break; }
        cJSON *record=array(); push(record,ctl); push(record,a);
        add(record,extra_bytes(r,p+3,n)); add(out,record); p+=3+n;
    }
    r->bad=1; return out;
}
static cJSON *extra_layout(reader *r,unsigned p)
{
    cJSON *out=array();
    for(unsigned budget=0;budget<32 && !r->bad;++budget,p+=2) {
        unsigned target=extra_word(r,p); if(!target) return out;
        cJSON *records=extra_records(r,target,0),*v;
        while((v=cJSON_DetachItemFromArray(records,0))) add(out,v);
        cJSON_Delete(records);
    }
    r->bad=1; return out;
}
static cJSON *extra_palette(reader *r,unsigned bg,unsigned sp)
{
    cJSON *out=extra_bytes(r,extra_word(r,0xE71E)+16*bg,16);
    unsigned p=extra_word(r,0xE720)+16*sp;
    for(unsigned i=0;i<16;++i) push(out,extra_byte(r,p+i));
    return out;
}
static cJSON *extra_command(const char *op,cJSON *records)
{
    cJSON *out=object(); string(out,"op",op); if(records) put(out,"records",records); return out;
}
static od_status write_extra(reader *r,cJSON *root,const char *dir,const char *rel,char *err,size_t cap)
{
    char *text=(r->bad || construction_failed)?NULL:cJSON_PrintUnformatted(root);
    cJSON_Delete(root); cJSON_Delete(failed_items); failed_items=NULL;
    if(construction_failed || (!r->bad && !text)) { snprintf(err,cap,"cannot allocate static scene extras"); return OD_ERR_NOMEM; }
    if(!text) { snprintf(err,cap,"invalid static scene extras table"); return OD_ERR_FORMAT; }
    size_t n=strlen(text); char *output=malloc(n+2);
    if(!output) { cJSON_free(text); return OD_ERR_NOMEM; }
    memcpy(output,text,n); output[n]='\n'; output[n+1]=0; cJSON_free(text);
    char path[4096]; od_platform_join(path,sizeof path,dir,rel);
    int result=od_fs_write(path,output,n+1); free(output);
    if(result) snprintf(err,cap,"cannot write static scene extras");
    return result?OD_ERR_IO:OD_OK;
}
static od_status import_extras(const od_rom *rom,const char *dir,char *err,size_t cap)
{
    construction_failed=0; failed_items=NULL;
    reader r={rom,0,0,0}; cJSON *root=object(),*bases=array();
    integer(root,"schema_version",2); string(root,"id","layouts");
    for(unsigned i=0;i<2;++i) {
        /* Map roots2/43 call9/10, fixed bank8 background9F46/9F58. */
        unsigned p=word(&r,9,0x9CF4+2*(9+i))+2;
        if(byte(&r,9,p)!=0x41) r.bad=1;
        cJSON *blocks=array(); add(blocks,bytes(&r,9,p+6,2));
        cJSON *base=object(); integer(base,"scene_index",i?43:2); integer(base,"chr_mode",0);
        put(base,"background_banks",bg_banks(&r,byte(&r,9,p+1)));
        put(base,"ciram",background(&r,blocks,0)); cJSON_Delete(blocks); add(bases,base);
    }
    put(root,"map_bases",bases);
    od_status status=write_extra(&r,root,dir,"screens/layouts.json",err,cap);
    if(status!=OD_OK) return status;
    construction_failed=0; failed_items=NULL;
    root=object(); integer(root,"schema_version",2); string(root,"id","magicword");
    put(root,"palette_buffer",extra_palette(&r,12,8)); put(root,"chr",extra_bytes(&r,0xEFC7,6));
    integer(root,"song_request",extra_byte(&r,0xEFD3));
    cJSON *screen=array(); add(screen,extra_command("records",extra_layout(&r,0xB5D4)));
    add(screen,extra_command("yield",NULL)); add(screen,extra_command("records",extra_records(&r,0xBC62,0x2141)));
    add(screen,extra_command("yield",NULL)); add(screen,extra_command("word",NULL)); put(root,"screen",screen);
    cJSON *question=array(); add(question,extra_command("records",extra_layout(&r,0xB5F2))); put(root,"question",question);
    cJSON *word_doc=object(); integer(word_doc,"address",extra_byte(&r,0x841D)|(extra_byte(&r,0x841F)<<8));
    integer(word_doc,"count",extra_byte(&r,0x8434)); integer(word_doc,"group",extra_byte(&r,0x8443));
    integer(word_doc,"gap_tile",extra_byte(&r,0x846D)); put(word_doc,"glyphs",extra_bytes(&r,0xBC21,32)); put(root,"word",word_doc);
    cJSON *cursor=object(); integer(cursor,"y",extra_byte(&r,0x8511)); integer(cursor,"tile",extra_byte(&r,0x8515));
    integer(cursor,"attr",extra_byte(&r,0x8519)); put(cursor,"x",extra_bytes(&r,0x8565,2)); put(root,"cursor",cursor);
    cJSON *cycle=object(),*tables=array(),*entries=array();
    integer(cycle,"bg_mask",extra_byte(&r,0x8D8E)); for(unsigned i=0;i<3;++i) { push(entries,5+i); add(tables,extra_bytes(&r,0x8DDC+3*i,3)); }
    put(cycle,"bg_entries",entries); put(cycle,"bg_tables",tables); integer(cycle,"sprite_mask",extra_byte(&r,0x8DBA));
    integer(cycle,"sprite_entry",19); put(cycle,"sprite_table",extra_bytes(&r,0x8DE5,5)); put(root,"cycle",cycle);
    integer(root,"queue_capacity_bytes",extra_byte(&r,0xE19B));
    cJSON *night=object(),*night_cycle=object(); put(night,"palette_buffer",extra_palette(&r,extra_byte(&r,0x8554),extra_byte(&r,0x8556)));
    integer(night,"brightness_bg",extra_byte(&r,0xE68A)); integer(night,"brightness_sprite",extra_byte(&r,0xE68A));
    integer(night_cycle,"mask",extra_byte(&r,0x8DED)); integer(night_cycle,"entry",3);
    put(night_cycle,"colours",extra_bytes(&r,0x8E0F,4)); put(night,"cycle",night_cycle); put(root,"post_goodnight",night);
    return write_extra(&r,root,dir,"screens/magicword.json",err,cap);
}

od_status od_import_cinematics(const od_rom *rom,const char *directory,char *err,size_t cap)
{
    od_status status=import_family(rom,directory,err,cap,0);
    if(status==OD_OK) status=import_family(rom,directory,err,cap,1);
    if(status==OD_OK) status=import_family(rom,directory,err,cap,2);
    if(status==OD_OK) status=import_family(rom,directory,err,cap,3);
    if(status==OD_OK) status=import_family(rom,directory,err,cap,4);
    if(status==OD_OK) status=import_family(rom,directory,err,cap,5);
    if(status==OD_OK) status=import_family(rom,directory,err,cap,6);
    if(status==OD_OK) status=import_family(rom,directory,err,cap,7);
    return status==OD_OK?import_extras(rom,directory,err,cap):status;
}
