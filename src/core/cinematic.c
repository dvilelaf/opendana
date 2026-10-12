/* Cinematic scene language, not a CPU interpreter. Unknown instructions and
 * missing family data fail closed; no recordings are runtime inputs. */
#include "opendana/cinematic.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct od_cinematic_data { cJSON *root; };
enum { ENTRY_RUN, ENTRY_INIT, ENTRY_PALETTE, ENTRY_CLEAR };
static const cJSON *field(const cJSON *j, const char *key) { return cJSON_GetObjectItemCaseSensitive(j,key); }
static const cJSON *at(const cJSON *j, unsigned i) { return cJSON_GetArrayItem(j,(int)i); }
static unsigned value(const cJSON *j) { return j && cJSON_IsNumber(j) ? (unsigned)j->valueint : 0; }
static unsigned num(const cJSON *j, unsigned i) { return value(at(j,i)); }
static const cJSON *table(const od_cinematic *s, const char *key, unsigned i) { return at(field(s->data->root,key),i); }
static int bootstrap(const od_cinematic *s)
{
    const cJSON *id=field(s->data->root,"id");
    return cJSON_IsString(id) && !strcmp(id->valuestring,"bootstrap");
}
static int staged_entry(const od_cinematic *s)
{
    const cJSON *id=field(s->data->root,"id");
    return bootstrap(s) || (cJSON_IsString(id) && (!strcmp(id->valuestring,"editor-entry") || !strcmp(id->valuestring,"castle-round10") || !strcmp(id->valuestring,"world-clear") || !strcmp(id->valuestring,"goodnight-hidden") || !strcmp(id->valuestring,"boss-ending") || !strcmp(id->valuestring,"post-ending")));
}
/* The transfer is performed inline in the frame that reads its step; it only
 * occupies a held frame when its work does not fit one PAL frame. The block
 * writes one attribute byte per 4x4 metatile, and the cleared nametable leaves
 * every attribute at its maximum, so the number of attributes the program's own
 * expanded layout leaves below the maximum is the block's metatile count. The clear
 * (19.868 U) plus the block work (2.217 + 654*N U) fits the PAL frame (33.247 U
 * at 106.392 dots/frame, master/16 and master/5) while 19.868 + 2.217 + 654*N
 * <= 33.247, i.e. N <= 17. No captured frame number: N is read from the package
 * layout, the rest is the routine's own work. */
static unsigned background_metatiles(const cJSON *c)
{
    const cJSON *layout=field(c,"layout");
    if(!cJSON_IsArray(layout)) return 0;
    unsigned n=0;
    for(unsigned i=960;i<1024;++i) if(num(layout,i)!=255) ++n;
    for(unsigned i=1984;i<2048;++i) if(num(layout,i)!=255) ++n;
    return n;
}

/* True when the command batch that follows a staged `background` contains an
 * explicit `flush` before its `end-step`: that flush supplies the extra
 * main-loop iteration the transfer would otherwise have to hold itself. */
static int batch_has_flush(const cJSON *program, unsigned pc)
{
    for (unsigned i = pc; ; ++i) {
        const cJSON *c = at(program, i);
        if (!c) return 0;
        const cJSON *op = field(c, "op");
        if (!cJSON_IsString(op)) return 0;
        if (!strcmp(op->valuestring, "end-step")) return 0;
        if (!strcmp(op->valuestring, "flush")) return 1;
    }
}

static int numbers(const cJSON *j,unsigned min,unsigned max,unsigned bound)
{
    int n=cJSON_GetArraySize(j);
    if(!cJSON_IsArray(j) || n<(int)min || n>(int)max) return 0;
    const cJSON *v=NULL;
    cJSON_ArrayForEach(v,j) {
        if(!cJSON_IsNumber(v) || v->valuedouble<0 || v->valuedouble>bound ||
           v->valuedouble!=(double)v->valueint) return 0;
    }
    return 1;
}
static int rows(const cJSON *j,unsigned count,unsigned width)
{
    if(!cJSON_IsArray(j) || cJSON_GetArraySize(j)!=(int)count) return 0;
    for(unsigned i=0;i<count;++i) if(!numbers(at(j,i),width,width,255)) return 0;
    return 1;
}
static int editor_command_valid(const cJSON *c)
{
    const cJSON *op=field(c,"op"),*args=field(c,"args");
    if(!cJSON_IsString(op)) return 0;
    const char *name=op->valuestring;unsigned n=(unsigned)cJSON_GetArraySize(args);
    if(!strcmp(name,"finish") || !strcmp(name,"end-step") || !strcmp(name,"skip-toggle") || !strcmp(name,"text")) return n==0;
    if(!strcmp(name,"step") || !strcmp(name,"sound") || !strcmp(name,"bands") || !strcmp(name,"fade"))
        return n==1 && (strcmp(name,"bands") || (num(args,0)&63u)<14);
    if(!strcmp(name,"palettes")) return n==1+(num(args,0)&15u) && (num(args,0)&15u)<=8;
    if(!strcmp(name,"object")) {
        unsigned t=num(args,0)&15u;
        return n==1 && t==8 && (num(args,0)>>4)<8; /* only deletion8 used by1D */
    }
    if(!strcmp(name,"background")) return n==5 && numbers(field(c,"layout"),2048,2048,255) && numbers(field(c,"banks"),4,4,255);
    if(!strcmp(name,"objects")) {
        if(n!=5 || !numbers(field(c,"banks"),2,2,255) || !cJSON_IsArray(field(c,"objects")) || cJSON_GetArraySize(field(c,"objects"))>8) return 0;
        const cJSON *obj=NULL;
        cJSON_ArrayForEach(obj,field(c,"objects"))
            if(!numbers(obj,5,5,255) || (num(obj,3)&15u)!=1 || num(obj,2)>=208 || num(obj,4)>=27) return 0;
        return 1;
    }
    return 0; /* no undocumented actor2/3, shake or fabricated commands */
}
static int round10_command_valid(const cJSON *c)
{
    const cJSON *op=field(c,"op"),*args=field(c,"args");
    if(!cJSON_IsString(op))return 0;
    const char *name=op->valuestring;unsigned n=(unsigned)cJSON_GetArraySize(args),a=num(args,0);
    if(!strcmp(name,"finish")||!strcmp(name,"end-step")||!strcmp(name,"skip-toggle")||!strcmp(name,"clear")||!strcmp(name,"shake-stop")||!strcmp(name,"text"))return n==0;
    if(!strcmp(name,"step")||!strcmp(name,"sound")||!strcmp(name,"chr")||!strcmp(name,"fade")||!strcmp(name,"sprite-split")||!strcmp(name,"call")||!strcmp(name,"return"))return n==1;
    if(!strcmp(name,"bands"))return n==1&&(a&63)<14;
    if(!strcmp(name,"shake"))return n==1&&a==64; /* only independently derived used mode */
    if(!strcmp(name,"image"))return (a&128)?n==1:n==2&&a<8&&num(args,1)>0&&num(args,1)<=32;
    if(!strcmp(name,"palette-cycle"))return a?n==2&&a<=2&&num(args,1)<6:n==1;
    if(!strcmp(name,"image-cycle"))return a?n==2&&num(args,1)<20&&(a&15)>0:n==1;
    if(!strcmp(name,"delay")||!strcmp(name,"sprite-banks"))return n==2;
    if(!strcmp(name,"scroll"))return n==((a&128)?2u:3u);
    if(!strcmp(name,"velocity"))return n==((a&128)&&!(a&64)?5u:3u);
    if(!strcmp(name,"palettes"))return n==1+(a&15)&&(a&15)>0&&(a&15)<=((a&128)?4u:8u);
    if(!strcmp(name,"background"))return n==5&&numbers(field(c,"layout"),2048,2048,255)&&numbers(field(c,"banks"),4,4,255);
    if(!strcmp(name,"vram")) {
        if(n!=1||!cJSON_IsArray(field(c,"writes")))return 0;
        const cJSON *wr=NULL;cJSON_ArrayForEach(wr,field(c,"writes"))
            if(cJSON_GetArraySize(wr)!=3||!cJSON_IsNumber(at(wr,0))||value(at(wr,0))<0x2000||value(at(wr,0))>=0x3000||
               (value(at(wr,1))!=1&&value(at(wr,1))!=32)||!numbers(at(wr,2),1,256,255))return 0;
        return 1;
    }
    if(!strcmp(name,"object")) {
        unsigned t=a&15;if((a>>4)>=8)return 0;
        if(t==0||t==8)return n==1;
        if(t==1)return n==2&&num(args,1)<27;
        if(t==4||t==6)return n==(t==6?4u:3u)&&num(args,1)<27&&num(args,2)<44;
        return 0;
    }
    if(!strcmp(name,"objects")) {
        if(n!=5||!numbers(field(c,"banks"),2,2,255)||!cJSON_IsArray(field(c,"objects"))||cJSON_GetArraySize(field(c,"objects"))>8)return 0;
        const cJSON *obj=NULL;cJSON_ArrayForEach(obj,field(c,"objects"))
            if(!numbers(obj,4,4,255)||((num(obj,3)&15)!=0&&(num(obj,3)&15)!=8)||num(obj,2)>=208)return 0;
        return 1;
    }
    return 0;
}
static int world_actor_valid(const cJSON *args,unsigned type,unsigned offset)
{
    unsigned n=(unsigned)cJSON_GetArraySize(args);
    if(type==0||type==8)return n==offset;
    if(type==1)return n==offset+1&&num(args,offset)<27;
    if(type==2)return n==offset+2&&num(args,offset)<27;
    if(type==3)return n==offset+3&&num(args,offset)<27;
    if(type==4||type==5||type==6)return n==offset+(type==6?3u:2u)&&num(args,offset)<27&&num(args,offset+1)<46;
    return 0;
}
static int world_clear_command_valid(const cJSON *c)
{
    const cJSON *op=field(c,"op"),*args=field(c,"args");
    if(!cJSON_IsString(op))return 0;
    const char *name=op->valuestring;unsigned n=(unsigned)cJSON_GetArraySize(args);
    if(!strcmp(name,"backdrop"))return n==1;
    if(!strcmp(name,"flush"))return n==0;
    if(!strcmp(name,"object"))return n&&(num(args,0)>>4)<8&&world_actor_valid(args,num(args,0)&15,1);
    if(!strcmp(name,"objects")) {
        if(n!=5||!numbers(field(c,"banks"),2,2,255)||!cJSON_IsArray(field(c,"objects"))||cJSON_GetArraySize(field(c,"objects"))>8)return 0;
        const cJSON *obj=NULL;cJSON_ArrayForEach(obj,field(c,"objects"))
            if(!numbers(obj,4,7,255)||num(obj,2)>=208||!world_actor_valid(obj,num(obj,3)&15,4))return 0;
        return 1;
    }
    return round10_command_valid(c);
}
static int valid_data(const cJSON *root)
{
    const cJSON *id=field(root,"id");
    if(!cJSON_IsString(id)) return 0;
    int bootstrap=!strcmp(id->valuestring,"bootstrap");
    int editor=!strcmp(id->valuestring,"editor-entry");
    int round10=!strcmp(id->valuestring,"castle-round10");
    int world_clear=!strcmp(id->valuestring,"world-clear");
    int night=!strcmp(id->valuestring,"goodnight-hidden");
    int boss=!strcmp(id->valuestring,"boss-ending");
    int post=!strcmp(id->valuestring,"post-ending");
    int extended=round10||world_clear;
    unsigned animations=night?17:(world_clear||boss||post)?46:round10?44:32;
    if(!bootstrap && !editor && !extended && !night && !boss && !post && strcmp(id->valuestring,"castle-appears")) return 0;
    if(!rows(field(root,"colours"),256,3) || !rows(field(root,"velocities"),27,4) ||
       !rows(field(root,"bg_bank_sets"),256,4) || !numbers(field(root,"band_addresses"),12,12,65535)) return 0;
    if(cJSON_GetArraySize(field(root,"poses"))!=208 || cJSON_GetArraySize(field(root,"animations"))!=(int)animations ||
       cJSON_GetArraySize(field(root,"bands"))!=14 || cJSON_GetArraySize(field(root,"cycles"))!=20 ||
       cJSON_GetArraySize(field(root,"scenes"))!=(post?1:(night||boss)?2:bootstrap?3:editor?1:round10?9:10)) return 0;
    if(post) {
        if(!cJSON_IsNumber(field(root,"schema_version"))||field(root,"schema_version")->valuedouble!=1.0||
           !cJSON_IsArray(field(root,"image_cycles"))||cJSON_GetArraySize(field(root,"image_cycles"))!=8)return 0;
        for(unsigned i=0;i<8;++i)if(!numbers(at(field(root,"image_cycles"),i),(i==3||i==4)?4:0,(i==3||i==4)?4:0,255))return 0;
        static const char *const absent[]={"shake_patterns","palette_cycles","title_palette","title_colours","title_loop_ticks"};
        for(unsigned i=0;i<sizeof absent/sizeof *absent;++i)if(field(root,absent[i]))return 0;
    }
    if(boss) {
        if(!rows(field(root,"palette_cycles"),3,4)||!rows(field(root,"shake_patterns"),2,16)||
           field(root,"image_cycles")||field(root,"title_palette")||field(root,"title_colours")||field(root,"title_loop_ticks")||
           !cJSON_IsNumber(field(root,"schema_version"))||field(root,"schema_version")->valuedouble!=1.0)return 0;
    }
    if(night) {
        static const char *const absent[]={"image_cycles","shake_patterns","palette_cycles","title_palette","title_colours","title_loop_ticks"};
        for(unsigned i=0;i<sizeof absent/sizeof *absent;++i)if(field(root,absent[i]))return 0;
        if(!cJSON_IsNumber(field(root,"schema_version"))||field(root,"schema_version")->valuedouble!=1.0)return 0;
    }
    if(extended && (!rows(field(root,"palette_cycles"),3,4) ||
       !rows(field(root,"shake_patterns"),2,16) || !rows(field(root,"image_cycles"),8,64))) return 0;
    if(extended || night || boss || post) {
        const cJSON *sha=field(field(root,"source"),"rom_sha256");
        if(!cJSON_IsString(sha)||strcmp(sha->valuestring,"853646c50889dfb7a74c2acf7cf3b76f42d044745433f706dca33f47d826cd1d"))return 0;
    }
    if(editor || extended || night || boss || post) {
        if(!numbers(field(root,"text_addresses"),4,4,16383)) return 0;
        for(unsigned i=0;i<4;++i) if(num(field(root,"text_addresses"),i)<0x2000 || num(field(root,"text_addresses"),i)>=0x2800) return 0;
    }
    if(bootstrap && (!numbers(field(root,"text_addresses"),4,4,16383) ||
       !rows(field(root,"palette_cycles"),3,4) ||
       !numbers(field(root,"title_palette"),32,32,255) || !numbers(field(root,"title_colours"),7,7,255) ||
       !cJSON_IsNumber(field(root,"title_loop_ticks")) || !value(field(root,"title_loop_ticks")) || value(field(root,"title_loop_ticks"))>65535)) return 0;
    for(unsigned i=0;i<208;++i) {
        const cJSON *pose=at(field(root,"poses"),i);
        if(!rows(pose,(unsigned)cJSON_GetArraySize(pose),4) || cJSON_GetArraySize(pose)>64) return 0;
    }
    for(unsigned i=0;i<animations;++i) {
        const cJSON *anim=at(field(root,"animations"),i);
        if(!rows(anim,(unsigned)cJSON_GetArraySize(anim),2) || cJSON_GetArraySize(anim)>256) return 0;
        if(night || post) {
            if(!cJSON_GetArraySize(anim))return 0;
            for(unsigned k=0;k<(unsigned)cJSON_GetArraySize(anim);++k)if(num(at(anim,k),1)>=208)return 0;
        }
    }
    for(unsigned i=0;i<14;++i) if(!numbers(at(field(root,"bands"),i),7,64,255)) return 0;
    for(unsigned i=0;i<20;++i) {
        const cJSON *cycle=at(field(root,"cycles"),i);
        if(!numbers(field(cycle,"values"),64,64,255) || !numbers(field(cycle,"indices"),8,8,255)) return 0;
    }
    for(unsigned i=0;i<(unsigned)cJSON_GetArraySize(field(root,"scenes"));++i) {
        const cJSON *scene=at(field(root,"scenes"),i), *program=field(scene,"program");
        int n=cJSON_GetArraySize(program);
        const cJSON *index=field(scene,"index");
        if(editor && (!cJSON_IsNumber(index) || index->valuedouble!=29.0)) return 0;
        unsigned expected=post?7:boss?26+i:night?(i?5:31):bootstrap?(i==2?30:i):editor?29:round10?8+2*i:world_clear?(i<9?9+2*i:42):32+i;
        if(value(field(scene,"index"))!=expected || !cJSON_IsArray(program) || n<1 || n>20000) return 0;
        if(post) {
            const cJSON *end=field(at(program,(unsigned)n-1),"op");
            if(!cJSON_IsString(end)||strcmp(end->valuestring,"finish"))return 0;
        }
        if(round10 && (!cJSON_IsNumber(index) || index->valuedouble!=(double)(8+2*i))) return 0;
        if(world_clear && (!cJSON_IsNumber(index)||index->valuedouble!=(double)expected))return 0;
        if(night && (!cJSON_IsNumber(index)||index->valuedouble!=(double)expected))return 0;
        if((boss||post) && (!cJSON_IsNumber(index)||index->valuedouble!=(double)expected))return 0;
        if(bootstrap || editor || extended || night || boss || post) {
            const cJSON *text=field(scene,"text"),*c=NULL;
            if(!cJSON_IsArray(text) || cJSON_GetArraySize(text)>20000) return 0;
            if(editor) {
                if(cJSON_GetArraySize(text)<1) return 0;
                const cJSON *last=field(at(text,(unsigned)cJSON_GetArraySize(text)-1),"op");
                if(!cJSON_IsString(last) || strcmp(last->valuestring,"stop")) return 0;
            }
            cJSON_ArrayForEach(c,text) {
                const cJSON *op=field(c,"op");
                if(!cJSON_IsString(op) || !numbers(field(c,"args"),0,256,255)) return 0;
                static const char *const names[]={"glyph","speed","pause-short","line","pause","upload","erase","font-next","font-reset","stop"};
                unsigned found=0;
                for(unsigned k=0;k<sizeof names/sizeof *names;++k) if(!strcmp(op->valuestring,names[k])) found=1;
                if(!found) return 0;
                if(extended || night || boss || post) {
                    unsigned count=(unsigned)cJSON_GetArraySize(field(c,"args"));
                    const char *name=op->valuestring;
                    if(!strcmp(name,"glyph")) { if(count!=1||num(field(c,"args"),0)>127)return 0; }
                    else if(!strcmp(name,"speed")||!strcmp(name,"pause-short")||!strcmp(name,"pause")) {if(count!=1)return 0;}
                    else if(!strcmp(name,"upload")) {if(count<3||count!=3+num(field(c,"args"),0))return 0;}
                    else if(count)return 0;
                }
                if(editor && ((strcmp(op->valuestring,"glyph") && strcmp(op->valuestring,"stop")) ||
                   !numbers(field(c,"args"),!strcmp(op->valuestring,"glyph")?1:0,
                            !strcmp(op->valuestring,"glyph")?1:0,127))) return 0;
            }
        }
        for(int k=0;k<n;++k) {
            const cJSON *c=at(program,(unsigned)k), *op=field(c,"op");
            if(!cJSON_IsString(op) || !numbers(field(c,"args"),0,256,255)) return 0;
            if(editor && !editor_command_valid(c)) return 0;
            if(round10 && !round10_command_valid(c)) return 0;
            if(world_clear && !world_clear_command_valid(c)) return 0;
            if(boss || post) {
                const char *name=op->valuestring;
                if(post&&!strcmp(name,"finish")&&k!=n-1)return 0;
                int empty_objects=!strcmp(name,"objects")&&numbers(field(c,"args"),0,0,255)&&numbers(field(c,"banks"),0,0,255)&&
                    cJSON_IsArray(field(c,"objects"))&&!cJSON_GetArraySize(field(c,"objects"));
                if((!empty_objects&&!world_clear_command_valid(c))||(!post&&!strcmp(name,"image"))||!strcmp(name,"backdrop")||
                   (!post&&!strcmp(name,"sprite-banks"))||((post||i)&&!strcmp(name,"skip-toggle"))||
                   (post&&(!strcmp(name,"shake")||!strcmp(name,"shake-stop")||!strcmp(name,"palette-cycle"))))return 0;
                if(post&&!strcmp(name,"image")) {
                    unsigned selector=num(field(c,"args"),0),n=(unsigned)cJSON_GetArraySize(field(c,"args"));
                    if(selector!=128&&(n!=2||(selector!=3&&selector!=4)||num(field(c,"args"),1)!=2))return 0;
                }
                const cJSON *args=field(c,"args");
                if(!strcmp(name,"object")&&((num(args,0)&15)!=0&&(num(args,0)&15)!=1&&(num(args,0)&15)!=4&&(num(args,0)&15)!=6&&(num(args,0)&15)!=8&&!(post&&(num(args,0)&15)==5)))return 0;
                if(!strcmp(name,"objects")) {
                    const cJSON *obj=NULL;cJSON_ArrayForEach(obj,field(c,"objects")) {
                        unsigned t=num(obj,3)&15;
                        if(t!=0&&t!=1&&t!=4&&t!=5&&t!=6&&t!=8)return 0;
                    }
                }
                if(!strcmp(name,"vram")) {
                    unsigned bytes=0;const cJSON *w=NULL;
                    cJSON_ArrayForEach(w,field(c,"writes"))bytes+=3+(unsigned)cJSON_GetArraySize(at(w,2));
                    if(bytes>255)return 0; /*0300 queue incl terminating0; never truncate records*/
                }
            }
            if(night) {
                const char *name=op->valuestring;
                if((strcmp(name,"objects")&&!round10_command_valid(c))||!strcmp(name,"image")||!strcmp(name,"shake")||!strcmp(name,"shake-stop")||!strcmp(name,"palette-cycle")||(!i&&!strcmp(name,"skip-toggle")))return 0;
                const cJSON *args=field(c,"args");
                if(!strcmp(name,"object")) {
                    unsigned t=num(args,0)&15;
                    if(t!=0&&t!=8&&t!=4)return 0;
                    if(t==4&&num(args,2)>=17)return 0;
                }
                if(!strcmp(name,"objects")) {
                    if(!numbers(args,5,5,255)||!numbers(field(c,"banks"),2,2,255)||!cJSON_IsArray(field(c,"objects"))||cJSON_GetArraySize(field(c,"objects"))>8)return 0;
                    const cJSON *obj=NULL;
                    cJSON_ArrayForEach(obj,field(c,"objects")) {
                        unsigned t=num(obj,3)&15;
                        if(t==1) {if(!numbers(obj,5,5,255)||num(obj,4)>=27)return 0;}
                        else if((t!=0&&t!=8)||!numbers(obj,4,4,255))return 0;
                        if(num(obj,2)>=208)return 0;
                    }
                }
            }
            const char *name=op->valuestring;
            if(!strcmp(name,"background") && (!numbers(field(c,"layout"),2048,2048,255) || !numbers(field(c,"banks"),4,4,255))) return 0;
            if(!strcmp(name,"objects") && (!cJSON_IsArray(field(c,"objects")) || cJSON_GetArraySize(field(c,"objects"))>8 || !numbers(field(c,"banks"),cJSON_GetArraySize(field(c,"args"))?2:0,cJSON_GetArraySize(field(c,"args"))?2:0,255))) return 0;
        }
    }
    return 1;
}

od_status od_cinematic_load(od_cinematic_data **out, const char *path)
{
    if(!out || !path) return OD_ERR_FORMAT;
    *out = NULL;
    FILE *f = fopen(path,"rb");
    if (!f) return OD_ERR_IO;
    if (fseek(f,0,SEEK_END)) { fclose(f); return OD_ERR_IO; }
    long n = ftell(f);
    if (n <= 0 || n > 4000000 || fseek(f,0,SEEK_SET)) { fclose(f); return OD_ERR_FORMAT; }
    char *text = malloc((size_t)n+1);
    if (!text) { fclose(f); return OD_ERR_IO; }
    size_t got = fread(text,1,(size_t)n,f); fclose(f); text[got]=0;
    od_status status=got==(size_t)n?od_cinematic_load_text(out,text,got):OD_ERR_FORMAT;
    free(text);
    return status;
}
od_status od_cinematic_load_text(od_cinematic_data **out,const char *text,size_t size)
{
    if(!out) return OD_ERR_FORMAT;
    *out=NULL;
    if(!text || !size || size>4000000 || memchr(text,0,size)) return OD_ERR_FORMAT;
    char *copy=malloc(size+1);
    if(!copy) return OD_ERR_IO;
    memcpy(copy,text,size); copy[size]=0;
    cJSON *root=cJSON_ParseWithLengthOpts(copy,size+1,NULL,1);
    free(copy);
    if (!root || value(field(root,"schema_version")) != 1 || !valid_data(root)) {
        cJSON_Delete(root); return OD_ERR_FORMAT;
    }
    od_cinematic_data *d = malloc(sizeof *d);
    if (!d) { cJSON_Delete(root); return OD_ERR_IO; }
    d->root=root; *out=d; return OD_OK;
}
void od_cinematic_free(od_cinematic_data *d) { if(d) { cJSON_Delete(d->root); free(d); } }
od_status od_cinematic_scene_index(const od_cinematic_data *d,unsigned scene,unsigned *index)
{
    if(!d || !index) return OD_ERR_FORMAT;
    const cJSON *entry=at(field(d->root,"scenes"),scene);
    if(!entry) return OD_ERR_FORMAT;
    *index=value(field(entry,"index")); return OD_OK;
}

void od_cinematic_begin(od_cinematic *s, const od_cinematic_data *data, unsigned world)
{
    memset(s,0,sizeof *s); s->data=data; s->scene=world; s->control=0x88;
    s->render_enabled=1;
    s->initial_flushes=value(field(table(s,"scenes",world),"index"))>=32?2:0;
    if(value(field(table(s,"scenes",world),"index"))==30) s->initial_flushes=1;
    if(staged_entry(s)) { s->entry_stage=ENTRY_INIT; s->initial_flushes=0; }
    const cJSON *id=field(data->root,"id");
    if(cJSON_IsString(id)&&(!strcmp(id->valuestring,"goodnight-hidden")||!strcmp(id->valuestring,"boss-ending")||!strcmp(id->valuestring,"post-ending")))s->sounds.managed=1;
    for(unsigned i=0;i<32;++i) s->palette[i]=s->pending_palette[i]=15;
    memset(s->tiles+960,255,64); memset(s->tiles+1984,255,64);
    for(unsigned i=0;i<4;++i) s->band_control[i]=0x88;
}
void od_cinematic_begin_reset(od_cinematic *s,const od_cinematic_data *data)
{
    od_cinematic_begin(s,data,0);
    /* The cold entry runs its reset stages before the first request: f0 enters the
     * reset path, f3 runs the RAM/mapper/task set-up and f4 enables the display and vblank pass and
     * enters the script. Four simulation ticks of reset, not a frame offset: the
     * two status observations plus the tasks they gate. The repeat re-entry
     * (`begin`) has none of them. */
    s->reset_sync=4;
    s->entry_stage=ENTRY_INIT;
    s->render_enabled=0;
    /* Native reset clears its own logical state. Do not copy the reference's
     * undefined power-on CIRAM, OAM or palette pattern. */
    memset(s->tiles,0,sizeof s->tiles);
    memset(s->palette,0,sizeof s->palette);
    memset(s->pending_palette,0,sizeof s->pending_palette);
}

/* Eight palette triples and shared backdrop. Palette uploads become visible at
 * the following presentation tick, like the VRAM queue. */
static void palette_queue(od_cinematic *s)
{
    for(unsigned i=0;i<8;++i) {
        const cJSON *col=table(s,"colours",s->palettes[i]);
        s->pending_palette[4*i]=s->backdrop;
        for(unsigned j=0;j<3;++j) s->pending_palette[4*i+1+j]=(uint8_t)num(col,j);
    }
}
static void actor_set(od_cinematic *s, od_cinematic_actor *a, unsigned t, const cJSON *args, unsigned offset)
{
    a->type=(uint8_t)t;
    if(!t) return;
    unsigned velocity=t==8?0:num(args,offset++);
    if(t==8) a->type=1;
    const cJSON *v=table(s,"velocities",velocity);
    a->vx=(int16_t)(num(v,0)|(num(v,1)<<8));
    a->vy=(int16_t)(num(v,2)|(num(v,3)<<8));
    if(t==2||t==6) { /* EU87E4: reversal period, NOT an animation index. */
        unsigned period=num(args,offset+(t==6?1u:0u));
        a->reverse_axes=(uint8_t)((period>>1)&24);
        a->reverse_clock=(uint8_t)((period&15)*17);
    }
    if(t==3) { /* EU880B: acceleration operands consumed after velocity. */
        a->acceleration_x=(int8_t)num(args,offset);
        a->acceleration_y=(int8_t)num(args,offset+1);
    }
    if(t>=4 && t!=8) {
        a->animation=(uint8_t)num(args,offset++); a->hold=0; a->phase=0;
    }
}
static void actors_step(od_cinematic *s)
{
    s->pending_oam[0]=0;
    for(unsigned i=0;i<8;++i) {
        od_cinematic_actor *a=&s->actors[i]; if(!a->type) continue;
        unsigned x=(unsigned)a->fx+(uint16_t)a->vx;
        unsigned y=(unsigned)a->fy+(uint16_t)a->vy;
        /* Fixed-point addition, sign of velocity determines wrap flags. */
        int dx=(int8_t)((uint16_t)a->vx>>8)+((a->fx+(uint8_t)a->vx)>255);
        int dy=(int8_t)((uint16_t)a->vy>>8)+((a->fy+(uint8_t)a->vy)>255);
        int sx=(int)a->x+dx, sy=(int)a->y+dy;
        a->flags^=(uint8_t)((sx<0||sx>255)?1:0);
        a->flags^=(uint8_t)((sy<0||sy>255)?2:0);
        a->x=(uint8_t)sx; a->y=(uint8_t)sy; a->fx=(uint8_t)x; a->fy=(uint8_t)y;
        if(a->type>=4 && ((a->type!=5 && a->type!=7) || s->text_font)) {
            if((a->type==5 || a->type==7) && (s->text_font&128 || s->text_pause)) {
                a->phase=0; if(s->text_font&128) s->text_font=0;
                a->hold=0;
            }
            if(a->hold) --a->hold;
            else {
                const cJSON *seq=table(s,"animations",a->animation);
                const cJSON *key=at(seq,a->phase);
                a->hold=(uint8_t)num(key,0); a->pose=(uint8_t)num(key,1);
                if(++a->phase >= cJSON_GetArraySize(seq)) a->phase=0;
            }
        }
        if(a->type==3) { /* EU8BC2: signed8 acceleration after movement. */
            a->vx=(int16_t)(a->vx+a->acceleration_x);
            a->vy=(int16_t)(a->vy+a->acceleration_y);
        }
        if(a->type==2||a->type==6) { /* EU8B74; actor6 animates first. */
            if(a->reverse_clock&15) --a->reverse_clock;
            else {
                if(!(a->reverse_axes&16)) a->vx=(int16_t)(-(int32_t)a->vx);
                if(!(a->reverse_axes&8)) a->vy=(int16_t)(-(int32_t)a->vy);
                a->reverse_clock|=a->reverse_clock>>4;
            }
        }
        const cJSON *pose=table(s,"poses",a->pose);
        for(int j=0;j<cJSON_GetArraySize(pose);++j) {
            const cJSON *piece=at(pose,(unsigned)j);
            int dxp=(int8_t)num(piece,0), dyp=(int8_t)num(piece,1);
            if(a->flags&128) dxp=-dxp;
            int px=a->x+dxp, py=a->y+dyp;
            if((((px<0||px>255)?1:0)^(a->flags&1)) ||
               (((py<0||py>255)?2:0)^(a->flags&2)) || (uint8_t)py>=239) continue;
            unsigned n=s->pending_oam[0]; if(n>=64) break;
            unsigned attr=num(piece,3)|(a->flags&32);
            if(a->flags&128) attr^=64;
            s->pending_oam[1+4*n]=(uint8_t)px;
            s->pending_oam[2+4*n]=(uint8_t)py+1u;
            s->pending_oam[3+4*n]=(uint16_t)num(piece,2);
            s->pending_oam[4+4*n]=(uint16_t)attr;
            s->pending_oam[0]=n+1;
        }
    }
}

static void bands_set(od_cinematic *s, unsigned a)
{
    /* Reload rows/fractions/origins but preserve the horizontal scroll registers.
     * Begin/reset zero them; only scroll/motion changes them later. */
    s->band_mode=0; memset(s->band_row,0,4);
    memset(s->band_speed,0,sizeof s->band_speed); memset(s->band_origin,0,4);
    memset(s->band_fraction,0,4); memset(s->band_delta,0,4);
    memset(s->global_fraction,0,2); memset(s->global_speed,0,sizeof s->global_speed);
    memset(s->global_acceleration,0,2);
    if(!a) return;
    const cJSON *b=table(s,"bands",a&63);
    s->global_speed[0]=(int16_t)(num(b,0)|(num(b,1)<<8));
    unsigned n=num(b,2)&7; if(n>4) n=4;
    s->band_mode=(uint8_t)(a>>6); s->band_cut=(uint8_t)((num(b,2)>>6)&3);
    for(unsigned i=0;i<n;++i) {
        s->band_row[i]=(uint8_t)num(b,3+3*i);
        s->band_speed[i]=(int16_t)(num(b,4+3*i)|(num(b,5+3*i)<<8));
    }
    unsigned p=3+3*n;
    if(s->band_mode==2) {
        s->global_speed[1]=(int16_t)(num(b,p)|(num(b,p+1)<<8));
        p+=2; s->band_period=(uint8_t)num(b,p++); s->band_clock=s->band_period;
        for(unsigned i=0;i<n;++i) { s->band_origin[i]=(uint8_t)num(b,p++); s->band_delta[i]=(uint8_t)num(b,p++); }
    }
    unsigned chr=num(b,p);
    const cJSON *banks=table(s,"bg_bank_sets",chr);
    for(unsigned i=0;i<4;++i) s->split_banks[i]=(uint8_t)num(banks,i);
}
/* Name-table address to the two-page tile store. The original's cartridge
 * mirroring decides which pages $2800-$2FFF fold onto: with the horizontal
 * arrangement (chr_mode) tables 2 and 3 are page 1, otherwise tables 1 and 3. */
#define NT_IDX(s,a) ((((a)-0x2000u)&0x3ffu)|((s)->chr_mode?(((a)-0x2000u)>>1)&0x400u:((a)-0x2000u)&0x400u))
/* paced glyph uploads, dictionary already expanded in content. */
static void text_step(od_cinematic *s)
{
    if(!s->text_active) return;
    if(s->text_wait) { --s->text_wait; return; }
    if(s->text_state) {
        unsigned address=num(field(s->data->root,"text_addresses"),0)+s->text_x;
        for(unsigned i=0;i<8;++i) s->tiles[NT_IDX(s,address+32*i)]=0;
        ++s->text_x;
        if(((address+1)&255)==0x5d) { s->text_state=0; s->text_region=s->text_x=0; }
        return;
    }
    if(s->text_pause) { --s->text_pause; s->text_wait=8; return; }
    const cJSON *stream=field(table(s,"scenes",s->scene),"text");
    s->text_wait=s->text_speed;
    for(unsigned budget=0;budget<1000;++budget) {
        const cJSON *command=at(stream,s->text_pc++),*op=field(command,"op"),*args=field(command,"args");
        if(!cJSON_IsString(op)) { s->text_active=0; return; }
        const char *name=op->valuestring; unsigned a=num(args,0);
        unsigned address=num(field(s->data->root,"text_addresses"),s->text_region&3)+s->text_x;
        if(!strcmp(name,"glyph")) {
            s->tiles[NT_IDX(s,address)]=0;
            s->tiles[NT_IDX(s,address+32)]=(uint8_t)a;
            ++s->text_x; s->text_wait=s->text_speed; return;
        }
        if(!strcmp(name,"line")) { ++s->text_region; s->text_x=0; return; }
        if(!strcmp(name,"speed")) { s->text_speed=(uint8_t)a; return; }
        if(!strcmp(name,"pause-short")) { s->text_pause=(uint8_t)a; return; }
        if(!strcmp(name,"pause")) {
            s->text_region=0; s->text_x=0; s->text_wait=(uint8_t)a;
            s->text_state=1; return;
        }
        if(!strcmp(name,"upload")) {
            address=(num(args,1)|(num(args,2)<<8))+32;
            for(unsigned i=0;i<a;++i) s->tiles[NT_IDX(s,address+i)]=(uint8_t)num(args,i+3);
            return;
        }
        if(!strcmp(name,"font-next")) { ++s->text_font; return; }
        if(!strcmp(name,"font-reset")) { s->text_font=128; return; }
        if(!strcmp(name,"stop")) { s->text_active=0; return; }
        if(!strcmp(name,"erase")) { s->text_state=1; s->text_region=s->text_x=0; return; }
    }
}

static void dynamic_step(od_cinematic *s)
{
    if(s->vram_pending) {
        /* EU89FD walks until source terminator; no16-record truncation. */
        const cJSON *c=at(field(table(s,"scenes",s->scene),"program"),s->vram_command),*w=NULL;
        cJSON_ArrayForEach(w,field(c,"writes")) {
            unsigned p=num(w,0),inc=num(w,1);const cJSON *v=at(w,2);
            for(unsigned k=0;k<(unsigned)cJSON_GetArraySize(v);++k,p+=inc)s->tiles[NT_IDX(s,p)]=(uint8_t)num(v,k);
        }
        s->vram_pending=0;
    }
    if(s->band_mode) {
        for(unsigned i=0;i<2;++i) {
            s->global_speed[i]=(int16_t)(s->global_speed[i]+s->global_acceleration[i]);
            unsigned v=s->global_fraction[i]+(uint8_t)s->global_speed[i];
            int delta=(int8_t)((uint16_t)s->global_speed[i]>>8)+(v>255);
            s->global_fraction[i]=(uint8_t)v;
            if(i==0) {
                int x=s->scroll_x+delta; if(x<0||x>255) s->control^=1; s->scroll_x=(uint8_t)x;
            } else {
                int y=s->scroll_y+delta;
                if(y<0) { y+=240; s->control^=2; }
                else if(y>=240) { y-=240; s->control^=2; }
                s->scroll_y=(uint8_t)y;
            }
        }
        for(unsigned i=0;i<4;++i) {
            unsigned v=s->band_fraction[i]+(uint8_t)s->band_speed[i];
            int x=s->band_x[i]+(int8_t)((uint16_t)s->band_speed[i]>>8)+(v>255);
            if(x<0||x>255) s->band_control[i]^=1;
            s->band_fraction[i]=(uint8_t)v; s->band_x[i]=(uint8_t)x;
        }
        if(s->band_mode==2 && !--s->band_clock) {
            s->band_clock=s->band_period;
            for(unsigned i=0;i<4 && s->band_row[i];++i) s->band_row[i]+=s->band_delta[i];
        }
    }
    if(s->palette_dirty && !s->fade) { palette_queue(s); s->palette_dirty=0; }
    if(s->fade) {
        if(s->fade_hold) --s->fade_hold;
        else {
            s->fade_hold=s->fade_speed;
            palette_queue(s);
            s->palette_dirty=0;
            unsigned loss=s->fade_brightness;
            if(s->fade&1) loss=64-loss;
            unsigned first=s->fade&128?0:16, end=s->fade&64?32:16;
            for(unsigned i=first;i<end;++i) {
                int v=(int)s->pending_palette[i]-(int)loss;
                s->pending_palette[i]=(uint8_t)(v<0?15:v);
            }
            s->fade_brightness+=16;
            if(s->fade_brightness==80) { s->fade=0; s->fade_brightness=0; }
        }
    }
    if(s->backdrop_cycle) {
        if(s->backdrop_hold) --s->backdrop_hold;
        else {
            const cJSON *cycle=table(s,"palette_cycles",s->backdrop_frame/2);
            s->backdrop_hold=(uint8_t)num(cycle,2*s->backdrop_phase);
            s->backdrop=(uint8_t)num(cycle,2*s->backdrop_phase+1);
            if(++s->backdrop_phase==s->backdrop_cycle) s->backdrop_phase=0;
            /* This step changes only the universal entries; the fade step uploads the
             * existing fade/image-cycle palette, not freshly expanded sets. */
            for(unsigned i=0;i<32;i+=4) s->pending_palette[i]=s->backdrop;
        }
    }
    if(s->image && !(s->image&128)) { /* EU9021: CHR selector pair, not pose. */
        if(s->image_hold) --s->image_hold;
        else {
            const cJSON *sequence=table(s,"image_cycles",s->image_frame);
            s->image_hold=(uint8_t)num(sequence,2*s->image_phase);
            unsigned bank=num(sequence,2*s->image_phase+1);
            for(unsigned i=0;i<4;++i) s->bg_banks[i]=(uint8_t)num(table(s,"bg_bank_sets",bank),i);
            if(++s->image_phase==s->image_count) s->image_phase=0;
        }
    }
    if(s->shake) { /* EU8A6C..8B1B: packed signed x/y nibbles. */
        unsigned v=num(table(s,"shake_patterns",s->shake&15),s->shake_phase++&15);
        int dx=(v>>4)&7,dy=v&7;if(v&128)dx=-dx;if(v&8)dy=-dy;
        int x=s->scroll_x+dx,y=s->scroll_y+dy;
        if(x<0||x>255)s->control^=1;
        s->scroll_x=(uint8_t)x;
        if(y<0){y+=240;s->control^=2;}else if(y>=240){y-=240;s->control^=2;}s->scroll_y=(uint8_t)y;
        if(s->band_row[0])s->band_row[0]=(uint8_t)(s->band_row[0]+dy);
        for(unsigned i=0;i<4;++i){x=s->band_x[i]+dx;if(x<0||x>255)s->band_control[i]^=1;s->band_x[i]=(uint8_t)x;}
    }
    text_step(s);
    if(s->cycle) {
        if(s->cycle_hold) --s->cycle_hold;
        else {
            const cJSON *cy=table(s,"cycles",s->cycle_frame);
            const cJSON *v=field(cy,"values"), *ix=field(cy,"indices");
            unsigned n=((s->cycle&127)>>4)+1;
            unsigned p=s->cycle_phase*(n+1);
            s->cycle_hold=(uint8_t)num(v,p++);
            for(unsigned i=0;i<n;++i) {
                unsigned index=num(ix,i), colour=num(v,p++);
                if(s->cycle&128) {
                    s->pending_palette[index&31]=(uint8_t)colour;
                } else s->palettes[index&7]=(uint8_t)colour;
            }
            if(++s->cycle_phase >= (s->cycle&15)) s->cycle_phase=0;
            if(!(s->cycle&128)) palette_queue(s);
        }
    }
    actors_step(s);
}

static od_status sound_emit(od_cinematic *s,unsigned id)
{
    if(id>255) { s->sounds.overflow=1; return OD_ERR_FORMAT; }
    /* The `sound` opcode writes the request byte directly, and the display pass
     * reads that single byte once per frame. A frame with two writes therefore
     * exposes only the last one to the driver. The former append kept every
     * intermediate write, which the reference never records: the boss program's
     * index 82 `2` (before the shared call's `69`) at f21073 and castle-round10
     * scene 24's `110` before `69` were both phantom entries. The init/finish/skip
     * writes are the same register, so the last one wins. */
    s->sounds.ids[0]=(uint8_t)id;
    s->sounds.count=1;
    s->sound=id; /* compatibility diagnostic; not authoritative for events */
    return OD_OK;
}
static void round10_cleanup(od_cinematic *s)
{
    const cJSON *id=field(s->data->root,"id");
    if(!cJSON_IsString(id)||(strcmp(id->valuestring,"castle-round10")&&strcmp(id->valuestring,"world-clear")&&strcmp(id->valuestring,"goodnight-hidden")&&strcmp(id->valuestring,"boss-ending")))return;
    if(!strcmp(id->valuestring,"world-clear")||!strcmp(id->valuestring,"boss-ending")||(!strcmp(id->valuestring,"goodnight-hidden")&&s->scene==1)) {
        /* EU81C9 A=C0 calls8863 directly: enqueue all32 colour0F, not
         * a timed fade.81DD/8768 also restores the presentation publisher. */
        memset(s->pending_palette,15,sizeof s->pending_palette);s->render_enabled=1;
        /* 81D4 ->82F2 publishes palette BEFORE 81D7 ->81DD clears VRAM/OAM.
         * Two distinct resources/publications, not a calibrated extra wait. */
        s->cleanup_stage=1;
        s->vram_pending=0;
        s->text_active=s->fade=s->cycle=s->image=s->shake=s->backdrop_cycle=0;
        return;
    }
    /* EU81CF/81DD: all presentation jobs cease before returning to caller. */
    memset(s->actors,0,sizeof s->actors);memset(s->pending_oam,0,sizeof s->pending_oam);
    memset(s->tiles,0,sizeof s->tiles);memset(s->tiles+960,255,64);memset(s->tiles+1984,255,64);
    s->text_active=s->fade=s->cycle=s->image=s->shake=s->backdrop_cycle=0;
}
od_status od_cinematic_skip(od_cinematic *s)
{
    if(!s || !s->data || !s->skip_enabled) return OD_ERR_FORMAT;
    const cJSON *family=field(s->data->root,"id");
    if(cJSON_IsString(family)&&!strcmp(family->valuestring,"post-ending"))return OD_ERR_FORMAT;
    if(s->finished) return OD_OK;
    memset(s->actors,0,sizeof s->actors);
    const cJSON *id=field(s->data->root,"id");
    if(!cJSON_IsString(id)||(strcmp(id->valuestring,"world-clear")&&strcmp(id->valuestring,"goodnight-hidden")&&strcmp(id->valuestring,"boss-ending")))memset(s->pending_oam,0,sizeof s->pending_oam);
    s->text_active=0; s->fade=0; s->palette_dirty=0; s->render_enabled=1;
    for(unsigned i=0;i<32;++i) s->pending_palette[i]=15;
    round10_cleanup(s);
    s->finished=1;
    return sound_emit(s,1);
}
od_status od_cinematic_validate_candidate(const od_cinematic *s,const od_cinematic_data *owner)
{
    if(!s || !owner || s->data!=owner) return OD_ERR_FORMAT;
    const cJSON *scene=at(field(owner->root,"scenes"),s->scene);
    if(!scene) return OD_ERR_FORMAT;
    unsigned commands=(unsigned)cJSON_GetArraySize(field(scene,"program"));
    unsigned text=(unsigned)cJSON_GetArraySize(field(scene,"text"));
    if(s->pc>commands || s->text_pc>text || s->flush>2 || s->started>64 || s->entry_stage>ENTRY_CLEAR ||
       s->reset_sync>4 || s->render_enabled>1 || s->finished>1 || s->skip_enabled>1 || s->cleanup_stage>3 ||
       s->sounds.managed>1 || s->sounds.count>8 || s->sounds.overflow>1 ||
       s->pending_oam[0]>64 || s->text_region>3 || s->text_active>1 ||
        s->text_state>2 || s->text_x>32 || s->band_cut>4) return OD_ERR_FORMAT;
    const cJSON *id=field(owner->root,"id");
    int night=cJSON_IsString(id)&&!strcmp(id->valuestring,"goodnight-hidden");
    int boss=cJSON_IsString(id)&&!strcmp(id->valuestring,"boss-ending");
    int post=cJSON_IsString(id)&&!strcmp(id->valuestring,"post-ending");
    if(post&&(!s->sounds.managed||s->cleanup_stage||s->skip_enabled||s->shake||s->backdrop_cycle||
       (s->image!=0&&s->image!=3&&s->image!=4&&s->image!=128)||
       ((s->image==3||s->image==4)&&s->image_frame!=s->image)||
       s->vram_command>=commands||s->transfer_command>=commands||
       (s->image_count&&((s->image_frame!=3&&s->image_frame!=4)||s->image_count!=2||s->image_phase>=2))||
       (s->finished&&s->pc!=commands)||(!s->finished&&s->pc==commands)))return OD_ERR_FORMAT;
    if(s->vram_pending>1||(!(boss||post)&&s->vram_pending))return OD_ERR_FORMAT;
    if(s->vram_pending) {
        const cJSON *c=at(field(scene,"program"),s->vram_command),*op=field(c,"op");
        if(!cJSON_IsString(op)||strcmp(op->valuestring,"vram")||s->vram_command>=s->pc||s->finished)return OD_ERR_FORMAT;
    }
    if(boss&&(!s->sounds.managed||s->image||(s->scene&&s->skip_enabled)))return OD_ERR_FORMAT;
    if(s->cleanup_stage&&(!cJSON_IsString(id)||(strcmp(id->valuestring,"world-clear")&&!boss&&!(night&&s->scene==1))||!s->finished))return OD_ERR_FORMAT;
    if(night&&(!s->sounds.managed||(!s->scene&&(s->skip_enabled||s->cleanup_stage))||s->image||s->shake||s->backdrop_cycle))return OD_ERR_FORMAT;
    if(night&&((s->finished&&!s->scene&&s->pc!=commands)||(!s->finished&&s->pc==commands)))return OD_ERR_FORMAT;
    if(cJSON_IsString(id) && (!strcmp(id->valuestring,"castle-round10")||!strcmp(id->valuestring,"world-clear")||night||boss||post)) {
        int world_clear=!strcmp(id->valuestring,"world-clear");
        if((s->shake && s->shake!=64) || s->image_frame>=8 || s->image_count>32 ||
           (s->image_count && s->image_phase>=s->image_count) || s->band_mode>2 ||
            s->cycle_frame>=20 || s->backdrop_frame>=6) return OD_ERR_FORMAT;
        if(post&&s->image&&s->image!=128&&
           ((s->image_frame!=3&&s->image_frame!=4)||s->image_count!=2||s->image_phase>=2))return OD_ERR_FORMAT;
        for(unsigned i=0;i<8;++i) {
            const od_cinematic_actor *a=&s->actors[i];
            if(night&&a->type!=0&&a->type!=1&&a->type!=4)return OD_ERR_FORMAT;
            if(a->type!=0&&a->type!=1&&a->type!=4&&a->type!=6&&!((world_clear||boss||post)&&(a->type==2||a->type==3||a->type==5)))return OD_ERR_FORMAT;
            if((boss||post)&&(a->type==2||a->type==3))return OD_ERR_FORMAT;
            if(a->pose>=208 || a->animation>=(night?17:(world_clear||boss||post)?46:44) || (a->reverse_axes&~24))return OD_ERR_FORMAT;
            if(a->type>=4) {
                const cJSON *seq=at(field(owner->root,"animations"),a->animation);
                if(a->phase>=(unsigned)cJSON_GetArraySize(seq))return OD_ERR_FORMAT;
            }
        }
    }
    if(s->flush==2 && staged_entry(s)) {
        const cJSON *command=at(field(scene,"program"),s->transfer_command);
        const cJSON *op=field(command,"op");
        if(!cJSON_IsString(op) || strcmp(op->valuestring,"background")) return OD_ERR_FORMAT;
    }
    for(unsigned i=0;i<s->pending_oam[0];++i) {
        const uint16_t *p=s->pending_oam+1+4*i;
        if(p[0]>255 || p[1]>256 || p[2]>8191 || p[3]>255) return OD_ERR_FORMAT;
    }
    return OD_OK;
}

od_status od_cinematic_take_sounds(od_cinematic *s,od_cinematic_sounds *out)
{
    if(!s || !out) return OD_ERR_FORMAT;
    *out=s->sounds;
    s->sounds.count=0;
    return out->overflow?OD_ERR_FORMAT:OD_OK;
}

static od_status background_apply(od_cinematic *s,const cJSON *c)
{
    const cJSON *layout=field(c,"layout"), *args=field(c,"args");
    if(cJSON_GetArraySize(layout)!=2048) return OD_ERR_FORMAT;
    for(unsigned i=0;i<2048;++i) s->tiles[i]=(uint8_t)num(layout,i);
    s->backdrop=(uint8_t)num(args,1);
    for(unsigned i=0;i<4;++i) s->bg_banks[i]=s->split_banks[i]=(uint8_t)num(field(c,"banks"),i);
    for(unsigned i=0;i<3;++i) s->palettes[i]=(uint8_t)num(args,4-i);
    s->palettes[3]=15;
    s->palette_dirty=1;
    return OD_OK;
}

static od_status commands(od_cinematic *s)
{
    const cJSON *program=field(table(s,"scenes",s->scene),"program");
    for(unsigned limit=0;limit<1000;++limit) {
        const cJSON *c=at(program,s->pc++), *args=field(c,"args");
        const cJSON *op=field(c,"op");
        if(!cJSON_IsString(op)) return OD_ERR_FORMAT;
        const char *name=op->valuestring; unsigned a=num(args,0);
        if(!strcmp(name,"step")) s->delay=a;
        else if(!strcmp(name,"end-step")) {
            /* A batch publishes explicit palette commands after it ends; object setup
             * clears that request, fade has its own queue. */
            if(s->palette_dirty && (s->palette_dirty==2 || !s->fade)) {
                palette_queue(s); s->palette_dirty=0;
            }
            return OD_OK;
        }
        else if(!strcmp(name,"finish")) {
            s->finished=1;
            /* The title scene returns (top bit set) instead of running the non-returning
             * cleanup; no stop request in the input loop. */
            const cJSON *id=field(s->data->root,"id");
            if(s->next_scene || value(field(table(s,"scenes",s->scene),"index"))==30 ||
               (cJSON_IsString(id)&&!strcmp(id->valuestring,"post-ending")) ||
               (cJSON_IsString(id)&&!strcmp(id->valuestring,"goodnight-hidden")&&s->scene==0)) return OD_OK;
            /* Cleanup after the final producer boundary; next tick publishes
             * the emptied jobs before the caller handoff. */
            round10_cleanup(s);
            return sound_emit(s,1);
        } /* cleanup; the caller handles next_scene as a scene transition */
        else if(!strcmp(name,"call")||!strcmp(name,"return")) { /* statically resolved source flow */ }
        else if(!strcmp(name,"flush")) { s->flush=1; return OD_OK; }
        else if(!strcmp(name,"background")) {
            if(staged_entry(s)) {
                /* The transfer runs inline in this frame; only a transfer whose clear+block
                 * work exceeds one PAL frame is held. The attract bootstrap's small transfers
                 * (N <= 17 metatiles) are the ones the reference executes inside the step
                 * frame, and so are the 4-metatile transfers of the editor entry (scene 29) and of
                 * the hidden-access cinematic (scene 5): the reference runs their batch 0 (sound 57,
                 * the two stars) in the step frame and the stars move 32 frames (R7: counter
                 * 31 at f1777, OAM f1778..f1809; C6.28 D2, tests/test_cinematic_stars.c). A
                 * batch that carries its own explicit `flush` keeps the held path below (boss
                 * scene 26 is exact only that way). */
                if(background_metatiles(c)<=17u &&
                   (bootstrap(s) || !batch_has_flush(program,s->pc))) {
                    od_status st=background_apply(s,c); if(st) return st;
                    continue;
                }
                /* Presentation and the vblank pass are disabled while the native background job owns VRAM.
                 * Store the semantic command index, not CPU state. No actors or palette
                 * publication run inside this transfer. Each transfer keeps the vblank pass off across a
                 * main-loop frame, so the transfer occupies one held frame per block. `started`
                 * is the pending transfer length in frames (never CPU state); zero means no
                 * staged transfer is pending.
                 *
                 * The main loop runs the remaining commands of the batch on the iteration AFTER
                 * the last block, not on the block's own frame. The 0x41 variant is followed by
                 * an explicit `flush`, whose own held frame supplies that iteration; the
                 * 0x42 variant (no flush) must hold it itself, and the resume frame decrements
                 * the step delay once (counter 119 at f19790, 118 at f19791 where the cue is
                 * written). The attract bootstrap enters through `begin_reset`, whose
                 * reset/transfer frames are already modelled, so it is excluded. */
                s->render_enabled=0;
                s->transfer_command=s->pc-1;
                s->started=(unsigned)cJSON_GetArraySize(field(c,"blocks"));
                if(!s->started) s->started=1;
                if(bootstrap(s)) s->started=1;
                else {
                    /* The extra iteration only exists while the step delay is
                     * still counting: with delay 0 the original completes the batch
                     * in the same iteration (castle-round10 worlds 1-4, step 0),
                     * so the transfer must not shift the program there. It is
                     * also skipped when the batch carries its own explicit
                     * `flush`, whose held frame supplies it. */
                    if(!batch_has_flush(program,s->pc) &&
                       s->delay && s->started<64u)
                        ++s->started; /* one extra held frame before the batch */
                }
            } else {
                od_status st=background_apply(s,c); if(st) return st;
                /* Remaining sprite palette fields are retained until overwritten.
                 * A non-staged background publishes before the next batch. */
            }
            s->flush=2; return OD_OK;
        } else if(!strcmp(name,"palettes")) {
            unsigned count=a&15, base=a&128?4:0;
            if(count>8-base) return OD_ERR_FORMAT;
            for(unsigned i=0;i<count;++i) s->palettes[base+i]=(uint8_t)num(args,i+1);
            s->palette_dirty=2;
        } else if(!strcmp(name,"backdrop")) { /* EU8471: colour + explicit upload */
            s->backdrop=(uint8_t)a;s->palette_dirty=2;
        } else if(!strcmp(name,"chr")) s->chr_mode=(uint8_t)a;
        else if(!strcmp(name,"objects")) {
            memset(s->actors,0,sizeof s->actors);
            if(!cJSON_GetArraySize(args)) { s->pending_oam[0]=0; continue; }
            for(unsigned i=0;i<4;++i) s->palettes[i+4]=(uint8_t)num(args,i);
            for(unsigned i=0;i<2;++i) s->sprite_banks[i]=(uint8_t)(num(field(c,"banks"),i)&254);
            const cJSON *objs=field(c,"objects");
            for(int i=0;i<cJSON_GetArraySize(objs)&&i<8;++i) {
                const cJSON *obj=at(objs,(unsigned)i); od_cinematic_actor *actor=&s->actors[i];
                unsigned f=num(obj,3);
                actor->x=(uint8_t)num(obj,0); actor->y=(uint8_t)num(obj,1); actor->pose=(uint8_t)num(obj,2);
                actor->flags=(uint8_t)((f&128)|((f&64)>>1)|((f>>4)&3));
                actor_set(s,actor,f&15,obj,4);
            }
            s->palette_dirty=1;
        } else if(!strcmp(name,"object")) {
            if((a>>4)>=8) return OD_ERR_FORMAT;
            actor_set(s,&s->actors[a>>4],a&15,args,1);
        } else if(!strcmp(name,"vram")) {
            const cJSON *id=field(s->data->root,"id");
            if(cJSON_IsString(id)&&(!strcmp(id->valuestring,"boss-ending")||!strcmp(id->valuestring,"post-ending"))) {
                if(s->vram_pending)return OD_ERR_FORMAT;
                s->vram_command=s->pc-1;s->vram_pending=1;continue;
            }
            const cJSON *writes=field(c,"writes");
            for(int i=0;i<cJSON_GetArraySize(writes);++i) {
                const cJSON *wr=at(writes,(unsigned)i), *v=at(wr,2);
                unsigned p=num(wr,0)&0x3fff, inc=num(wr,1);
                for(int j=0;j<cJSON_GetArraySize(v);++j,p+=inc) s->tiles[NT_IDX(s,p)]=(uint8_t)num(v,(unsigned)j);
            }
        } else if(!strcmp(name,"bands")) bands_set(s,a);
        else if(!strcmp(name,"scroll")) {
            if(a&128) { unsigned i=(a>>2)&3; s->band_control[i]=(s->band_control[i]&252)|(a&3); s->band_x[i]=(uint8_t)num(args,1); }
            else { s->control=(s->control&252)|(a&3); s->scroll_x=(uint8_t)num(args,1); s->scroll_y=(uint8_t)num(args,2); }
        } else if(!strcmp(name,"image-cycle")) {
            s->cycle=(uint8_t)a; s->cycle_hold=s->cycle_phase=0; s->cycle_frame=(uint8_t)num(args,1);
        } else if(!strcmp(name,"fade")) {
            s->fade=(uint8_t)a; s->fade_speed=s->fade_hold=(uint8_t)((a>>1)&31); s->fade_brightness=16;
            s->palette_dirty=0; /* fade owns the upload, clears the palette request */
            if(a&1) {
                unsigned first=a&128?0:16,end=a&64?32:16;
                for(unsigned i=first;i<end;++i) s->pending_palette[i]=15;
            }
        } else if(!strcmp(name,"sound")) { od_status st=sound_emit(s,a); if(st) return st; }
        else if(!strcmp(name,"delay")) s->delay=a|(num(args,1)<<8);
        else if(!strcmp(name,"scene")) s->next_scene=(uint8_t)a;
        else if(!strcmp(name,"sprite-split")) s->sprite_split=(uint8_t)a;
        else if(!strcmp(name,"clear")) {
            memset(s->actors,0,sizeof s->actors); s->pending_oam[0]=0;
            for(unsigned i=0;i<32;++i) s->pending_palette[i]=15;
            s->palette_dirty=0;
        } else if(!strcmp(name,"palette-cycle")) {
            s->backdrop_cycle=(uint8_t)a; s->backdrop_frame=(uint8_t)num(args,1);
            s->backdrop_phase=s->backdrop_hold=0;
        } else if(!strcmp(name,"velocity")) {
            if(a&128) {
                if(a&64) { s->global_acceleration[0]=(int8_t)num(args,1); s->global_acceleration[1]=(int8_t)num(args,2); }
                else { s->global_speed[1]=(int16_t)((num(args,1)<<8)|num(args,2)); s->global_speed[0]=(int16_t)((num(args,3)<<8)|num(args,4)); }
            } else {
                if(a&64) s->band_speed[a&3]=(int16_t)(num(args,1)|(num(args,2)<<8));
                else s->global_speed[0]=(int16_t)(num(args,1)|(num(args,2)<<8));
            }
        } else if(!strcmp(name,"text")) {
            s->text_active=1; s->text_region=s->text_wait=s->text_pause=s->text_state=s->text_x=0; s->text_speed=2;
        }
        else if(!strcmp(name,"sprite-banks")) {
            for(unsigned i=0;i<4;++i) {
                s->bg_banks[i]=(uint8_t)num(table(s,"bg_bank_sets",a),i);
                s->split_banks[i]=(uint8_t)num(table(s,"bg_bank_sets",num(args,1)),i);
            }
        } else if(!strcmp(name,"image")) {
            s->image=(uint8_t)a;
            if(!(a&128)){s->image_frame=(uint8_t)a;s->image_count=(uint8_t)num(args,1);s->image_phase=s->image_hold=0;}
        } else if(!strcmp(name,"shake")) { s->shake=(uint8_t)a; }
        else if(!strcmp(name,"shake-stop")) { s->shake=s->shake_phase=0; }
        else if(!strcmp(name,"skip-toggle")) s->skip_enabled=!s->skip_enabled;
        else return OD_ERR_FORMAT;
    }
    return OD_ERR_FORMAT;
}

static od_status measure(const od_cinematic_data *data,unsigned scene,int reset,unsigned *duration)
{
    if(!data || !duration || scene>=(unsigned)cJSON_GetArraySize(field(data->root,"scenes"))) return OD_ERR_FORMAT;
    od_cinematic s;
    if(reset) od_cinematic_begin_reset(&s,data);
    else od_cinematic_begin(&s,data,scene);
    /* The same tasks as the product, including cold entry. No duplicate timing
     * runner that omits reset/publication or adds a captured duration. */
    for(unsigned t=0;t<65535;++t) {
        uint16_t display[2382];
        od_status status=od_cinematic_tick(&s,display);
        if(status!=OD_OK) return status;
        if(s.finished) { *duration=t+2; return OD_OK; }
    }
    return OD_ERR_FORMAT;
}
od_status od_cinematic_duration(const od_cinematic_data *data,unsigned scene,unsigned *duration)
{
    return measure(data,scene,0,duration);
}
od_status od_cinematic_reset_duration(const od_cinematic_data *data,unsigned *duration)
{
    return measure(data,0,1,duration);
}

od_status od_cinematic_tick(od_cinematic *s, uint16_t d[2382])
{
    if(!s->data || s->scene>=(unsigned)cJSON_GetArraySize(field(s->data->root,"scenes"))) return OD_ERR_FORMAT;
    memset(d,0,2382*sizeof *d); s->sound=0;
    memset(&s->sounds,0,sizeof s->sounds); s->sounds.managed=1;
    int entered=0;
    if(s->reset_sync) {
        /* One PAL presentation event is supplied by each simulation tick.
         * Synchronize before initializing the cinematic engine. No CPU poll
         * phase is asserted; that remains a separate strict diagnostic. */
        --s->reset_sync; ++s->tick;
        d[2093]=1;
        return OD_OK;
    }
    if(s->entry_stage==ENTRY_INIT) {
        s->render_enabled=1;
        for(unsigned i=0;i<32;++i) s->pending_palette[i]=15;
        s->entry_stage=ENTRY_PALETTE;
        entered=1;
        od_status st=sound_emit(s,1); if(st) return st; /* initial request */
    } else if(!s->tick) { od_status st=sound_emit(s,1); if(st) return st; }
    if(s->render_enabled) memcpy(s->palette,s->pending_palette,32);
    for(unsigned i=0;i<16;i+=4) s->palette[i]=s->pending_palette[16];
    for(unsigned i=16;i<32;i+=4) s->palette[i]=0;
    for(unsigned i=0;i<32;++i) d[i]=s->palette[i];
    for(unsigned i=0;i<2048;++i) d[32+i]=s->tiles[i];
    for(unsigned i=0;i<4;++i) { d[2080+i]=(uint16_t)s->bg_banks[i]*64; d[2084+i]=(uint16_t)s->split_banks[i]*64; }
    for(unsigned i=0;i<2;++i) d[2088+i]=d[2090+i]=(uint16_t)s->sprite_banks[i]*64;
    d[2092]=s->render_enabled?30:0; d[2093]=1; d[2095]=s->scroll_x; d[2096]=s->scroll_y; d[2097]=s->control&3;
    unsigned row=s->band_row[0]?s->band_row[0]+1:240;
    for(unsigned i=0;i<4 && row<240;++i) {
        unsigned n=d[2093]++; unsigned p=2094+6*n;
        unsigned origin=s->band_origin[i];
        unsigned addr=num(field(s->data->root,"band_addresses"),origin);
        unsigned y=origin?((((addr>>5)&31)*8+((addr>>12)&7)-row+1)&255):0;
        d[p]=row; d[p+1]=s->band_x[i]; d[p+2]=y; d[p+3]=s->band_control[i]&3; d[p+4]=i>=s->band_cut; d[p+5]=1;
        if(i==3 || !s->band_row[i+1]) break;
        row+=s->band_row[i+1]+1;
    }
    d[2124]=s->pending_oam[0];
    d[2381]=s->skip_enabled | (s->chr_mode?2:0);
    if(s->sprite_split&2) d[2090]=d[2091]=0x36*64;
    if(s->sprite_split&1) d[2088]=d[2089]=0x36*64;
    memcpy(d+2125,s->pending_oam+1,256*sizeof *d);
    ++s->tick;
    if(s->render_enabled) ++s->frame_tick;
    if(s->cleanup_stage) {
        if(s->cleanup_stage==1) {
            /* The palette was just actually published; now the clear wipes VRAM and hides
             * OAM. The next vblank publisher commits this second resource. */
            memset(s->actors,0,sizeof s->actors);memset(s->pending_oam,0,sizeof s->pending_oam);
            memset(s->tiles,0,sizeof s->tiles);memset(s->tiles+960,255,64);memset(s->tiles+1984,255,64);
            s->cleanup_stage=2;
        } else if(s->cleanup_stage==2)s->cleanup_stage=3;
        return OD_OK;
    }
    if(entered) return OD_OK;
    if(s->entry_stage==ENTRY_PALETTE) {
        /* The initial full fade is published before clearing the display. Then the
         * nametables are erased and OAM hidden, and the fade is published again. */
        const cJSON *id=field(s->data->root,"id");
        int night=cJSON_IsString(id)&&!strcmp(id->valuestring,"goodnight-hidden")&&s->scene==0;
        if(value(field(table(s,"scenes",s->scene),"index"))!=30&&!night) {
            memset(s->tiles,0,sizeof s->tiles);
            memset(s->tiles+960,255,64); memset(s->tiles+1984,255,64);
            memset(s->pending_oam,0,sizeof s->pending_oam);
            s->entry_stage=ENTRY_CLEAR;
            return OD_OK;
        }
        if(night) {
            /* EU8125 bit7 skips81DD/8CA8, NOT the following82F2 publication.
             * No NT/OAM clear is performed; close the explicit palette job. */
            s->entry_stage=ENTRY_CLEAR;
            return OD_OK;
        }
        /* The returning title entry has its top bit set: it bypasses the erase. */
        s->entry_stage=ENTRY_RUN;
    }
    if(s->entry_stage==ENTRY_CLEAR) s->entry_stage=ENTRY_RUN;
    if(s->initial_flushes) { --s->initial_flushes; return OD_OK; }
    if(s->finished && value(field(table(s,"scenes",s->scene),"index"))==30) {
        ++s->exit_age;
        if(s->exit_age==2) {
            const cJSON *palette=field(s->data->root,"title_palette");
            for(unsigned i=0;i<32;++i) s->pending_palette[i]=(uint8_t)num(palette,i);
        }
        if(s->exit_age>=2 && s->exit_age<2+value(field(s->data->root,"title_loop_ticks")) && !(s->frame_tick&3)) {
            s->pending_palette[15]=(uint8_t)num(field(s->data->root,"title_colours"),s->backdrop_phase);
            s->backdrop_phase=(s->backdrop_phase+1)%3;
        }
    }
    /* The terminal loop of the script only closes the producer. Mainline jobs do
     * not advance after the end marker; the existing vblank/scanline publisher remains
     * active. */
    const cJSON *family=field(s->data->root,"id");
    int post_terminal=s->finished&&cJSON_IsString(family)&&!strcmp(family->valuestring,"post-ending");
    if(!s->flush&&!post_terminal) dynamic_step(s);
    if(!s->finished) {
        if(s->flush) {
            if(s->flush==2 && staged_entry(s)) {
                const cJSON *program=field(table(s,"scenes",s->scene),"program");
                if(!s->render_enabled) {
                    od_status st=background_apply(s,at(program,s->transfer_command));
                    if(st) return st;
                    /* Presentation is restored after the first block's job; the remaining blocks
                     * keep the transfer busy for their own main-loop frames before the producer is
                     * closed. */
                    s->render_enabled=1;
                }
                if(s->started>1) { --s->started; return OD_OK; }
                s->started=0;
                /* The extra held frame (the 0x42 variant without its own flush) resumes the
                 * batch on the next main-loop iteration, whose step-delay decrement lands on
                 * the same frame as the resumed commands (counters 118 and 25 both at f19791).
                 * The flush-carrying 0x41 variant never took it, and the bootstrap is excluded.
                 * The same guards as the command site must hold, including the 64-frame bound
                 * that can suppress the hold. */
                const cJSON *tc=at(program,s->transfer_command);
                unsigned blocks=(unsigned)cJSON_GetArraySize(field(tc,"blocks"));
                if(!blocks) blocks=1;
                if(!bootstrap(s) && !batch_has_flush(program,s->pc) && s->delay &&
                   blocks<64u)
                    --s->delay;
                /* A staged `0x41` background is immediately followed by its own explicit
                 * `flush`. That flush is the resume/publication of the same transfer, not
                 * a further main-loop frame: consume it here so the rest of the batch (and the
                 * step-delay decrement) land on the reference tick. The `0x42` variant has no
                 * flush and instead carries the extra held frame above. Without this the 0x41
                 * batch spends one frame too many, which the early boss entry had been masking
                 * (boss scene 26 index 3, counter 249 at f18317/f18318 and 248 at f18319). */
                const cJSON *next=at(program,s->pc);
                if(!bootstrap(s) && next && cJSON_IsString(field(next,"op")) &&
                   !strcmp(field(next,"op")->valuestring,"flush")) {
                    if(s->palette_dirty && !s->fade) { palette_queue(s); s->palette_dirty=0; }
                    ++s->pc;
                }
            }
            if(s->flush==1 && s->palette_dirty && !s->fade) { palette_queue(s); s->palette_dirty=0; }
            s->flush=0; od_status st=commands(s); if(st) return st;
        }
        else if(s->delay) --s->delay;
        else { od_status st=commands(s); if(st) return st; }
    }
    return OD_OK;
}
