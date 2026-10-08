/* ui.c - modern overlay interface (C5.1).
 *
 * A tiny immediate-mode rasteriser over the 256x240 NES frame. There is no
 * external widget library and no font from the game: the widgets draw into a
 * private ARGB canvas with the public-domain 8x8 font of ui_font.c, and the
 * canvas is uploaded as a blended texture on top of the scaled game frame.
 *
 * What lives here: the framework, the modern menu opened with ESC (D1, Quit
 * inside), the settings screen, the states screen (C5.3), mute/volume and the
 * on-screen toasts. The editor and the catalogue draw into their own 384x360
 * canvas (C6.24). Every text is cut or wrapped to its box (C6.32). */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "opendana/ui.h"
#include "opendana/render_sdl.h"
#if defined(__GNUC__)
void od_renderer_cursor_update(od_renderer *r) __attribute__((weak));
#endif
#include "opendana/userdir.h"
#include "opendana/ui_font.h"
#include "opendana/ui_strings.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "../../external/stb/stb_image.h"
#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* Only a host-generated slot basename is read, never a serialized path.
 * C6.32 (UI-14): the folders on the way may be aliases (Silverblue's /home,
 * a data folder moved to another disk); the file itself is never followed
 * when it is a symbolic link. */
static FILE *state_thumbnail_file(const char *path, long max_bytes)
{
#if defined(_WIN32)
    HANDLE h=(HANDLE)od_userdir_win_open_noreparse(path);   /* wide (C6.36) */
    if(h==NULL)return NULL;
    BY_HANDLE_FILE_INFORMATION info;
    if(!GetFileInformationByHandle(h,&info)||GetFileType(h)!=FILE_TYPE_DISK||
       (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||
       (max_bytes&&(info.nFileSizeHigh||info.nFileSizeLow>(DWORD)max_bytes))) {
        CloseHandle(h);return NULL;
    }
    int fd=_open_osfhandle((intptr_t)h,_O_RDONLY|_O_BINARY);
    if(fd<0){CloseHandle(h);return NULL;}
    FILE *f=_fdopen(fd,"rb");if(!f)_close(fd);return f;
#else
    char folder[1200];
    if(strlen(path)>=sizeof folder||strstr(path,"/../")||!strncmp(path,"../",3))
        return NULL;
    strcpy(folder,path);
    char *slash=strrchr(folder,'/');
    const char *base=slash?slash+1:folder;
    if(!*base||!strcmp(base,"..")||!strcmp(base,"."))return NULL;
    int dir;
    if(slash==NULL) dir=open(".",O_RDONLY|O_DIRECTORY);
    else if(slash==folder) dir=open("/",O_RDONLY|O_DIRECTORY);
    else { *slash='\0'; dir=open(folder,O_RDONLY|O_DIRECTORY); }
    if(dir<0)return NULL;
    int fd=openat(dir,path+(base-folder),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    close(dir);
    if(fd<0)return NULL;
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||(max_bytes&&st.st_size>max_bytes)) {
        close(fd);return NULL;
    }
    FILE *f=fdopen(fd,"rb");if(!f)close(fd);return f;
#endif
}
static uint32_t thumb_u32(const unsigned char *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint32_t thumb_crc(const unsigned char *p,size_t n)
{
    uint32_t c=~0u;
    for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u& (0u-(c&1)));}
    return ~c;
}
static void state_thumbnail_load(od_ui *ui,int slot,const char *state_path)
{
    char path[1200];size_t len=strlen(state_path);
    const char *suffix=".odstate";size_t tail=strlen(suffix);
    if(len<tail||strcmp(state_path+len-tail,suffix)||len-tail+5>sizeof path)return;
    memcpy(path,state_path,len-tail);strcpy(path+len-tail,".png");
    FILE *f=state_thumbnail_file(path,1024*1024);if(!f)return;
    /* A failed PNG write must not resurrect the previous save's preview.
     * Both timestamps come from opened, no-follow files, not a path stat.
     * C6.32 (UI-13): written in the same instant counts as fresh (ext4 and
     * coarse clocks give both files the same mtime); an older PNG does not. */
    FILE *state=state_thumbnail_file(state_path,0);
    if(!state){fclose(f);return;}
    int fresh=0;
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION si,pi;
    if(GetFileInformationByHandle((HANDLE)_get_osfhandle(_fileno(state)),&si)&&
       GetFileInformationByHandle((HANDLE)_get_osfhandle(_fileno(f)),&pi))
        fresh=CompareFileTime(&pi.ftLastWriteTime,&si.ftLastWriteTime)>=0;
#else
    struct stat si,pi;
    if(!fstat(fileno(state),&si)&&!fstat(fileno(f),&pi)) {
#if defined(__APPLE__)
        fresh=pi.st_mtimespec.tv_sec>si.st_mtimespec.tv_sec||
            (pi.st_mtimespec.tv_sec==si.st_mtimespec.tv_sec&&pi.st_mtimespec.tv_nsec>=si.st_mtimespec.tv_nsec);
#else
        fresh=pi.st_mtim.tv_sec>si.st_mtim.tv_sec||
            (pi.st_mtim.tv_sec==si.st_mtim.tv_sec&&pi.st_mtim.tv_nsec>=si.st_mtim.tv_nsec);
#endif
    }
#endif
    fclose(state);if(!fresh){fclose(f);return;}
    unsigned char *data=malloc(1024*1024+1),*idat=NULL,*rgba=NULL;
    char *raw=NULL;size_t n=0,used=0,pos=8;int w=0,h=0,ch=0,end=0;
    if(!data)goto done;
    n=fread(data,1,1024*1024+1,f);
    if(ferror(f)||n>1024*1024||n<45||memcmp(data,"\211PNG\r\n\032\n",8)||
       thumb_u32(data+8)!=13||memcmp(data+12,"IHDR",4)||
       data[24]!=8||data[25]!=2||data[26]||data[27]||data[28])goto done;
    w=(int)thumb_u32(data+16);h=(int)thumb_u32(data+20);
    if(w<=0||h<=0||w>256||h>240)goto done;
    /* Fixed output bounds BEFORE stb's malloc-based decoder. Exact scanline
     * length rejects small-dimension deflate bombs, not just oversized IHDR. */
    size_t expected=(size_t)h*(1+3*(size_t)w);
    idat=malloc(n);raw=malloc(expected);if(!idat||!raw)goto done;
    while(pos+12<=n) {
        uint32_t size=thumb_u32(data+pos);if(size>n-pos-12)goto done;
        if(thumb_crc(data+pos+4,(size_t)size+4)!=thumb_u32(data+pos+8+size))goto done;
        if(!memcmp(data+pos+4,"IDAT",4)){memcpy(idat+used,data+pos+8,size);used+=size;}
        else if(memcmp(data+pos+4,"IEND",4)&&
                !(pos==8&&!memcmp(data+pos+4,"IHDR",4)))goto done;
        if(!memcmp(data+pos+4,"IEND",4)){if(size||pos+12!=n)goto done;end=1;break;}
        pos+=(size_t)size+12;
    }
    if(!end||!used||stbi_zlib_decode_buffer(raw,(int)expected,(const char *)idat,(int)used)!=(int)expected)goto done;
    rgba=stbi_load_from_memory(data,(int)n,&w,&h,&ch,4);
    if(!rgba||w<=0||h<=0||w>256||h>240)goto done;
    int tw=w>=h?14:14*w/h,th=h>=w?14:14*h/w;
    if(!tw)tw=1;
    if(!th)th=1;
    for(int y=0;y<th;++y)for(int x=0;x<tw;++x) {
        const unsigned char *p=rgba+4*((y*h/th)*w+x*w/tw);
        ui->slot_thumbnails[slot].pixels[y*14+x]=0xff000000u|(uint32_t)p[0]<<16|(uint32_t)p[1]<<8|p[2];
    }
    ui->slot_thumbnails[slot].w=tw;ui->slot_thumbnails[slot].h=th;
done:
    fclose(f);stbi_image_free(rgba);free(raw);free(idat);free(data);
}

static void states_scroll_to_focus(od_ui *ui);

/* C5.3: the states screen lists the slot files. The UI borrows the slot count
 * and the current slot from the settings and peeks each file for its line. */
static void slot_path(char *out, size_t cap, int slot)
{
    od_state_default_path(out, cap, slot);
}

void od_ui_states_refresh(od_ui *ui, int current)
{
    if (ui == NULL || ui->settings == NULL)
        return;
    int slots = ui->settings->states_slots;
    if (slots < 1) slots = 1;
    if (slots > OD_SETTINGS_STATES_MAX) slots = OD_SETTINGS_STATES_MAX;
    if (current < 0) current = 0;
    if (current >= slots) current = slots - 1;
    ui->slot_focus = current;
    states_scroll_to_focus(ui);
    memset(ui->slot_thumbnails,0,sizeof ui->slot_thumbnails);
    for (int i = 0; i < OD_SETTINGS_STATES_MAX; i++) {
        ui->slot_occupied[i] = 0;
        memset(&ui->slot_info[i], 0, sizeof ui->slot_info[i]);
        if (i >= slots)
            continue;
        char path[1200];
        slot_path(path, sizeof path, i + 1);
        if (od_state_peek(path, &ui->slot_info[i]) == OD_OK &&
            ui->slot_info[i].valid) {
            ui->slot_occupied[i] = 1;
            state_thumbnail_load(ui,i,path);
        }
    }
}

void od_ui_states_status(od_ui *ui, const char *text, unsigned toast_ms)
{
    if (ui == NULL)
        return;
    if (text != NULL)
        snprintf(ui->state_status, sizeof ui->state_status, "%s", text);
    else
        ui->state_status[0] = '\0';
    if (toast_ms > 0)
        od_ui_toast(ui, text ? text : "", toast_ms);
}

/* ---- palette ------------------------------------------------------------ */
#define C_DIM     0x96000000u   /* 150/255 black: dims the frozen frame  */
#define C_PANEL   0xFF14142Cu
#define C_BORDER  0xFFB0B0D0u
#define C_TITLE   0xFFFFE080u
#define C_TEXT    0xFFE8E8F0u
#define C_SEL_BG  0xFF2E4E9Eu
#define C_SEL_TX  0xFFFFFFFFu
#define C_MUTED   0xFF9090A0u
#define C_BTN     0xFF303048u
#define C_TOAST   0xE6303040u
#define C_TOASTTX 0xFFFFFFFFu

/* ---- canvas primitives -------------------------------------------------- */

static void canvas_clear(od_ui *ui)
{
    memset(ui->canvas, 0, sizeof ui->canvas);
}

static void fill_rect(od_ui *ui, int x, int y, int w, int h, uint32_t argb)
{
    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= OD_UI_SCREEN_H)
            continue;
        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= OD_UI_SCREEN_W)
                continue;
            ui->canvas[(size_t)py * OD_UI_SCREEN_W + (size_t)px] = argb;
        }
    }
}

static void fill_dim(od_ui *ui)
{
    for (int i = 0; i < OD_UI_SCREEN_W * OD_UI_SCREEN_H; i++)
        ui->canvas[i] = C_DIM;
}

static void draw_border(od_ui *ui, int x, int y, int w, int h, uint32_t argb)
{
    fill_rect(ui, x, y, w, 1, argb);
    fill_rect(ui, x, y + h - 1, w, 1, argb);
    fill_rect(ui, x, y, 1, h, argb);
    fill_rect(ui, x + w - 1, y, 1, h, argb);
}

/* Text cut to `w` pixels with "..." (never past its box). */
static void text_in(od_ui *ui, int x, int y, int w, const char *s, uint32_t argb)
{
    char buf[200];
    od_ui_font_text(ui->canvas, OD_UI_SCREEN_W, OD_UI_SCREEN_H, x, y,
                     od_ui_font_fit(s, w, 1, buf, sizeof buf), argb, 1);
}

/* Text centred on `cx`, cut to `w` pixels. */
static void text_centered(od_ui *ui, int cx, int y, int w, const char *s,
                          uint32_t argb)
{
    char buf[200];
    const char *t = od_ui_font_fit(s, w, 1, buf, sizeof buf);
    int tw = od_ui_font_width(t, 1);
    od_ui_font_text(ui->canvas, OD_UI_SCREEN_W, OD_UI_SCREEN_H, cx - tw / 2,
                     y, t, argb, 1);
}

static void draw_button(od_ui *ui, int x, int y, int w, int h, const char *label,
                        int focused)
{
    fill_rect(ui, x, y, w, h, focused ? C_SEL_BG : C_BTN);
    draw_border(ui, x, y, w, h, focused ? C_SEL_TX : C_BORDER);
    text_centered(ui, x + w / 2, y + (h - OD_UI_FONT_H) / 2, w - 4, label,
                  C_SEL_TX);
}

/* ---- shared geometry (draw and pointer must agree) ---------------------- */

#define MENU_PANEL_X 44
#define MENU_PANEL_Y 24
#define MENU_PANEL_W 168
#define MENU_PANEL_H 188
#define MENU_ROW_Y   (MENU_PANEL_Y + 34)
#define MENU_ROW_H   18

#define SETTINGS_PANEL_X 20
#define SETTINGS_PANEL_Y 18
#define SETTINGS_PANEL_W 216
#define SETTINGS_PANEL_H 186
#define SETTINGS_ROW_Y   (SETTINGS_PANEL_Y + 28)
#define SETTINGS_ROW_H   20

#define STATES_PANEL_X 12
#define STATES_PANEL_Y 14
#define STATES_PANEL_W 232
#define STATES_PANEL_H 200
#define STATES_ROW_Y   (STATES_PANEL_Y + 30)
#define STATES_ROW_H   18
#define STATES_STATUS_Y (STATES_ROW_Y + OD_UI_STATES_VISIBLE * STATES_ROW_H + 3)
#define STATES_BUTTON_Y (STATES_PANEL_Y + STATES_PANEL_H - 16 - 8)

#define CONFIRM_X 28
#define CONFIRM_Y 64
#define CONFIRM_W 200
#define CONFIRM_H 92

enum { STATES_SAVE = 0, STATES_DELETE, STATES_BACK, STATES_BUTTONS };

static void menu_row_rect(int i, int *x, int *y, int *w, int *h)
{
    *x = MENU_PANEL_X + 8;
    *y = MENU_ROW_Y + i * MENU_ROW_H - 3;
    *w = MENU_PANEL_W - 16;
    *h = 16;
}

static void settings_row_rect(int i, int *x, int *y, int *w, int *h)
{
    *x = SETTINGS_PANEL_X + 6;
    *y = SETTINGS_ROW_Y + i * SETTINGS_ROW_H - 3;
    *w = SETTINGS_PANEL_W - 12;
    *h = 16;
}

/* Visible row `i` of the states list (0 .. OD_UI_STATES_VISIBLE-1). */
static void states_row_rect(int i, int *x, int *y, int *w, int *h)
{
    *x = STATES_PANEL_X + 8;
    *y = STATES_ROW_Y + i * STATES_ROW_H;
    *w = STATES_PANEL_W - 22;            /* room for the scroll bar */
    *h = 16;
}

/* Each button is as wide as its label needs (8 px a character plus 4 on
 * each side, at least 64): "Save here" was cut to "Save ..." (C6.32). */
static void states_button_rect(int b, int *x, int *y, int *w, int *h)
{
    static const od_string labels[] = { OD_STR_SAVE_HERE, OD_STR_DELETE,
                                         OD_STR_BACK };
    const int gap = 6;
    int bx = STATES_PANEL_X + 8, bw = 0;
    for (int i = 0; i <= b && i < 3; i++) {
        if (i > 0)
            bx += bw + gap;
        bw = od_ui_font_width(od_str(labels[i]), 1) + 8;
        if (bw < 64)
            bw = 64;
    }
    *w = bw;
    *h = 16;
    *y = STATES_BUTTON_Y;
    *x = bx;
}

static void confirm_button_rect(int yes, int *x, int *y, int *w, int *h)
{
    *w = 56;
    *h = 16;
    *y = CONFIRM_Y + CONFIRM_H - 16 - 8;
    *x = yes ? CONFIRM_X + 16 : CONFIRM_X + CONFIRM_W - 16 - 56;
}

static int inside(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* ---- menu model --------------------------------------------------------- */

static const od_string k_menu_items[] = {
    OD_STR_CONTINUE, OD_STR_STATES, OD_STR_EDITOR,
    OD_STR_LEVELS, OD_STR_SETTINGS, OD_STR_CONTROLS, OD_STR_NEW_GAME,
    OD_STR_QUIT,
};
#define MENU_COUNT ((int)(sizeof k_menu_items / sizeof k_menu_items[0]))

static const od_string k_settings_labels[] = {
    OD_STR_MUTE, OD_STR_VOLUME, OD_STR_INTEGER_SCALE,
    OD_STR_ASPECT_87, OD_STR_PAUSE_FOCUS, OD_STR_FULLSCREEN,
#ifdef __linux__
    OD_STR_DESKTOP_MENU,      /* C6.46: only where a launcher makes sense */
#endif
    OD_STR_BACK,
};
#define SETTINGS_COUNT ((int)(sizeof k_settings_labels / sizeof k_settings_labels[0]))

enum {
    S_MUTE = 0, S_VOLUME, S_INTSCALE, S_ASPECT, S_PAUSE, S_FULLSCREEN,
#ifdef __linux__
    S_DESKTOP,
#endif
    S_BACK
};
enum {
    M_CONTINUE = 0, M_STATES, M_EDITOR, M_LEVELS, M_SETTINGS, M_CONTROLS, M_NEW_GAME, M_QUIT
};

static void settings_value(const od_ui *ui, int row, char *out, size_t cap)
{
    const od_settings *s = ui->settings;
    switch (row) {
    case S_MUTE:       snprintf(out, cap, "%s", od_str_bool(s->muted)); break;
    case S_VOLUME:     snprintf(out, cap, "%d%%", s->volume); break;
    case S_INTSCALE:   snprintf(out, cap, "%s", od_str_bool(s->integer_scale)); break;
    case S_ASPECT:     snprintf(out, cap, "%s", od_str_bool(s->aspect_87)); break;
    case S_PAUSE:      snprintf(out, cap, "%s", od_str_bool(s->pause_on_focus_loss)); break;
    case S_FULLSCREEN: snprintf(out, cap, "%s", od_str_bool(s->fullscreen)); break;
#ifdef __linux__
    case S_DESKTOP:    snprintf(out, cap, "%s", od_str(ui->desktop_installed
                           ? OD_STR_DESKTOP_ADDED : OD_STR_DESKTOP_NOT_ADDED)); break;
#endif
    default:           out[0] = '\0'; break;
    }
}

/* ---- states model ------------------------------------------------------- */

static int states_slots(const od_ui *ui)
{
    int slots = ui->settings->states_slots;
    if (slots < 1) slots = 1;
    if (slots > OD_SETTINGS_STATES_MAX) slots = OD_SETTINGS_STATES_MAX;
    return slots;
}

/* Keep the focused slot inside the visible window of the list. */
static void states_scroll_to_focus(od_ui *ui)
{
    int slots = states_slots(ui);
    int max = slots - OD_UI_STATES_VISIBLE;
    if (max < 0) max = 0;
    if (ui->slot_focus < ui->slot_scroll)
        ui->slot_scroll = ui->slot_focus;
    if (ui->slot_focus >= ui->slot_scroll + OD_UI_STATES_VISIBLE)
        ui->slot_scroll = ui->slot_focus - OD_UI_STATES_VISIBLE + 1;
    if (ui->slot_scroll > max) ui->slot_scroll = max;
    if (ui->slot_scroll < 0) ui->slot_scroll = 0;
}

/* Ask before deleting or overwriting a slot (C6.32 UI-1/UI-17). */
static void states_request(od_ui *ui, int action, int slot)
{
    if (action == OD_UI_STATE_DELETE) {
        if (!ui->slot_occupied[slot])
            return;                         /* nothing to delete */
        ui->confirm = OD_UI_CONFIRM_DELETE;
        ui->confirm_slot = slot;
        ui->confirm_yes = 0;
        return;
    }
    if (action == OD_UI_STATE_SAVE && ui->slot_occupied[slot]) {
        ui->confirm = OD_UI_CONFIRM_OVERWRITE;
        ui->confirm_slot = slot;
        ui->confirm_yes = 0;
        return;
    }
    ui->state_slot = slot;
    ui->state_action = action;
}

static void states_answer(od_ui *ui, int yes)
{
    int what = ui->confirm;
    ui->confirm = OD_UI_CONFIRM_NONE;
    if (what == OD_UI_CONFIRM_DESKTOP) {
        /* Asked once, whatever the answer; the host installs on Yes. */
        ui->settings->desktop_prompted = 1;
        ui->save_settings = 1;
        ui->screen = OD_UI_HIDDEN;
        if (yes)
            ui->desktop_action = OD_UI_DESKTOP_INSTALL;
        return;
    }
    if (!yes)
        return;
    if (what == OD_UI_CONFIRM_NEW_GAME) {
        ui->new_game = 1;            /* the host starts it (C6.31) */
        return;
    }
    ui->state_slot = ui->confirm_slot;
    ui->state_action = what == OD_UI_CONFIRM_DELETE ? OD_UI_STATE_DELETE
                                                     : OD_UI_STATE_SAVE;
}

static void states_button(od_ui *ui, int b)
{
    if (b == STATES_BACK) {
        ui->screen = OD_UI_MENU;
        ui->selected = M_STATES;
        ui->states_button = -1;
    } else {
        states_request(ui, b == STATES_SAVE ? OD_UI_STATE_SAVE
                                            : OD_UI_STATE_DELETE,
                       ui->slot_focus);
    }
}

/* ---- input -------------------------------------------------------------- */

void od_ui_init(od_ui *ui, od_settings *settings)
{
    if (ui == NULL)
        return;
    memset(ui, 0, sizeof *ui);
    ui->settings = settings;
    ui->screen = OD_UI_HIDDEN;
    ui->selected = 0;
    ui->states_button = -1;
}

void od_ui_shutdown(od_ui *ui)
{
    if (ui == NULL)
        return;
    if (ui->overlay != NULL)
        SDL_DestroyTexture(ui->overlay);
    ui->overlay = NULL;
    memset(ui->slot_thumbnails,0,sizeof ui->slot_thumbnails);
}

void od_ui_toast(od_ui *ui, const char *text, unsigned ms)
{
    if (ui == NULL || text == NULL)
        return;
    snprintf(ui->toast, sizeof ui->toast, "%s", text);
    ui->toast_ms = ms;
}

void od_ui_tick(od_ui *ui, unsigned elapsed_ms)
{
    if (ui == NULL || ui->toast_ms == 0)
        return;
    ui->toast_ms = elapsed_ms >= ui->toast_ms ? 0 : ui->toast_ms - elapsed_ms;
}

int od_ui_modal(const od_ui *ui)
{
    return ui != NULL && ui->screen != OD_UI_HIDDEN;
}

int od_ui_visible(const od_ui *ui)
{
    return ui != NULL && (ui->screen != OD_UI_HIDDEN || ui->toast_ms > 0);
}

static void notify_settings_changed(od_ui *ui)
{
    ui->save_settings = 1;
}

int od_ui_handle_key(od_ui *ui, od_ui_key key)
{
    if (ui == NULL || key == OD_UI_KEY_NONE)
        return 0;

    /* Mute is global: it works with the menu open or closed. */
    if (key == OD_UI_KEY_MUTE) {
        ui->settings->muted = !ui->settings->muted;
        notify_settings_changed(ui);
        od_ui_toast(ui, ui->settings->muted ? od_str(OD_STR_TOAST_MUTED)
                                             : od_str(OD_STR_TOAST_SOUND),
                     1600);
        return 1;
    }

    if (ui->screen == OD_UI_EDITOR && ui->editor != NULL) {
        od_ui_editor *e = ui->editor;
        switch (key) {
        case OD_UI_KEY_UNDO:  od_ued_key(e, OD_UED_KEY_UNDO); return 1;
        case OD_UI_KEY_REDO:  od_ued_key(e, OD_UED_KEY_REDO); return 1;
        case OD_UI_KEY_EDITOR_SAVE: od_ued_key(e, OD_UED_KEY_SAVE); return 1;
        case OD_UI_KEY_EDITOR_TEST: od_ued_key(e, OD_UED_KEY_TEST); return 1;
        case OD_UI_KEY_PALETTE: od_ued_key(e, OD_UED_KEY_TOGGLE_PALETTE); return 1;
        case OD_UI_KEY_ACCEPT: od_ued_key(e, OD_UED_KEY_ACCEPT); return 1;
        case OD_UI_KEY_BACK:
            /* Asks first when there are unsaved changes (C6.32); the host
             * closes and unfreezes once the editor requests it. */
            od_ued_key(e, OD_UED_KEY_BACK);
            return 1;
        default:
            return 1;
        }
    }
    if (ui->screen == OD_UI_CATALOG && ui->catalog != NULL) {
        od_ui_catalog *c = ui->catalog;
        switch (key) {
        case OD_UI_KEY_UP:     return od_ucat_key(c, OD_UCAT_KEY_UP, 0);
        case OD_UI_KEY_DOWN:   return od_ucat_key(c, OD_UCAT_KEY_DOWN, 0);
        case OD_UI_KEY_ACCEPT: return od_ucat_key(c, OD_UCAT_KEY_PLAY, 0);
        case OD_UI_KEY_DELETE: return od_ucat_key(c, OD_UCAT_KEY_DELETE, 0);
        case OD_UI_KEY_BACK:   return od_ucat_key(c, OD_UCAT_KEY_BACK, 0);
        default:
            return 1;
        }
    }

    /* C6.40: F1 opens the controls from the game or any menu screen and
     * closes them again; Esc or B (back) close them too. A pending question
     * is answered first. */
    if (key == OD_UI_KEY_CONTROLS) {
        if (ui->screen == OD_UI_CONTROLS) {
            ui->screen = ui->controls_return;
        } else if (ui->confirm == OD_UI_CONFIRM_NONE) {
            ui->controls_return = ui->screen;
            ui->screen = OD_UI_CONTROLS;
            ui->toast_ms = 0;           /* the one-time hint is now answered */
        }
        return 1;
    }
    if (ui->screen == OD_UI_CONTROLS) {
        if (key == OD_UI_KEY_BACK || key == OD_UI_KEY_ACCEPT)
            ui->screen = ui->controls_return;
        return 1;                         /* modal: swallow the rest */
    }

    if (ui->screen == OD_UI_HIDDEN) {
        /* ESC opens the modern menu; everything else reaches the game. */
        if (key == OD_UI_KEY_BACK) {
            ui->screen = OD_UI_MENU;
            ui->selected = 0;
            return 1;
        }
        return 0;
    }

    /* F5/F9 save/load the current slot even with a screen open. Delete is no hotkey: it only acts in the states screen, on the
     * focused slot, after a confirmation (C6.32, UI-1). */
    if (key == OD_UI_KEY_SAVE || key == OD_UI_KEY_LOAD) {
        ui->state_slot = ui->settings->states_current;
        ui->state_action = key == OD_UI_KEY_SAVE ? OD_UI_STATE_SAVE
                                                  : OD_UI_STATE_LOAD;
        return 1;
    }

    /* A question (menu or states): Left/Right move between Yes and No, Enter
     * answers the focused one (No unless moved), Esc answers No. */
    if ((ui->screen == OD_UI_MENU || ui->screen == OD_UI_STATES) &&
        ui->confirm != OD_UI_CONFIRM_NONE) {
        if (key == OD_UI_KEY_LEFT || key == OD_UI_KEY_RIGHT)
            ui->confirm_yes = !ui->confirm_yes;
        else if (key == OD_UI_KEY_ACCEPT)
            states_answer(ui, ui->confirm_yes);
        else if (key == OD_UI_KEY_BACK)
            states_answer(ui, 0);
        return 1;
    }

    if (ui->screen == OD_UI_MENU) {
        switch (key) {
        case OD_UI_KEY_UP:
            ui->selected = (ui->selected + MENU_COUNT - 1) % MENU_COUNT;
            return 1;
        case OD_UI_KEY_DOWN:
            ui->selected = (ui->selected + 1) % MENU_COUNT;
            return 1;
        case OD_UI_KEY_BACK:
            ui->screen = OD_UI_HIDDEN;
            return 1;
        case OD_UI_KEY_ACCEPT:
            switch (ui->selected) {
            case M_CONTINUE:
                ui->screen = OD_UI_HIDDEN;
                break;
            case M_STATES:
                ui->screen = OD_UI_STATES;
                ui->slot_scroll = 0;
                ui->states_button = -1;
                ui->confirm = OD_UI_CONFIRM_NONE;
                od_ui_states_refresh(ui, ui->settings->states_current);
                ui->state_action = OD_UI_STATE_NONE;
                ui->state_status[0] = '\0';
                break;
            case M_SETTINGS:
                ui->screen = OD_UI_SETTINGS;
                ui->selected = 0;
                break;
            case M_CONTROLS:
                ui->controls_return = OD_UI_MENU;
                ui->screen = OD_UI_CONTROLS;
                ui->toast_ms = 0;
                break;
            case M_NEW_GAME:
                ui->confirm = OD_UI_CONFIRM_NEW_GAME;
                ui->confirm_yes = 0;
                break;
            case M_QUIT:
                ui->quit = 1;
                break;
            case M_EDITOR:   /* C5.5: the mouse level editor            */
                if (ui->editor != NULL) {
                    ui->screen = OD_UI_EDITOR;
                    ui->editor->open = 1;
                } else {
                    od_ui_toast(ui, od_str(OD_STR_TOAST_SOON), 1400);
                }
                break;
            case M_LEVELS:   /* C5.5: the custom-level catalogue        */
                if (ui->catalog != NULL) {
                    od_ucat_open(ui->catalog);
                    ui->catalog->return_to_editor = 0;   /* Back = menu */
                    ui->screen = OD_UI_CATALOG;
                } else {
                    od_ui_toast(ui, od_str(OD_STR_TOAST_SOON), 1400);
                }
                break;
            default:
                od_ui_toast(ui, od_str(OD_STR_TOAST_SOON), 1400);
                break;
            }
            return 1;
        default:
            return 1; /* modal: swallow game keys */
        }
    }

    if (ui->screen == OD_UI_SETTINGS) {
        od_settings *s = ui->settings;
        int changed = 0;
        switch (key) {
        case OD_UI_KEY_UP:
            ui->selected = (ui->selected + SETTINGS_COUNT - 1) % SETTINGS_COUNT;
            return 1;
        case OD_UI_KEY_DOWN:
            ui->selected = (ui->selected + 1) % SETTINGS_COUNT;
            return 1;
        case OD_UI_KEY_BACK:
            ui->screen = OD_UI_MENU;
            ui->selected = M_SETTINGS;
            return 1;
        case OD_UI_KEY_LEFT:
        case OD_UI_KEY_RIGHT: {
            int inc = key == OD_UI_KEY_RIGHT;
            switch (ui->selected) {
            case S_MUTE:       s->muted = !s->muted; changed = 1; break;
            case S_VOLUME: {
                int v = s->volume + (inc ? 10 : -10);
                if (v < 0) v = 0;
                if (v > OD_SETTINGS_VOLUME_MAX) v = OD_SETTINGS_VOLUME_MAX;
                if (v != s->volume) { s->volume = v; changed = 1; }
                break;
            }
            case S_INTSCALE:   s->integer_scale = !s->integer_scale; changed = 1; break;
            case S_ASPECT:     s->aspect_87 = !s->aspect_87; changed = 1; break;
            case S_PAUSE:      s->pause_on_focus_loss = !s->pause_on_focus_loss; changed = 1; break;
            case S_FULLSCREEN:
                s->fullscreen = !s->fullscreen;
                ui->apply_video = 1;    /* the host sets the window to it */
                changed = 1;
                break;
#ifdef __linux__
            case S_DESKTOP:             /* C6.46: the host adds or removes it */
                ui->desktop_action = ui->desktop_installed ? OD_UI_DESKTOP_UNINSTALL
                                                           : OD_UI_DESKTOP_INSTALL;
                break;
#endif
            default: break;
            }
            if (changed) {
                notify_settings_changed(ui);
                if (ui->selected == S_MUTE)
                    od_ui_toast(ui, s->muted ? od_str(OD_STR_TOAST_MUTED)
                                              : od_str(OD_STR_TOAST_SOUND), 1400);
            }
            return 1;
        }
        case OD_UI_KEY_ACCEPT:
            if (ui->selected == S_BACK) {
                ui->screen = OD_UI_MENU;
                ui->selected = M_SETTINGS;
            } else {
                /* Accept acts like "right" on value rows. */
                return od_ui_handle_key(ui, OD_UI_KEY_RIGHT);
            }
            return 1;
        default:
            return 1;
        }
    }

    if (ui->screen == OD_UI_STATES) {
        int slots = states_slots(ui);
        switch (key) {
        case OD_UI_KEY_UP:
            ui->states_button = -1;
            ui->slot_focus = (ui->slot_focus + slots - 1) % slots;
            states_scroll_to_focus(ui);
            return 1;
        case OD_UI_KEY_DOWN:
            ui->states_button = -1;
            ui->slot_focus = (ui->slot_focus + 1) % slots;
            states_scroll_to_focus(ui);
            return 1;
        case OD_UI_KEY_LEFT:
        case OD_UI_KEY_RIGHT: {
            /* The buttons are reachable without a mouse (C6.32, UI-24). */
            int b = ui->states_button + (key == OD_UI_KEY_RIGHT ? 1 : -1);
            if (b < -1) b = STATES_BUTTONS - 1;
            if (b >= STATES_BUTTONS) b = -1;
            ui->states_button = b;
            return 1;
        }
        case OD_UI_KEY_DELETE:
            states_request(ui, OD_UI_STATE_DELETE, ui->slot_focus);
            return 1;
        case OD_UI_KEY_BACK:
            ui->screen = OD_UI_MENU;
            ui->selected = M_STATES;
            ui->states_button = -1;
            return 1;
        case OD_UI_KEY_ACCEPT:
            if (ui->states_button >= 0) {
                states_button(ui, ui->states_button);
                return 1;
            }
            /* Enter on an occupied slot loads; on an empty one, saves. */
            states_request(ui, ui->slot_occupied[ui->slot_focus]
                               ? OD_UI_STATE_LOAD : OD_UI_STATE_SAVE,
                           ui->slot_focus);
            return 1;
        default:
            return 1;
        }
    }

    return 1;
}

/* C5.3: the mouse. The canvas is 256x240; the host maps the window pointer
 * onto it. A click selects and activates the item under the cursor. */
int od_ui_pointer(od_ui *ui, int x, int y, int click)
{
    if (ui == NULL)
        return 0;
    ui->ptr_x = x;
    ui->ptr_y = y;
    if (ui->screen == OD_UI_EDITOR && ui->editor != NULL)
        return od_ued_pointer(ui->editor, x, y, click ? OD_UED_BTN_LEFT : 0,
                               click, 0, 0, 0);
    if (ui->screen == OD_UI_CATALOG && ui->catalog != NULL)
        return od_ucat_pointer(ui->catalog, x, y, click ? 1 : 0, click, 0);
    if (ui->screen == OD_UI_HIDDEN)
        return 0;
    if (ui->screen == OD_UI_CONTROLS) {       /* C6.40: a click closes it */
        if (click)
            ui->screen = ui->controls_return;
        return 1;
    }

    if (ui->screen == OD_UI_MENU && ui->confirm != OD_UI_CONFIRM_NONE) {
        for (int yes = 0; yes <= 1; yes++) {
            int bx, by, bw, bh;
            confirm_button_rect(yes, &bx, &by, &bw, &bh);
            if (!inside(x, y, bx, by, bw, bh))
                continue;
            ui->confirm_yes = yes;
            if (click)
                states_answer(ui, yes);
            return 1;
        }
        return 1;
    }

    if (ui->screen == OD_UI_MENU) {
        for (int i = 0; i < MENU_COUNT; i++) {
            int rx, ry, rw, rh;
            menu_row_rect(i, &rx, &ry, &rw, &rh);
            if (!inside(x, y, rx, ry, rw, rh))
                continue;
            ui->selected = i;
            if (click)
                return od_ui_handle_key(ui, OD_UI_KEY_ACCEPT);
            return 1;
        }
        return 0;
    }

    if (ui->screen == OD_UI_STATES) {
        if (ui->confirm != OD_UI_CONFIRM_NONE) {
            for (int yes = 0; yes <= 1; yes++) {
                int bx, by, bw, bh;
                confirm_button_rect(yes, &bx, &by, &bw, &bh);
                if (!inside(x, y, bx, by, bw, bh))
                    continue;
                ui->confirm_yes = yes;
                if (click)
                    states_answer(ui, yes);
                return 1;
            }
            return 1;
        }
        int slots = states_slots(ui);
        for (int i = 0; i < OD_UI_STATES_VISIBLE; i++) {
            int slot = ui->slot_scroll + i;
            if (slot >= slots)
                break;
            int rx, ry, rw, rh;
            states_row_rect(i, &rx, &ry, &rw, &rh);
            if (!inside(x, y, rx, ry, rw, rh))
                continue;
            ui->slot_focus = slot;
            ui->states_button = -1;
            if (click)
                states_request(ui, ui->slot_occupied[slot] ? OD_UI_STATE_LOAD
                                                           : OD_UI_STATE_SAVE,
                               slot);
            return 1;
        }
        for (int b = 0; b < STATES_BUTTONS; b++) {
            int rx, ry, rw, rh;
            states_button_rect(b, &rx, &ry, &rw, &rh);
            if (!inside(x, y, rx, ry, rw, rh))
                continue;
            if (click)
                states_button(ui, b);
            return 1;
        }
        return 0;
    }

    if (ui->screen == OD_UI_SETTINGS) {
        for (int i = 0; i < SETTINGS_COUNT; i++) {
            int rx, ry, rw, rh;
            settings_row_rect(i, &rx, &ry, &rw, &rh);
            if (!inside(x, y, rx, ry, rw, rh))
                continue;
            if (ui->selected != i) {
                ui->selected = i;
                return 1;
            }
            if (click)
                return od_ui_handle_key(ui, i == S_BACK ? OD_UI_KEY_ACCEPT
                                                         : OD_UI_KEY_RIGHT);
            return 1;
        }
        return 0;
    }

    return 0;
}

int od_ui_pointer2(od_ui *ui, int x, int y, int buttons, int click,
                    int wheel, int shift, int ctrl)
{
    if (ui == NULL)
        return 0;
    ui->ptr_x = x;
    ui->ptr_y = y;
    ui->ptr_buttons = buttons;
    ui->ptr_wheel = wheel;
    ui->ptr_shift = shift;
    ui->ptr_ctrl = ctrl;
    /* The editor and the catalogue take points of their own 384x360 canvas
     * (C6.24); the host maps the window through that canvas's viewport. */
    if (ui->screen == OD_UI_EDITOR && ui->editor != NULL)
        return od_ued_pointer(ui->editor, x, y, buttons, click, wheel,
                               shift, ctrl);
    if (ui->screen == OD_UI_CATALOG && ui->catalog != NULL)
        return od_ucat_pointer(ui->catalog, x, y, buttons, click, wheel);
    if (ui->screen == OD_UI_STATES && wheel != 0 &&
        ui->confirm == OD_UI_CONFIRM_NONE) {
        int max = states_slots(ui) - OD_UI_STATES_VISIBLE;
        ui->slot_scroll += wheel > 0 ? -1 : 1;
        if (ui->slot_scroll > max) ui->slot_scroll = max;
        if (ui->slot_scroll < 0) ui->slot_scroll = 0;
        return 1;
    }
    return od_ui_pointer(ui, x, y, click);
}

/* ---- drawing ------------------------------------------------------------ */

/* A toast takes the place of the bottom hint line while it shows. */
static void draw_confirm(od_ui *ui, const char *question, const char *detail);

static int toast_up(const od_ui *ui)
{
    return ui->toast_ms > 0 && ui->toast[0] != '\0';
}

static void draw_menu(od_ui *ui)
{
    fill_rect(ui, MENU_PANEL_X, MENU_PANEL_Y, MENU_PANEL_W, MENU_PANEL_H, C_PANEL);
    draw_border(ui, MENU_PANEL_X, MENU_PANEL_Y, MENU_PANEL_W, MENU_PANEL_H, C_BORDER);
    text_centered(ui, OD_UI_SCREEN_W / 2, MENU_PANEL_Y + 10, MENU_PANEL_W - 16,
                  od_str(OD_STR_MENU_TITLE), C_TITLE);

    for (int i = 0; i < MENU_COUNT; i++) {
        int rx, ry, rw, rh;
        menu_row_rect(i, &rx, &ry, &rw, &rh);
        if (i == ui->selected)
            fill_rect(ui, rx, ry, rw, rh, C_SEL_BG);
        uint32_t col = (i == ui->selected) ? C_SEL_TX : C_TEXT;
        if (i == ui->selected)
            text_in(ui, rx + 6, ry + 3, 8, ">", col);
        text_in(ui, rx + 20, ry + 3, rw - 24, od_str(k_menu_items[i]), col);
    }

    if (!toast_up(ui))
        text_centered(ui, OD_UI_SCREEN_W / 2, MENU_PANEL_Y + MENU_PANEL_H + 6,
                      OD_UI_SCREEN_W - 16, od_str(OD_STR_HINT_MENU), C_MUTED);
    if (ui->confirm == OD_UI_CONFIRM_NEW_GAME)
        draw_confirm(ui, od_str(OD_STR_NEW_GAME_QUESTION),
                     od_str(OD_STR_NEW_GAME_DETAIL));
    else if (ui->confirm == OD_UI_CONFIRM_DESKTOP)
        draw_confirm(ui, od_str(OD_STR_DESKTOP_QUESTION),
                     od_str(OD_STR_DESKTOP_DETAIL));
}

static void draw_settings(od_ui *ui)
{
    fill_rect(ui, SETTINGS_PANEL_X, SETTINGS_PANEL_Y, SETTINGS_PANEL_W,
              SETTINGS_PANEL_H, C_PANEL);
    draw_border(ui, SETTINGS_PANEL_X, SETTINGS_PANEL_Y, SETTINGS_PANEL_W,
                SETTINGS_PANEL_H, C_BORDER);
    text_centered(ui, OD_UI_SCREEN_W / 2, SETTINGS_PANEL_Y + 8,
                  SETTINGS_PANEL_W - 16, od_str(OD_STR_SETTINGS_TITLE), C_TITLE);

    for (int i = 0; i < SETTINGS_COUNT; i++) {
        int rx, ry, rw, rh;
        settings_row_rect(i, &rx, &ry, &rw, &rh);
        if (i == ui->selected)
            fill_rect(ui, rx, ry, rw, rh, C_SEL_BG);
        uint32_t col = (i == ui->selected) ? C_SEL_TX : C_TEXT;
        char value[24];
        settings_value(ui, i, value, sizeof value);
        int vw = od_ui_font_width(value, 1);
        od_ui_font_text(ui->canvas, OD_UI_SCREEN_W, OD_UI_SCREEN_H,
                         rx + rw - 6 - vw, ry + 3, value, col, 1);
        /* The label takes what the value leaves, with a gap. */
        text_in(ui, rx + 6, ry + 3, rw - 12 - vw - (vw ? 8 : 0),
                od_str(k_settings_labels[i]), col);
    }

    if (!toast_up(ui))
        text_centered(ui, OD_UI_SCREEN_W / 2, SETTINGS_PANEL_Y + SETTINGS_PANEL_H + 6,
                      OD_UI_SCREEN_W - 16, od_str(OD_STR_HINT_SETTINGS), C_MUTED);
}

/* A question box: the question, up to three wrapped lines of detail and the
 * Yes/No buttons, the focused one highlighted. */
static void draw_confirm(od_ui *ui, const char *question, const char *detail)
{
    fill_rect(ui, CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, C_PANEL);
    draw_border(ui, CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, C_TITLE);
    /* C6.46: a long question wraps to two lines and the detail gives way. */
    char ql[2][64];
    int qn = od_ui_font_wrap(question, CONFIRM_W - 16, 1, &ql[0][0], sizeof ql[0], 2);
    if (qn < 1) {
        qn = 1;
        snprintf(ql[0], sizeof ql[0], "%s", question);
    }
    for (int i = 0; i < qn; i++)
        text_centered(ui, CONFIRM_X + CONFIRM_W / 2, CONFIRM_Y + 10 + 10 * i,
                      CONFIRM_W - 16, ql[i], C_TITLE);
    if (detail != NULL) {
        char lines[3][64];
        int n = od_ui_font_wrap(detail, CONFIRM_W - 16, 1, &lines[0][0],
                                 sizeof lines[0], qn > 1 ? 2 : 3);
        for (int i = 0; i < n; i++)
            text_centered(ui, CONFIRM_X + CONFIRM_W / 2,
                          CONFIRM_Y + 26 + 10 * (qn - 1) + 10 * i,
                          CONFIRM_W - 16, lines[i], C_TEXT);
    }
    for (int yes = 0; yes <= 1; yes++) {
        int bx, by, bw, bh;
        confirm_button_rect(yes, &bx, &by, &bw, &bh);
        draw_button(ui, bx, by, bw, bh, od_str(yes ? OD_STR_YES : OD_STR_NO),
                    yes == ui->confirm_yes);
    }
}

static void draw_states(od_ui *ui)
{
    const int panel_x = STATES_PANEL_X, panel_y = STATES_PANEL_Y;
    const int panel_w = STATES_PANEL_W, panel_h = STATES_PANEL_H;
    fill_rect(ui, panel_x, panel_y, panel_w, panel_h, C_PANEL);
    draw_border(ui, panel_x, panel_y, panel_w, panel_h, C_BORDER);
    text_centered(ui, OD_UI_SCREEN_W / 2, panel_y + 8, panel_w - 16,
                  od_str(OD_STR_STATES_TITLE), C_TITLE);

    int slots = states_slots(ui);
    for (int i = 0; i < OD_UI_STATES_VISIBLE; i++) {
        int slot = ui->slot_scroll + i;
        if (slot >= slots)
            break;
        int rx, ry, rw, rh;
        states_row_rect(i, &rx, &ry, &rw, &rh);
        int selected = slot == ui->slot_focus;
        if (selected)
            fill_rect(ui, rx, ry, rw, rh, ui->states_button < 0 ? C_SEL_BG : C_BTN);
        uint32_t col = selected ? C_SEL_TX : C_TEXT;
        char line[96];
        if (ui->slot_occupied[slot]) {
            const od_state_info *in = &ui->slot_info[slot];
            char when[32] = "";
            time_t t = (time_t)in->date;
            struct tm *tm = localtime(&t);
            if (tm)
                strftime(when, sizeof when, "%y-%m-%d %H:%M", tm);
            snprintf(line, sizeof line, "%d W%d-%d %s", slot + 1,
                     in->world, in->round, when);
        } else {
            snprintf(line, sizeof line, "%d  %s", slot + 1,
                     od_str(OD_STR_SLOT_EMPTY));
        }
        text_in(ui, rx + 22, ry + 4, rw - 24, line, col);
        int tw=ui->slot_thumbnails[slot].w,th=ui->slot_thumbnails[slot].h;
        draw_border(ui,rx+2,ry,16,16,C_MUTED);
        for(int y=0;y<th;++y)for(int x=0;x<tw;++x)
            fill_rect(ui,rx+3+x,ry+1+y,1,1,ui->slot_thumbnails[slot].pixels[y*14+x]);
    }
    /* Scroll bar when there are more slots than rows (C6.32, UI-12). */
    if (slots > OD_UI_STATES_VISIBLE) {
        int bx = panel_x + panel_w - 12, by = STATES_ROW_Y;
        int bh = OD_UI_STATES_VISIBLE * STATES_ROW_H - 2;
        draw_border(ui, bx, by, 6, bh, C_MUTED);
        int th = bh * OD_UI_STATES_VISIBLE / slots;
        int ty = by + (bh - th) * ui->slot_scroll /
                      (slots - OD_UI_STATES_VISIBLE);
        fill_rect(ui, bx + 1, ty, 4, th, C_TEXT);
    }

    const char *labels[STATES_BUTTONS] = {
        od_str(OD_STR_SAVE_HERE), od_str(OD_STR_DELETE),
        od_str(OD_STR_BACK),
    };
    for (int b = 0; b < STATES_BUTTONS; b++) {
        int rx, ry, rw, rh;
        states_button_rect(b, &rx, &ry, &rw, &rh);
        draw_button(ui, rx, ry, rw, rh, labels[b], b == ui->states_button);
    }

    if (ui->state_status[0] != '\0')
        text_in(ui, panel_x + 8, STATES_STATUS_Y, panel_w - 16, ui->state_status,
                C_TITLE);
    if (!toast_up(ui))
        text_centered(ui, OD_UI_SCREEN_W / 2, panel_y + panel_h + 4,
                      OD_UI_SCREEN_W - 16, od_str(OD_STR_HINT_STATES), C_MUTED);

    if (ui->confirm != OD_UI_CONFIRM_NONE) {
        char question[64];
        snprintf(question, sizeof question,
                 od_str(ui->confirm == OD_UI_CONFIRM_DELETE
                             ? OD_STR_CONFIRM_DELETE_SLOT
                             : OD_STR_CONFIRM_OVERWRITE_SLOT),
                 ui->confirm_slot + 1);
        draw_confirm(ui, question, NULL);
    }
}

/* ---- controls screen (C6.40) -------------------------------------------- */

/* One row per binding. The keys are the real ones of src/app/main.c and
 * src/app/input_pad.c; the left column is the key, the right one its action. */
typedef struct { const char *key, *action; } controls_row;

static const controls_row k_controls_game[] = {
    { "Arrows",        "Move, jump" },
    { "Z, Space",      "A (wand)" },
    { "X",             "B (wand)" },
    { "Enter",         "START" },
    { "Right Shift",   "SELECT" },
    { "Esc / Guide", "Menu" },
    { "M",             "Mute" },
    { "F5 / F9",       "Save/load state" },
    { "F6 / F7",       "Prev/next slot" },
    { "F12",           "Screenshot" },
    { "F2",            "Console RESET" },
    { "F11",           "Fullscreen" },
    { "F1",            "This screen" },
};
static const controls_row k_controls_editor[] = {
    { "Ctrl+Z / Y", "Undo / redo" },
    { "Ctrl+S",        "Save level" },
    { "F5",            "Test level" },
    { "L/R click", "Paint / erase" },
    { "Mouse wheel",   "Change material" },
    { "Middle click",  "Pick material" },
};
#define CONTROLS_PITCH 9
#define CONTROLS_KEY_X 9
#define CONTROLS_KEY_W 112          /* 14 characters */
#define CONTROLS_ACT_X 124
#define CONTROLS_ACT_W 120          /* 15 characters */

static int draw_controls_rows(od_ui *ui, int y, const controls_row *rows, int n)
{
    for (int i = 0; i < n; i++, y += CONTROLS_PITCH) {
        text_in(ui, CONTROLS_KEY_X, y, CONTROLS_KEY_W, rows[i].key, C_SEL_TX);
        text_in(ui, CONTROLS_ACT_X, y, CONTROLS_ACT_W, rows[i].action, C_TEXT);
    }
    return y;
}

static void draw_controls(od_ui *ui)
{
    fill_rect(ui, 4, 4, OD_UI_SCREEN_W - 8, OD_UI_SCREEN_H - 8, C_PANEL);
    draw_border(ui, 4, 4, OD_UI_SCREEN_W - 8, OD_UI_SCREEN_H - 8, C_BORDER);
    text_centered(ui, OD_UI_SCREEN_W / 2, 8, OD_UI_SCREEN_W - 24,
                  od_str(OD_STR_CONTROLS_TITLE), C_TITLE);
    int y = 19;
    text_in(ui, CONTROLS_KEY_X, y, 100, "GAME", C_TITLE);
    y = draw_controls_rows(ui, y + 11, k_controls_game,
                           (int)(sizeof k_controls_game / sizeof k_controls_game[0]));
    y += 3;
    text_in(ui, CONTROLS_KEY_X, y, 100, "EDITOR", C_TITLE);
    draw_controls_rows(ui, y + 11, k_controls_editor,
                       (int)(sizeof k_controls_editor / sizeof k_controls_editor[0]));
    if (!toast_up(ui))
        text_centered(ui, OD_UI_SCREEN_W / 2, OD_UI_SCREEN_H - 16,
                      OD_UI_SCREEN_W - 24, od_str(OD_STR_HINT_CONTROLS), C_MUTED);
}

/* A toast wraps to two lines and never leaves the screen (C6.32, UI-11). */
static void draw_toast(od_ui *ui)
{
    char lines[2][64];
    int n = od_ui_font_wrap(ui->toast, OD_UI_SCREEN_W - 24, 1, &lines[0][0],
                             sizeof lines[0], 2);
    if (n < 1)
        return;
    int tw = 0;
    for (int i = 0; i < n; i++) {
        int w = od_ui_font_width(lines[i], 1);
        if (w > tw) tw = w;
    }
    int w = tw + 16, h = 8 + 10 * n;
    int x = (OD_UI_SCREEN_W - w) / 2;
    int y = OD_UI_SCREEN_H - 4 - h;
    fill_rect(ui, x, y, w, h, C_TOAST);
    draw_border(ui, x, y, w, h, C_BORDER);
    for (int i = 0; i < n; i++)
        text_centered(ui, OD_UI_SCREEN_W / 2, y + 5 + 10 * i, w - 8, lines[i],
                      C_TOASTTX);
}

void od_ui_draw(od_ui *ui)
{
    if (ui == NULL)
        return;
    canvas_clear(ui);
    if (ui->screen == OD_UI_EDITOR || ui->screen == OD_UI_CATALOG) {
        /* C6.24: these two screens have their own 384x360 canvas, drawn by
         * od_ued_draw/od_ucat_draw and presented by the host; drawing them
         * into this 256x240 game overlay is what used to cover the level. */
    } else if (ui->screen != OD_UI_HIDDEN) {
        fill_dim(ui);
        if (ui->screen == OD_UI_MENU)
            draw_menu(ui);
        else if (ui->screen == OD_UI_STATES)
            draw_states(ui);
        else if (ui->screen == OD_UI_CONTROLS)
            draw_controls(ui);
        else
            draw_settings(ui);
    }
    if (ui->toast_ms > 0 && ui->toast[0] != '\0')
        draw_toast(ui);
    ui->canvas_active = od_ui_visible(ui);
}

/* ---- presentation ------------------------------------------------------- */

od_status od_ui_present(od_ui *ui, od_renderer *r, const uint32_t *game_argb)
{
    if (r == NULL || r->renderer == NULL || r->texture == NULL || game_argb == NULL)
        return OD_ERR_RANGE;

    if (SDL_UpdateTexture(r->texture, NULL, game_argb,
                          OD_UI_SCREEN_W * (int)sizeof(uint32_t)) != 0)
        return OD_ERR_IO;
    SDL_SetRenderDrawColor(r->renderer, 0, 0, 0, 255);
    SDL_RenderClear(r->renderer);
    if (SDL_RenderCopy(r->renderer, r->texture, NULL, NULL) != 0)
        return OD_ERR_IO;

    if (ui != NULL && ui->canvas_active) {
        if (ui->overlay == NULL) {
            ui->overlay = SDL_CreateTexture(r->renderer, SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            OD_UI_SCREEN_W, OD_UI_SCREEN_H);
            if (ui->overlay == NULL)
                return OD_ERR_IO;
            SDL_SetTextureBlendMode(ui->overlay, SDL_BLENDMODE_BLEND);
        }
        if (SDL_UpdateTexture(ui->overlay, NULL, ui->canvas,
                              OD_UI_SCREEN_W * (int)sizeof(uint32_t)) != 0)
            return OD_ERR_IO;
        if (SDL_RenderCopy(r->renderer, ui->overlay, NULL, NULL) != 0)
            return OD_ERR_IO;
    }

    if (od_renderer_cursor_update)   /* absent in the headless layout tests */
        od_renderer_cursor_update(r);
    SDL_RenderPresent(r->renderer);
    return OD_OK;
}
