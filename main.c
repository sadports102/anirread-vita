/* ===== aniread - Step 1: library UI (built from aniwatch's UI code) ===== */
#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include "md.h"
#include "nv.h"
#define APP_VERSION "v0.1.0"
#define SCREEN_W 960
#define SCREEN_H 544
#define DATA_ROOT   "ux0:data/aniread"
#define MANGA_ROOT  DATA_ROOT "/manga"
#define NOVEL_ROOT  DATA_ROOT "/novels"
#define IMPORT_ROOT DATA_ROOT "/import"
#define FONT_REG  "app0:font.ttf"
#define FONT_BOLD "app0:font_bold.ttf"
#define MAX_TITLES 384
#define POSTER_W 200
#define POSTER_H 300
#define BANNER_W 480
#define BANNER_H 270
#define FEAT_TIME 10.0f     /* hero banner rotation (seconds) */

/* colours (dango style, same as aniwatch) */
#define C_BG      13, 13, 16
#define C_BAR     20, 21, 30
#define C_PANEL   28, 28, 36
#define C_PANEL2  42, 42, 54
#define C_LINE    52, 52, 66
#define C_TEXT    242, 242, 246
#define C_SOFT    200, 200, 212
#define C_DIM     140, 140, 156
#define C_ACCENT  108, 99, 255
#define C_ACCLT   152, 144, 255
#define C_TINT    34, 31, 74

/* layout (same as aniwatch) */
#define BAR_H     60
#define HERO_H    340
#define SEC_H     308
#define CARD_W    128
#define CARD_H    192
#define CARD_GAP  18
#define GCOLS     6
#define GCARD_W   132
#define GCARD_H   198
#define GGAP      22
#define GROW_H    256
#define GRID_Y    150
#define DRAWER_W  300
#define D_LIST_Y  452
#define D_ROW_EP  66
#define HINT_CY   (SCREEN_H - 18)

/* ===== Globals ===== */
static SDL_Window *win;
static SDL_Renderer *ren;

enum { F_SMALL, F_SMALLB, F_BODY, F_BODYB, F_SUB, F_LOGO, F_H2, F_HERO, F_COUNT };
static const int font_px[F_COUNT]   = { 15, 15, 18, 18, 22, 26, 30, 44 };
static const int font_bold[F_COUNT] = { 0, 1, 0, 1, 1, 1, 1, 1 };
static TTF_Font *fonts[F_COUNT];

enum { K_MANGA, K_NOVEL };

typedef struct {
    char path[512], file[256], label[128], sub[160];
    int number;
} Chapter;               /* manga: a chapter folder, novel: a volume file */

typedef struct {
    int kind;
    char name[128], card_name[128];
    char dir[512], cover_path[512], banner_path[512];
    char synopsis[1024], genres[160], year[16], rating[8], type[16], status[24];
    Chapter *chs; int nchs, cap;
    SDL_Texture *cover; int cover_state;        /* 0 not tried, 1 loaded, 2 none */
    unsigned pg_gen; int pg_done; float pg_f;   /* reading progress cache */
} Title;

static Title titles[MAX_TITLES];
static int ntitles;
static int manga_ix[MAX_TITLES], nmanga, novel_ix[MAX_TITLES], nnovel;

enum { PG_HOME, PG_MANGA, PG_NOVELS };
static int page = PG_HOME;
static float tab_x;

/* home */
static int   h_focus, h_btn, row_sel[3];   /* h_focus: 0 hero, else HR_ row + 1 */
static float row_scroll[3], row_target[3], h_scroll, h_target;
static int   feat; static float feat_t, hero_a = 1;
static char  hero_title[128], hero_meta[200], hero_genres[200];

/* grids */
static int   g_sel[2];
static float g_scroll[2], g_target[2];

/* drawer */
static int   drawer_open, drawer_sel;
static float drawer_t;

/* details */
static int    in_details, d_ti, d_focus, d_btn, d_sel;
static float  d_scroll, d_target;
static char   d_title[128], d_meta[200], d_genres[200];

typedef struct { SDL_Texture *t; int w, h; } WTex;
static WTex hero_syn, d_syn;

/* misc */
static char  toast_msg[160];
static float toast_t;
static unsigned frame_no = 1;
static int cover_budget;
static SDL_Texture *circ_tex, *dots_tex;
#define CIRC 128

/* input */
static unsigned btn, btn_old;
static int st_up, st_down, st_left, st_right;
static float rep_t[4]; static int rep_on[4];
static int   t_down, t_axis, tap_pending;
static float t_x0, t_y0, t_lx, t_ly, drag_dx, drag_dy, tap_x, tap_y;

/* tap targets (drawn last frame, tested this frame) */
enum { H_NONE, H_MENU, H_TAB, H_REFRESH, H_SETTINGS, H_HPLAY, H_HDETAILS, H_HPREV, H_HNEXT,
       H_ROWCARD, H_SEEALL, H_GRIDCARD, H_DPLAY, H_DBACK, H_CH, H_DRAWER_ITEM, H_DRAWER_OUT,
       H_ST_BACK, H_ST_ROW, H_SRC_BACK, H_SRC_TAB, H_SRC_SEARCH, H_SRC_CARD, H_SRC_PREV, H_SRC_NEXT,
       H_ON_ALL, H_ON_BACK, H_ON_CH };
typedef struct { int x, y, w, h, id, arg; } Hit;
#define MAX_HITS 200
static Hit hits[MAX_HITS], hits_prev[MAX_HITS];
static int nhits, nhits_prev;

/* ===== Small helpers ===== */
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float lerpk(float dt) { return clampf(dt * 14.0f, 0, 1); }
static int ends_with_ci(const char *s, const char *ext) {
    size_t a = strlen(s), b = strlen(ext);
    return a >= b && strcasecmp(s + a - b, ext) == 0;
}
static int file_exists(const char *p) { SceIoStat st; return sceIoGetstat(p, &st) >= 0; }

static void toast(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(toast_msg, sizeof toast_msg, fmt, ap);
    va_end(ap);
    toast_t = 2.4f;
}

static void hit(int x, int y, int w, int h, int id, int arg, int content) {
    if (content) {
        int y0 = y < BAR_H ? BAR_H : y, y1 = y + h > SCREEN_H ? SCREEN_H : y + h;
        if (y1 <= y0) return;
        y = y0; h = y1 - y0;
    }
    if (nhits < MAX_HITS) { Hit t = { x, y, w, h, id, arg }; hits[nhits++] = t; }
}
static Hit *tap_hit(void) {
    if (!tap_pending) return NULL;
    for (int i = nhits_prev - 1; i >= 0; i--) {
        Hit *h = &hits_prev[i];
        if (tap_x >= h->x && tap_x < h->x + h->w && tap_y >= h->y && tap_y < h->y + h->h) return h;
    }
    return NULL;
}

/* ===== Drawing primitives ===== */
static void fill(int x, int y, int w, int h, int r, int g, int b, int a) {
    if (w <= 0 || h <= 0) return;
    SDL_SetRenderDrawColor(ren, r, g, b, a);
    SDL_Rect rc = { x, y, w, h };
    SDL_RenderFillRect(ren, &rc);
}
static void frame_rect(int x, int y, int w, int h, int t, int r, int g, int b, int a) {
    fill(x, y, w, t, r, g, b, a); fill(x, y + h - t, w, t, r, g, b, a);
    fill(x, y + t, t, h - 2 * t, r, g, b, a); fill(x + w - t, y + t, t, h - 2 * t, r, g, b, a);
}
static void grad_v(int x, int y, int w, int h, int r, int g, int b, int a0, int a1) {
    const int steps = 16;
    for (int i = 0; i < steps; i++) {
        int y0 = y + h * i / steps, y1 = y + h * (i + 1) / steps;
        fill(x, y0, w, y1 - y0, r, g, b, a0 + (a1 - a0) * i / (steps - 1));
    }
}
static void grad_h(int x, int y, int w, int h, int r, int g, int b, int a0, int a1) {
    const int steps = 24;
    for (int i = 0; i < steps; i++) {
        int x0 = x + w * i / steps, x1 = x + w * (i + 1) / steps;
        fill(x0, y, x1 - x0, h, r, g, b, a0 + (a1 - a0) * i / (steps - 1));
    }
}
static void tex_col(SDL_Texture *t, int r, int g, int b, int a) {
    SDL_SetTextureColorMod(t, r, g, b);
    SDL_SetTextureAlphaMod(t, a);
}

/* anti-aliased disc/ring texture */
static SDL_Texture *make_ring_tex(int size, float r_out, float r_in) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return NULL;
    Uint32 *px = s->pixels; int p = s->pitch / 4; float c = size * 0.5f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float dx = x + 0.5f - c, dy = y + 0.5f - c, d = sqrtf(dx * dx + dy * dy);
            float a = clampf(r_out - d + 0.5f, 0, 1);
            if (r_in > 0) a *= clampf(d - r_in + 0.5f, 0, 1);
            px[y * p + x] = ((Uint32)(a * 255 + 0.5f) << 24) | 0xFFFFFF;
        }
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

typedef struct { int r, t; SDL_Texture *tex; } RingC;
static RingC rings[32]; static int nrings;
static SDL_Texture *ring_tex(int r, int t) {
    for (int i = 0; i < nrings; i++) if (rings[i].r == r && rings[i].t == t) return rings[i].tex;
    if (nrings >= 32) return NULL;
    SDL_Texture *tx = make_ring_tex(r * 2, (float)r, (float)(r - t));
    rings[nrings].r = r; rings[nrings].t = t; rings[nrings].tex = tx; nrings++;
    return tx;
}

static void rrect(int x, int y, int w, int h, int rad, int r, int g, int b, int a) {
    if (w <= 0 || h <= 0) return;
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    if (rad < 1) { fill(x, y, w, h, r, g, b, a); return; }
    tex_col(circ_tex, r, g, b, a);
    int q = CIRC / 2;
    SDL_Rect s[4] = { {0,0,q,q}, {q,0,q,q}, {0,q,q,q}, {q,q,q,q} };
    SDL_Rect d[4] = { {x,y,rad,rad}, {x+w-rad,y,rad,rad}, {x,y+h-rad,rad,rad}, {x+w-rad,y+h-rad,rad,rad} };
    for (int i = 0; i < 4; i++) SDL_RenderCopy(ren, circ_tex, &s[i], &d[i]);
    fill(x + rad, y, w - 2 * rad, h, r, g, b, a);
    fill(x, y + rad, rad, h - 2 * rad, r, g, b, a);
    fill(x + w - rad, y + rad, rad, h - 2 * rad, r, g, b, a);
}
static void rrect_line(int x, int y, int w, int h, int rad, int t, int r, int g, int b, int a) {
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    SDL_Texture *rt = rad > 0 ? ring_tex(rad, t) : NULL;
    if (!rt) { frame_rect(x, y, w, h, t, r, g, b, a); return; }
    tex_col(rt, r, g, b, a);
    SDL_Rect s[4] = { {0,0,rad,rad}, {rad,0,rad,rad}, {0,rad,rad,rad}, {rad,rad,rad,rad} };
    SDL_Rect d[4] = { {x,y,rad,rad}, {x+w-rad,y,rad,rad}, {x,y+h-rad,rad,rad}, {x+w-rad,y+h-rad,rad,rad} };
    for (int i = 0; i < 4; i++) SDL_RenderCopy(ren, rt, &s[i], &d[i]);
    fill(x + rad, y, w - 2 * rad, t, r, g, b, a);
    fill(x + rad, y + h - t, w - 2 * rad, t, r, g, b, a);
    fill(x, y + rad, t, h - 2 * rad, r, g, b, a);
    fill(x + w - t, y + rad, t, h - 2 * rad, r, g, b, a);
}
static void disc(float cx, float cy, float rad, int r, int g, int b, int a) {
    tex_col(circ_tex, r, g, b, a);
    SDL_Rect d = { (int)(cx - rad), (int)(cy - rad), (int)(rad * 2), (int)(rad * 2) };
    SDL_RenderCopy(ren, circ_tex, NULL, &d);
}

/* filled triangles via SDL_RenderGeometry */
static SDL_Color gcol;
static void gc(int r, int g, int b, int a) { gcol.r = r; gcol.g = g; gcol.b = b; gcol.a = a; }
static void tri(float x0, float y0, float x1, float y1, float x2, float y2) {
    SDL_Vertex v[3];
    memset(v, 0, sizeof v);
    v[0].position.x = x0; v[0].position.y = y0;
    v[1].position.x = x1; v[1].position.y = y1;
    v[2].position.x = x2; v[2].position.y = y2;
    v[0].color = v[1].color = v[2].color = gcol;
    SDL_RenderGeometry(ren, NULL, v, 3, NULL, 0);
}
static void line_w(float x0, float y0, float x1, float y1, float w) {
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l < 0.01f) return;
    float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    tri(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny);
    tri(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny);
}
static void fillg(float x, float y, float w, float h) { fill((int)x, (int)y, (int)(w + 0.5f), (int)(h + 0.5f), gcol.r, gcol.g, gcol.b, gcol.a); }
static void rrectg(float x, float y, float w, float h, int rad) { rrect((int)x, (int)y, (int)(w + 0.5f), (int)(h + 0.5f), rad, gcol.r, gcol.g, gcol.b, gcol.a); }
static void discg(float cx, float cy, float r) { disc(cx, cy, r, gcol.r, gcol.g, gcol.b, gcol.a); }

/* ===== Icons (fg colour = gcol, bg = colour for cut-outs) ===== */
enum { IC_MENU, IC_HOME, IC_BOOK, IC_NOVEL, IC_GEAR, IC_REFRESH, IC_PLAY, IC_INFO, IC_STAR, IC_LEFT, IC_RIGHT, IC_DOWN, IC_SEARCH };
static void icon(int ic, float cx, float cy, float s, int br, int bg, int bb) {
    SDL_Color fg = gcol;
    switch (ic) {
    case IC_MENU:
        for (int i = -1; i <= 1; i++) rrectg(cx - 10 * s, cy + i * 6 * s - 1.5f * s, 20 * s, 3 * s, 1);
        break;
    case IC_HOME:
        tri(cx - 11 * s, cy - 1 * s, cx, cy - 11 * s, cx + 11 * s, cy - 1 * s);
        rrectg(cx - 7.5f * s, cy - 2 * s, 15 * s, 11 * s, 1);
        gc(br, bg, bb, 255); fillg(cx - 2.5f * s, cy + 3 * s, 5 * s, 6 * s);
        break;
    case IC_BOOK:   /* open book: two pages with lines */
        rrectg(cx - 11 * s, cy - 8 * s, 10 * s, 17 * s, (int)(2 * s));
        rrectg(cx + 1 * s, cy - 8 * s, 10 * s, 17 * s, (int)(2 * s));
        gc(br, bg, bb, 255);
        for (int i = 0; i < 3; i++) {
            fillg(cx - 8.5f * s, cy - 4 * s + i * 4 * s, 5.5f * s, 1.6f * s);
            fillg(cx + 3 * s, cy - 4 * s + i * 4 * s, 5.5f * s, 1.6f * s);
        }
        break;
    case IC_NOVEL:  /* page of text */
        rrectg(cx - 8 * s, cy - 10 * s, 16 * s, 20 * s, (int)(2 * s));
        gc(br, bg, bb, 255);
        fillg(cx - 5 * s, cy - 6 * s, 10 * s, 1.8f * s);
        fillg(cx - 5 * s, cy - 1.5f * s, 10 * s, 1.8f * s);
        fillg(cx - 5 * s, cy + 3 * s, 6 * s, 1.8f * s);
        break;
    case IC_GEAR:
        for (int k = 0; k < 8; k++) {
            float a = k * 0.785398f, c = cosf(a), sn = sinf(a);
            line_w(cx + c * 5 * s, cy + sn * 5 * s, cx + c * 10.5f * s, cy + sn * 10.5f * s, 4.5f * s);
        }
        discg(cx, cy, 7.5f * s);
        gc(br, bg, bb, 255); discg(cx, cy, 3.2f * s);
        break;
    case IC_REFRESH: {
        float r = 8 * s, a0 = 0.7f, a1 = 5.5f, px = 0, py = 0;
        for (float a = a0; a <= a1 + 0.001f; a += 0.32f) {
            float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
            if (a > a0) line_w(px, py, x, y, 2.6f * s);
            px = x; py = y;
        }
        float ex = cx + cosf(a1) * r, ey = cy + sinf(a1) * r;
        float tx = -sinf(a1), ty = cosf(a1), nx = cosf(a1), ny = sinf(a1);
        tri(ex + tx * 5 * s, ey + ty * 5 * s, ex + nx * 4.5f * s, ey + ny * 4.5f * s, ex - nx * 4.5f * s, ey - ny * 4.5f * s);
        break; }
    case IC_PLAY:
        tri(cx - 6 * s, cy - 8 * s, cx + 9 * s, cy, cx - 6 * s, cy + 8 * s);
        break;
    case IC_INFO:
        discg(cx, cy, 9.5f * s);
        gc(br, bg, bb, 255);
        fillg(cx - 1.3f * s, cy - 1 * s, 2.6f * s, 6.5f * s);
        discg(cx, cy - 4.5f * s, 1.6f * s);
        break;
    case IC_STAR: {
        float px[11], py[11];
        for (int k = 0; k <= 10; k++) {
            float a = -1.570796f + k * 0.628318f, r = (k % 2) ? 4.2f * s : 9.5f * s;
            px[k] = cx + cosf(a) * r; py[k] = cy + sinf(a) * r;
        }
        for (int k = 0; k < 10; k++) tri(cx, cy, px[k], py[k], px[k + 1], py[k + 1]);
        break; }

    case IC_DOWN:   /* download arrow */
        line_w(cx, cy - 9 * s, cx, cy + 2 * s, 2.8f * s);
        tri(cx - 6 * s, cy - 1 * s, cx + 6 * s, cy - 1 * s, cx, cy + 6 * s);
        rrectg(cx - 9 * s, cy + 7.5f * s, 18 * s, 2.8f * s, 1);
        break;
    case IC_SEARCH: /* magnifier */
        discg(cx - 2 * s, cy - 2 * s, 7.5f * s);
        gc(br, bg, bb, 255); discg(cx - 2 * s, cy - 2 * s, 4.6f * s);
        gcol = fg;
        line_w(cx + 3 * s, cy + 3 * s, cx + 9 * s, cy + 9 * s, 3.2f * s);
        break;

    case IC_LEFT:
        line_w(cx + 3 * s, cy - 7 * s, cx - 4 * s, cy, 2.8f * s);
        line_w(cx - 4 * s, cy, cx + 3 * s, cy + 7 * s, 2.8f * s);
        discg(cx - 4 * s, cy, 1.4f * s);
        break;
    case IC_RIGHT:
        line_w(cx - 3 * s, cy - 7 * s, cx + 4 * s, cy, 2.8f * s);
        line_w(cx + 4 * s, cy, cx - 3 * s, cy + 7 * s, 2.8f * s);
        discg(cx + 4 * s, cy, 1.4f * s);
        break;
    }
    gcol = fg;
}

static void dango_icon(float cx, float cy, float s) {
    gc(160, 128, 96, 255);
    line_w(cx - 15 * s, cy + 11 * s, cx + 15 * s, cy - 11 * s, 2.4f * s);
    disc(cx - 8 * s, cy + 6 * s, 6.5f * s, 150, 140, 240, 255);
    disc(cx,         cy,         6.5f * s, 230, 150, 200, 255);
    disc(cx + 8 * s, cy - 6 * s, 6.5f * s, 236, 228, 200, 255);
}

/* ===== Text (cached white textures, tinted at draw time) ===== */
#define TC_MAX 448
typedef struct { char *s; unsigned hash; int font, w, h; unsigned used; SDL_Texture *t; } TCEntry;
static TCEntry tcache[TC_MAX];

static unsigned str_hash(const char *s) { unsigned h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h; }

static TCEntry *text_get(int f, const char *s) {
    if (!s || !s[0]) return NULL;
    unsigned h = str_hash(s) ^ ((unsigned)f * 2654435761u);
    int slot = -1;
    for (int i = 0; i < TC_MAX; i++) {
        TCEntry *e = &tcache[i];
        if (!e->s) { if (slot < 0 || tcache[slot].s) slot = i; continue; }
        if (e->hash == h && e->font == f && !strcmp(e->s, s)) { e->used = frame_no; return e; }
        if (slot < 0 || (tcache[slot].s && e->used < tcache[slot].used)) slot = i;
    }
    TCEntry *e = &tcache[slot];
    if (e->s) { free(e->s); SDL_DestroyTexture(e->t); e->s = NULL; e->t = NULL; }
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *sf = TTF_RenderUTF8_Blended(fonts[f], s, white);
    if (!sf) return NULL;
    e->t = SDL_CreateTextureFromSurface(ren, sf);
    e->w = sf->w; e->h = sf->h;
    SDL_FreeSurface(sf);
    if (!e->t) return NULL;
    SDL_SetTextureBlendMode(e->t, SDL_BLENDMODE_BLEND);
    e->s = strdup(s); e->hash = h; e->font = f; e->used = frame_no;
    return e;
}

enum { AL_L, AL_C, AL_R };
static int text(int f, int x, int y, int al, const char *s, int r, int g, int b, int a) {
    TCEntry *e = text_get(f, s);
    if (!e) return 0;
    if (al == AL_C) x -= e->w / 2; else if (al == AL_R) x -= e->w;
    tex_col(e->t, r, g, b, a);
    SDL_Rect d = { x, y, e->w, e->h };
    SDL_RenderCopy(ren, e->t, NULL, &d);
    return e->w;
}
static int textc(int f, int x, int cy, int al, const char *s, int r, int g, int b, int a) {
    TCEntry *e = text_get(f, s);
    if (!e) return 0;
    return text(f, x, cy - e->h / 2, al, s, r, g, b, a);
}
static int text_w(int f, const char *s) { TCEntry *e = text_get(f, s); return e ? e->w : 0; }

static void fit_text(int f, const char *src, int maxw, char *out, int outsz) {
    int w = 0;
    snprintf(out, outsz, "%s", src);
    if (TTF_SizeUTF8(fonts[f], out, &w, NULL) == 0 && w <= maxw) return;
    int len = (int)strlen(out);
    if (w > 0) {
        int est = len * maxw / w + 2;
        if (est < len) {
            while (est > 0 && ((unsigned char)out[est] & 0xC0) == 0x80) est--;
            out[est] = 0; len = est;
        }
    }
    char tmp[512] = "...";
    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80) len--;
        out[len] = 0;
        while (len > 0 && out[len - 1] == ' ') out[--len] = 0;
        snprintf(tmp, sizeof tmp, "%s...", out);
        if (TTF_SizeUTF8(fonts[f], tmp, &w, NULL) == 0 && w <= maxw) break;
    }
    snprintf(out, outsz, "%s", len > 0 ? tmp : "...");
}

static void first_char(const char *s, char *out) {
    unsigned char c = (unsigned char)s[0];
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    memcpy(out, s, n); out[n] = 0;
    if (n == 1) out[0] = (char)toupper(c);
}

static void wtex_free(WTex *w) { if (w->t) SDL_DestroyTexture(w->t); w->t = NULL; }
static void wtex_make(WTex *w, int f, const char *s, int maxw, int lines) {
    wtex_free(w);
    if (!s || !s[0]) return;
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *sf = TTF_RenderUTF8_Blended_Wrapped(fonts[f], s, white, maxw);
    if (!sf) return;
    int lh = TTF_FontLineSkip(fonts[f]), h = sf->h;
    if (h > lh * lines) h = lh * lines;
    w->t = SDL_CreateTextureFromSurface(ren, sf);
    w->w = sf->w; w->h = h;
    SDL_FreeSurface(sf);
}
static void wtex_draw(WTex *w, int x, int y, int r, int g, int b, int a) {
    if (!w->t) return;
    tex_col(w->t, r, g, b, a);
    SDL_Rect s = { 0, 0, w->w, w->h }, d = { x, y, w->w, w->h };
    SDL_RenderCopy(ren, w->t, &s, &d);
}

/* ===== Button icons + hints ===== */
enum { BI_X, BI_O, BI_TRI, BI_SQ, BI_DLR };
static void btn_icon(int k, float cx, float cy) {
    switch (k) {
    case BI_X:   gc(124, 178, 232, 255); line_w(cx - 5, cy - 5, cx + 5, cy + 5, 2.2f); line_w(cx + 5, cy - 5, cx - 5, cy + 5, 2.2f); break;
    case BI_O:   rrect_line((int)cx - 7, (int)cy - 7, 14, 14, 7, 2, 240, 100, 100, 255); break;
    case BI_TRI: gc(64, 210, 170, 255); line_w(cx, cy - 6, cx + 6, cy + 5, 2); line_w(cx + 6, cy + 5, cx - 6, cy + 5, 2); line_w(cx - 6, cy + 5, cx, cy - 6, 2); break;
    case BI_SQ:  rrect_line((int)cx - 6, (int)cy - 6, 12, 12, 2, 2, 220, 130, 220, 255); break;
    case BI_DLR: gc(C_SOFT, 255); icon(IC_LEFT, cx - 6, cy, 0.7f, 0, 0, 0); icon(IC_RIGHT, cx + 6, cy, 0.7f, 0, 0, 0); break;
    }
}
static int hint(int x, int kind, const char *label) {
    btn_icon(kind, x + 8, HINT_CY);
    int w = textc(F_SMALL, x + 22, HINT_CY, AL_L, label, C_SOFT, 255);
    return x + 22 + w + 24;
}
static int hint_key(int x, const char *key, const char *label) {
    int kw = text_w(F_SMALLB, key) + 14;
    rrect_line(x, HINT_CY - 10, kw, 20, 6, 1, C_SOFT, 220);
    textc(F_SMALLB, x + kw / 2, HINT_CY, AL_C, key, C_SOFT, 255);
    int w = textc(F_SMALL, x + kw + 8, HINT_CY, AL_L, label, C_SOFT, 255);
    return x + kw + 8 + w + 24;
}

/* ===== Images ===== */
static void round_mask(SDL_Surface *s, int rad) {
    Uint32 *px = s->pixels; int p = s->pitch / 4;
    for (int y = 0; y < rad; y++)
        for (int x = 0; x < rad; x++) {
            float dx = rad - (x + 0.5f), dy = rad - (y + 0.5f), d = sqrtf(dx * dx + dy * dy);
            float a = clampf(rad - d + 0.5f, 0, 1);
            if (a >= 1) continue;
            int xs[2] = { x, s->w - 1 - x }, ys[2] = { y, s->h - 1 - y };
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++) {
                    Uint32 *q = &px[ys[j] * p + xs[i]];
                    Uint32 al = (Uint32)((*q >> 24) * a);
                    *q = (*q & 0xFFFFFF) | (al << 24);
                }
        }
}

static SDL_Texture *load_image_fit(const char *path, int W, int H, int round) {
    SDL_Surface *s = IMG_Load(path);
    if (!s) return NULL;
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(s);
    if (!c) return NULL;
    SDL_Rect src = { 0, 0, c->w, c->h };
    if ((long)c->w * H > (long)c->h * W) { src.w = c->h * W / H; src.x = (c->w - src.w) / 2; }
    else                                 { src.h = c->w * H / W; src.y = (c->h - src.h) / 2; }
    SDL_Surface *o = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!o) { SDL_FreeSurface(c); return NULL; }
    SDL_SetSurfaceBlendMode(c, SDL_BLENDMODE_NONE);
    if (SDL_SoftStretchLinear(c, &src, o, NULL) < 0) SDL_BlitScaled(c, &src, o, NULL);
    SDL_FreeSurface(c);
    if (round > 0) round_mask(o, round);
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, o);
    SDL_FreeSurface(o);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static void ensure_cover(Title *tl) {
    if (tl->cover_state != 0) return;
    tl->cover = load_image_fit(tl->cover_path, POSTER_W, POSTER_H, 16);
    tl->cover_state = tl->cover ? 1 : 2;
}

typedef struct { int ti; SDL_Texture *t; unsigned used; } BCache;
static BCache bcache[3] = { { -1, NULL, 0 }, { -1, NULL, 0 }, { -1, NULL, 0 } };
static void bcache_clear(void) {
    for (int i = 0; i < 3; i++) {
        if (bcache[i].t) SDL_DestroyTexture(bcache[i].t);
        bcache[i].t = NULL; bcache[i].ti = -1; bcache[i].used = 0;
    }
}
static SDL_Texture *get_banner(int ti) {
    if (!titles[ti].banner_path[0]) return NULL;
    int lru = 0;
    for (int i = 0; i < 3; i++) {
        if (bcache[i].ti == ti) { bcache[i].used = frame_no; return bcache[i].t; }
        if (bcache[i].used < bcache[lru].used) lru = i;
    }
    if (bcache[lru].t) SDL_DestroyTexture(bcache[lru].t);
    bcache[lru].ti = ti;
    bcache[lru].used = frame_no;
    bcache[lru].t = load_image_fit(titles[ti].banner_path, BANNER_W, BANNER_H, 0);
    return bcache[lru].t;
}

static SDL_Texture *make_dots(void) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 480, 272, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return NULL;
    Uint32 *px = s->pixels; int p = s->pitch / 4;
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++)
            px[y * p + x] = (x % 4 == 1 && y % 4 == 1) ? 0xFFFFFFFF : 0x00FFFFFF;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

/* backdrop for hero and details: banner (or blurred cover) + dark fades + dots */
static void draw_backdrop(int ti, int y, int h, int a) {
    Title *tl = &titles[ti];
    SDL_Texture *t = get_banner(ti);
    SDL_Rect src;
    int dim;
    if (t) {
        src.w = BANNER_W; src.h = BANNER_W * h / SCREEN_W;
        if (src.h > BANNER_H) src.h = BANNER_H;
        src.x = 0; src.y = (BANNER_H - src.h) * 2 / 5;
        dim = 135;
    } else {
        ensure_cover(tl);
        t = tl->cover;
        src.w = POSTER_W; src.h = POSTER_W * h / SCREEN_W;
        src.x = 0; src.y = POSTER_H * 2 / 5 - src.h / 2;
        dim = 85;
    }
    if (t) {
        SDL_Rect d = { 0, y, SCREEN_W, h };
        tex_col(t, dim, dim, dim + 8, a);
        SDL_RenderCopy(ren, t, &src, &d);
        tex_col(t, 255, 255, 255, 255);
    }
    grad_h(0, y, 640, h, C_BG, 235 * a / 255, 0);
    grad_v(0, y + h - 160, SCREEN_W, 160, C_BG, 0, 255);
    if (dots_tex) {
        SDL_Rect s2 = { 0, 0, 480, h / 2 > 272 ? 272 : h / 2 }, d2 = { 0, y, SCREEN_W, s2.h * 2 };
        tex_col(dots_tex, 255, 255, 255, 20 * a / 255);
        SDL_RenderCopy(ren, dots_tex, &s2, &d2);
    }
}

/* ===== END OF PART 1 ===== */
/* ===== Library scan ===== */
static int parse_num(const char *name) {   /* chapter/volume number from a folder or file name */
    char c[256];
    int n = 0, depth = 0;
    for (const char *p = name; *p && n < 255; p++) {
        if (*p == '[' || *p == '(' || *p == '{') { depth++; continue; }
        if (*p == ']' || *p == ')' || *p == '}') { if (depth) depth--; continue; }
        if (!depth) c[n++] = (*p == '_' || *p == '.' || *p == '-') ? ' ' : (char)tolower((unsigned char)*p);
    }
    c[n] = 0;
    static const char *const keys[] = { "chapter", "chap", "ch", "volume", "vol", "v", "c" };
    for (int i = 0; c[i]; i++) {
        if (i > 0 && isalpha((unsigned char)c[i - 1])) continue;
        for (int k = 0; k < (int)(sizeof keys / sizeof *keys); k++) {
            int kl = (int)strlen(keys[k]);
            if (strncmp(&c[i], keys[k], kl)) continue;
            int j = i + kl;
            while (c[j] == ' ') j++;
            if (isdigit((unsigned char)c[j])) return atoi(&c[j]);
        }
    }
    int last = 0, found = 0;
    for (int i = 0; c[i]; ) {
        if (isdigit((unsigned char)c[i])) {
            int j = i, v = 0;
            while (isdigit((unsigned char)c[j])) { if (v < 100000) v = v * 10 + (c[j] - '0'); j++; }
            last = v; found = 1; i = j;
        } else i++;
    }
    return found ? last : 0;
}

static void nice_name(char *o, int n, const char *s) {   /* folder name -> shown name */
    snprintf(o, n, "%s", s);
    for (char *p = o; *p; p++) if (*p == '_') *p = ' ';
}

static int is_novel_file(const char *n) {
    if (!strcasecmp(n, "info.txt") || !strcasecmp(n, "title_map.txt")) return 0;
    return ends_with_ci(n, ".epub") || ends_with_ci(n, ".pdf") || ends_with_ci(n, ".txt");
}

static void add_ch(Title *tl, const char *dir, const char *name, int is_file) {
    if (tl->nchs >= tl->cap) {
        int nc = tl->cap ? tl->cap * 2 : 32;
        Chapter *n = realloc(tl->chs, nc * sizeof(Chapter));
        if (!n) return;
        tl->chs = n; tl->cap = nc;
    }
    Chapter *c = &tl->chs[tl->nchs++];
    memset(c, 0, sizeof *c);
    snprintf(c->path, sizeof c->path, "%s/%s", dir, name);
    snprintf(c->file, sizeof c->file, "%s", name);
    if (is_file) { char *dot = strrchr(c->file, '.'); if (dot) *dot = 0; }
    c->number = parse_num(c->file);
}

static void scan_chapters(Title *tl, const char *dir) {   /* manga: every sub-folder = a chapter */
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent de;
    for (;;) {
        memset(&de, 0, sizeof de);
        if (sceIoDread(d, &de) <= 0) break;
        if (de.d_name[0] == '.') continue;
        if (!SCE_S_ISDIR(de.d_stat.st_mode)) continue;
        if (!strcasecmp(de.d_name, "meta") || !strcasecmp(de.d_name, "chapters")) continue;
        add_ch(tl, dir, de.d_name, 0);
    }
    sceIoDclose(d);
}

static void scan_novel_files(Title *tl, const char *dir) {   /* novels: every epub/pdf/txt = a volume */
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent de;
    for (;;) {
        memset(&de, 0, sizeof de);
        if (sceIoDread(d, &de) <= 0) break;
        if (de.d_name[0] == '.' || SCE_S_ISDIR(de.d_stat.st_mode)) continue;
        if (is_novel_file(de.d_name)) add_ch(tl, dir, de.d_name, 1);
    }
    sceIoDclose(d);
}

static int ch_cmp(const void *a, const void *b) {
    const Chapter *x = a, *y = b;
    if (x->number != y->number) return x->number - y->number;
    return strcasecmp(x->file, y->file);
}
static int title_cmp(const void *a, const void *b) {
    return strcasecmp(((const Title *)a)->name, ((const Title *)b)->name);
}

static void load_info(Title *tl, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1200];
    int first = 1;
    while (fgets(line, sizeof line, f)) {
        char *l = line;
        if (first && (unsigned char)l[0] == 0xEF && (unsigned char)l[1] == 0xBB && (unsigned char)l[2] == 0xBF) l += 3;
        first = 0;
        l[strcspn(l, "\r\n")] = 0;
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = l, *v = eq + 1;
        if (!strcasecmp(k, "title") && v[0]) snprintf(tl->name, sizeof tl->name, "%s", v);
        else if (!strcasecmp(k, "synopsis")) snprintf(tl->synopsis, sizeof tl->synopsis, "%s", v);
        else if (!strcasecmp(k, "year"))     snprintf(tl->year, sizeof tl->year, "%s", v);
        else if (!strcasecmp(k, "genres"))   snprintf(tl->genres, sizeof tl->genres, "%s", v);
        else if (!strcasecmp(k, "rating"))   snprintf(tl->rating, sizeof tl->rating, "%s", v);
        else if (!strcasecmp(k, "type"))     snprintf(tl->type, sizeof tl->type, "%s", v);
        else if (!strcasecmp(k, "status"))   snprintf(tl->status, sizeof tl->status, "%s", v);
    }
    fclose(f);
}

static int find_img(char *out, int n, const char *dir, const char *const *names, int cnt) {
    char d[512];
    snprintf(d, sizeof d, "%s", dir);   /* local copy: dir and out can live in the same Title */
    for (int i = 0; i < cnt; i++) {
        snprintf(out, n, "%s/%s", d, names[i]);
        if (file_exists(out)) return 1;
    }
    out[0] = 0;
    return 0;
}
static int find_meta_img(char *out, int n, const char *dir, const char *const *names, int cnt) {
    char m[512];
    snprintf(m, sizeof m, "%s/meta", dir);   /* meta/ first, then the title folder */
    if (find_img(out, n, m, names, cnt)) return 1;
    return find_img(out, n, dir, names, cnt);
}

static void finish_title(Title *tl) {
    qsort(tl->chs, tl->nchs, sizeof(Chapter), ch_cmp);
    for (int i = 0; i < tl->nchs; i++) {
        Chapter *c = &tl->chs[i];
        if (c->number > 0) snprintf(c->label, sizeof c->label, "%s %d", tl->kind == K_MANGA ? "Chapter" : "Volume", c->number);
        else fit_text(F_BODYB, c->file, 690, c->label, sizeof c->label);
        fit_text(F_SMALL, c->file, 690, c->sub, sizeof c->sub);
    }
    tl->cover_state = tl->cover_path[0] ? 0 : 2;
    fit_text(F_SMALLB, tl->name, CARD_W + 4, tl->card_name, sizeof tl->card_name);
}

static const char *const cover_names[]  = { "cover.jpg", "cover.png", "poster.jpg", "poster.png", "folder.jpg", "cover.jpeg" };
static const char *const banner_names[] = { "banner.jpg", "banner.png", "fanart.jpg", "fanart.png", "backdrop.jpg" };

static void scan_root(const char *root, int kind) {
    SceUID d = sceIoDopen(root);
    if (d < 0) return;
    SceIoDirent de;
    while (ntitles < MAX_TITLES) {
        memset(&de, 0, sizeof de);
        if (sceIoDread(d, &de) <= 0) break;
        if (de.d_name[0] == '.') continue;
        Title *tl = &titles[ntitles];
        memset(tl, 0, sizeof *tl);
        tl->kind = kind;
        if (SCE_S_ISDIR(de.d_stat.st_mode)) {
            nice_name(tl->name, sizeof tl->name, de.d_name);
            snprintf(tl->dir, sizeof tl->dir, "%s/%s", root, de.d_name);
            if (kind == K_MANGA) {
                char cd[600];
                snprintf(cd, sizeof cd, "%s/chapters", tl->dir);
                scan_chapters(tl, file_exists(cd) ? cd : tl->dir);
            } else {
                scan_novel_files(tl, tl->dir);
                if (tl->nchs == 0) { free(tl->chs); memset(tl, 0, sizeof *tl); continue; }
            }
            find_meta_img(tl->cover_path, sizeof tl->cover_path, tl->dir, cover_names, 6);
            find_meta_img(tl->banner_path, sizeof tl->banner_path, tl->dir, banner_names, 5);
            if (!tl->banner_path[0]) snprintf(tl->banner_path, sizeof tl->banner_path, "%s", tl->cover_path);
            char ip[600];
            snprintf(ip, sizeof ip, "%s/meta/info.txt", tl->dir);
            if (!file_exists(ip)) snprintf(ip, sizeof ip, "%s/info.txt", tl->dir);
            load_info(tl, ip);
        } else if (kind == K_NOVEL && is_novel_file(de.d_name)) {   /* loose novel file */
            char base[256];
            snprintf(base, sizeof base, "%s", de.d_name);
            char *dot = strrchr(base, '.');
            if (dot) *dot = 0;
            nice_name(tl->name, sizeof tl->name, base);
            snprintf(tl->dir, sizeof tl->dir, "%s", root);
            add_ch(tl, root, de.d_name, 1);
            char a[300], b[300];
            snprintf(a, sizeof a, "%s.jpg", base);
            snprintf(b, sizeof b, "%s.png", base);
            const char *nm[2] = { a, b };
            find_img(tl->cover_path, sizeof tl->cover_path, root, nm, 2);
        } else continue;
        finish_title(tl);
        ntitles++;
    }
    sceIoDclose(d);
}

static void free_library(void) {
    for (int i = 0; i < ntitles; i++) {
        free(titles[i].chs);
        if (titles[i].cover) SDL_DestroyTexture(titles[i].cover);
    }
    memset(titles, 0, sizeof titles);
    ntitles = nmanga = nnovel = 0;
    bcache_clear();
}

static void scan_library(void) {
    free_library();
    scan_root(MANGA_ROOT, K_MANGA);
    scan_root(NOVEL_ROOT, K_NOVEL);
    qsort(titles, ntitles, sizeof(Title), title_cmp);
    for (int i = 0; i < ntitles; i++) {
        if (titles[i].kind == K_MANGA) manga_ix[nmanga++] = i;
        else novel_ix[nnovel++] = i;
    }
}

/* ===== Title text helpers ===== */
static const char *badge_of(Title *tl) {
    if (tl->type[0]) return tl->type;
    return tl->kind == K_NOVEL ? "NOVEL" : "MANGA";
}
static void add_part(char *o, int n, const char *p) {
    if (!p || !p[0]) return;
    if (o[0]) strncat(o, "  \xE2\x80\xA2  ", n - strlen(o) - 1);
    strncat(o, p, n - strlen(o) - 1);
}
static void title_meta(Title *tl, char *o, int n) {
    char t[48];
    o[0] = 0;
    add_part(o, n, badge_of(tl));
    add_part(o, n, tl->status);
    add_part(o, n, tl->year);
    if (tl->kind == K_MANGA) snprintf(t, sizeof t, tl->nchs == 1 ? "%d CHAPTER" : "%d CHAPTERS", tl->nchs);
    else snprintf(t, sizeof t, tl->nchs == 1 ? "%d VOLUME" : "%d VOLUMES", tl->nchs);
    add_part(o, n, t);
}
static void genres_fmt(const char *src, char *o, int n) {
    char tmp[200], *save = NULL;
    o[0] = 0;
    snprintf(tmp, sizeof tmp, "%s", src);
    for (char *t = strtok_r(tmp, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        while (*t == ' ') t++;
        int l = (int)strlen(t);
        while (l > 0 && t[l - 1] == ' ') t[--l] = 0;
        if (!l) continue;
        if (o[0]) strncat(o, "  /  ", n - strlen(o) - 1);
        strncat(o, t, n - strlen(o) - 1);
    }
}

/* ===== Shared widgets ===== */
static void sec_head(int x, int y, const char *small, const char *big) {
    rrect(x, y + 6, 28, 4, 2, C_ACCENT, 255);
    text(F_SMALLB, x + 40, y, AL_L, small, C_ACCLT, 255);
    text(F_H2, x, y + 20, AL_L, big, C_TEXT, 255);
}
static int pill_btn(int xr, int cy, const char *label, int chevron) {   /* right-aligned pill */
    int w = text_w(F_SMALLB, label) + (chevron ? 46 : 28), x = xr - w;
    rrect(x, cy - 18, w, 36, 14, C_PANEL, 255);
    rrect_line(x, cy - 18, w, 36, 14, 1, C_LINE, 255);
    textc(F_SMALLB, x + 14, cy, AL_L, label, C_SOFT, 255);
    if (chevron) { gc(C_SOFT, 255); icon(IC_RIGHT, xr - 20, cy, 0.6f, 0, 0, 0); }
    return x;
}
static int rating_pill(int x, int cy, const char *rating, int a) {
    int w = text_w(F_BODYB, rating) + 52;
    rrect(x, cy - 18, w, 36, 18, 14, 14, 20, a * 220 / 255);
    rrect_line(x, cy - 18, w, 36, 18, 1, C_LINE, a);
    gc(C_ACCENT, a); icon(IC_STAR, x + 20, cy, 0.85f, 0, 0, 0);
    textc(F_BODYB, x + 36, cy, AL_L, rating, C_TEXT, a);
    return w;
}
static void button(int x, int y, int w, int h, int primary, int ic, const char *label, int focus, int a) {
    if (primary) rrect(x, y, w, h, 14, C_ACCENT, a);
    else { rrect(x, y, w, h, 14, 16, 16, 22, a * 200 / 255); rrect_line(x, y, w, h, 14, 2, C_LINE, a); }
    if (focus) rrect_line(x - 5, y - 5, w + 10, h + 10, 18, 2, C_TEXT, a);
    int tw = text_w(F_BODYB, label), cx = x + (w - (tw + 30)) / 2;
    gc(C_TEXT, a);
    if (primary) icon(ic, cx + 9, y + h / 2, 0.8f, 108, 99, 255);
    else         icon(ic, cx + 9, y + h / 2, 0.8f, 16, 16, 22);
    textc(F_BODYB, cx + 30, y + h / 2, AL_L, label, C_TEXT, a);
}
static void circle_btn(int cx, int cy, int ic, int id, int content) {
    disc(cx, cy, 22, 16, 16, 22, 210);
    rrect_line(cx - 22, cy - 22, 44, 44, 22, 1, C_LINE, 255);
    gc(C_TEXT, 255); icon(ic, cx, cy, 0.85f, 16, 16, 22);
    hit(cx - 24, cy - 24, 48, 48, id, 0, content);
}

/* ===== Reading progress + settings ===== */
#define PROG_PATH   DATA_ROOT "/progress.txt"
#define PROG_TMP    DATA_ROOT "/progress.tmp"
#define CFG_PATH    DATA_ROOT "/settings.txt"
#define CW_MAX      10         /* Continue Reading cards */
#define ST_N        5
#define ST_CLEAR    4

enum { HR_CW, HR_MANGA, HR_NOVEL };   /* home rows; h_focus = row + 1, 0 = hero */

typedef struct { char path[512]; unsigned hash, seq; int page, pages, done; } Prog;
static Prog *prog;
static int nprog, prog_cap, prog_dirty;
static unsigned prog_seq, prog_gen = 1;

typedef struct { char key[512]; unsigned hash, seq; } Hide;   /* titles removed from Continue Reading */
static Hide *hide;
static int nhide, hide_cap;

/* settings (settings.txt) - used by the reader (next step) */
static int cfg_auto = 1;     /* next chapter when one ends */
static int cfg_orient = 0;   /* 0 landscape, 1 portrait (Vita turned sideways) */
static int cfg_resume = 0;   /* 0 ask, 1 always resume */
static int cfg_fit = 0;      /* 0 fit screen, 1 fit width */
static int in_settings, st_sel, st_confirm;

/* Continue Reading */
typedef struct { int ti, ch; unsigned seq; int page, pages; char tag[24], left[24]; } CWItem;
static CWItem cw[CW_MAX];
static int ncw;
static unsigned cw_gen;

static void settings_open(void);

static Prog *prog_find(const char *path) {
    unsigned h = str_hash(path);
    for (int i = 0; i < nprog; i++)
        if (prog[i].hash == h && !strcmp(prog[i].path, path)) return &prog[i];
    return NULL;
}

static Prog *prog_get(const char *path) {   /* find or add (pointers die on the next add) */
    Prog *p = prog_find(path);
    if (p) return p;
    if (nprog == prog_cap) {
        int nc = prog_cap ? prog_cap * 2 : 256;
        Prog *np = realloc(prog, nc * sizeof(Prog));
        if (!np) return NULL;
        prog = np; prog_cap = nc;
    }
    p = &prog[nprog++];
    memset(p, 0, sizeof *p);
    snprintf(p->path, sizeof p->path, "%s", path);
    p->hash = str_hash(p->path);
    return p;
}

/* the reader (next step) calls this; page counts from 0 */
static __attribute__((unused)) void prog_set(const char *path, int pg, int pages) {
    if (pages <= 0) return;
    Prog *p = prog_get(path);
    if (!p) return;
    if (pg < 0) pg = 0;
    if (pg > pages - 1) pg = pages - 1;
    p->pages = pages;
    if (pg >= pages - 1) { p->done = 1; p->page = 0; }
    else p->page = pg;
    p->seq = ++prog_seq;
    prog_dirty = 1;
    prog_gen++;
}

static Hide *hide_find(const char *key) {
    unsigned h = str_hash(key);
    for (int i = 0; i < nhide; i++)
        if (hide[i].hash == h && !strcmp(hide[i].key, key)) return &hide[i];
    return NULL;
}

static void hide_set(const char *key, unsigned seq) {
    Hide *h = hide_find(key);
    if (!h) {
        if (nhide == hide_cap) {
            int nc = hide_cap ? hide_cap * 2 : 32;
            Hide *nh = realloc(hide, nc * sizeof(Hide));
            if (!nh) return;
            hide = nh; hide_cap = nc;
        }
        h = &hide[nhide++];
        memset(h, 0, sizeof *h);
        snprintf(h->key, sizeof h->key, "%s", key);
        h->hash = str_hash(h->key);
    }
    h->seq = seq;
    prog_dirty = 1;
}

static void prog_load(void) {
    FILE *f = fopen(PROG_PATH, "r");
    if (!f) return;
    char l[700];
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n")] = 0;
        char *s, *e;
        if (l[0] == 'H' && l[1] == '\t') {
            unsigned long hs = strtoul(l + 2, &e, 10);
            if (*e == '\t' && e[1]) hide_set(e + 1, (unsigned)hs);
            continue;
        }
        if (l[0] != 'P' || l[1] != '\t') continue;
        s = l + 2;
        long pg = strtol(s, &e, 10);             if (*e != '\t') continue; s = e + 1;
        long pages = strtol(s, &e, 10);          if (*e != '\t') continue; s = e + 1;
        long done = strtol(s, &e, 10);           if (*e != '\t') continue; s = e + 1;
        unsigned long seq = strtoul(s, &e, 10);  if (*e != '\t' || !e[1]) continue;
        Prog *p = prog_get(e + 1);
        if (!p) break;
        p->page = (int)pg; p->pages = (int)pages; p->done = done != 0; p->seq = (unsigned)seq;
        if (p->seq > prog_seq) prog_seq = p->seq;
    }
    fclose(f);
    prog_dirty = 0;
    prog_gen++;
}

static void prog_save(void) {
    if (!prog_dirty) return;
    FILE *f = fopen(PROG_TMP, "w");
    if (!f) return;
    fprintf(f, "# aniread progress v1: P page pages done seq path / H seq key\n");
    for (int i = 0; i < nprog; i++)
        fprintf(f, "P\t%d\t%d\t%d\t%u\t%s\n", prog[i].page, prog[i].pages, prog[i].done, prog[i].seq, prog[i].path);
    for (int i = 0; i < nhide; i++)
        fprintf(f, "H\t%u\t%s\n", hide[i].seq, hide[i].key);
    if (fclose(f) != 0) return;
    remove(PROG_PATH);
    rename(PROG_TMP, PROG_PATH);
    prog_dirty = 0;
}

static void prog_clear(void) {
    free(prog); prog = NULL; nprog = prog_cap = 0;
    free(hide); hide = NULL; nhide = hide_cap = 0;
    prog_seq = 0;
    prog_dirty = 1;
    prog_save();
    prog_gen++;
}

static int cfg_nvsize = 2;   /* novel text size: index into nv_sizes */
static int cfg_nvtheme = 0;  /* 0 Black, 1 Dark Grey, 2 Sepia, 3 White */
static int cfg_nvfont = 0;   /* 0 Outfit, 1 Literata */
static int cfg_nvline = 1;   /* 0 tight, 1 normal, 2 wide */
static int cfg_nvmargin = 1; /* 0 narrow, 1 normal, 2 wide */
static int cfg_nvjust = 0;   /* justified text */

static void cfg_load(void) {
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) return;
    char l[128];
    int v;
    while (fgets(l, sizeof l, f)) {
        if (sscanf(l, "auto_next=%d", &v) == 1) cfg_auto = v != 0;
        else if (sscanf(l, "orient=%d", &v) == 1) cfg_orient = v != 0;
        else if (sscanf(l, "resume=%d", &v) == 1) cfg_resume = v != 0;
        else if (sscanf(l, "fit=%d", &v) == 1) cfg_fit = v != 0;
        else if (sscanf(l, "nv_size=%d", &v) == 1) cfg_nvsize = v;
        else if (sscanf(l, "nv_theme=%d", &v) == 1) cfg_nvtheme = v >= 0 && v < 4 ? v : 0;
        else if (sscanf(l, "nv_font=%d", &v) == 1) cfg_nvfont = v != 0;
        else if (sscanf(l, "nv_line=%d", &v) == 1) cfg_nvline = v >= 0 && v < 3 ? v : 1;
        else if (sscanf(l, "nv_margin=%d", &v) == 1) cfg_nvmargin = v >= 0 && v < 3 ? v : 1;
        else if (sscanf(l, "nv_just=%d", &v) == 1) cfg_nvjust = v != 0;
    }
    fclose(f);
}

static void cfg_save(void) {
    FILE *f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "auto_next=%d\norient=%d\nresume=%d\nfit=%d\nnv_size=%d\n", cfg_auto, cfg_orient, cfg_resume, cfg_fit, cfg_nvsize);
    fprintf(f, "nv_theme=%d\nnv_font=%d\nnv_line=%d\nnv_margin=%d\nnv_just=%d\n", cfg_nvtheme, cfg_nvfont, cfg_nvline, cfg_nvmargin, cfg_nvjust);
    fclose(f);
}

static void title_prog(Title *tl) {   /* cached per title, redone when prog_gen changes */
    if (tl->pg_gen == prog_gen) return;
    tl->pg_gen = prog_gen; tl->pg_done = 0; tl->pg_f = 0;
    if (tl->nchs <= 0 || nprog == 0) return;
    float sum = 0;
    for (int i = 0; i < tl->nchs; i++) {
        Prog *p = prog_find(tl->chs[i].path);
        if (!p) continue;
        if (p->done) { tl->pg_done++; sum += 1; }
        else if (p->pages > 0) sum += (float)p->page / (float)p->pages;
    }
    tl->pg_f = sum / tl->nchs;
}

static void check_mark(float cx, float cy, float s) {   /* uses the gc() colour */
    line_w(cx - 6 * s, cy, cx - 2 * s, cy + 4 * s, 2.6f * s);
    line_w(cx - 2 * s, cy + 4 * s, cx + 6 * s, cy - 4 * s, 2.6f * s);
}

static void cw_fill(CWItem *c, int ti, int ch, unsigned seq, int pg, int pages) {
    Title *tl = &titles[ti];
    Chapter *e = &tl->chs[ch];
    const char *k = tl->kind == K_MANGA ? "CH" : "VOL";
    c->ti = ti; c->ch = ch; c->seq = seq; c->page = pg; c->pages = pages;
    snprintf(c->tag, sizeof c->tag, "%s %d", k, e->number > 0 ? e->number : ch + 1);
    if (pg <= 0 || pages <= 0) snprintf(c->left, sizeof c->left, "UP NEXT");
    else if (tl->kind == K_NOVEL) snprintf(c->left, sizeof c->left, "%d%% READ", pg * 100 / pages);
    else snprintf(c->left, sizeof c->left, "PAGE %d / %d", pg + 1, pages);
}

static void cw_refresh(void) {   /* newest first, max CW_MAX, redone when prog_gen changes */
    if (cw_gen == prog_gen) return;
    cw_gen = prog_gen;
    ncw = 0;
    for (int s = 0; s < ntitles; s++) {
        Title *tl = &titles[s];
        int last = -1;
        unsigned best = 0;
        Prog *lp = NULL;
        for (int i = 0; i < tl->nchs; i++) {
            Prog *p = prog_find(tl->chs[i].path);
            if (p && p->seq > best) { best = p->seq; last = i; lp = p; }
        }
        if (last < 0) continue;
        Hide *hd = hide_find(tl->chs[0].path);
        if (hd && hd->seq >= best) continue;
        int ch = last, pg = lp->page, pages = lp->pages;
        if (lp->done) {   /* finished: point at the next unread chapter */
            ch = -1;
            for (int i = last + 1; i < tl->nchs; i++) {
                Prog *q = prog_find(tl->chs[i].path);
                if (!q || !q->done) { ch = i; pg = q ? q->page : 0; pages = q ? q->pages : 0; break; }
            }
            if (ch < 0) continue;
        }
        if (ncw == CW_MAX && best <= cw[CW_MAX - 1].seq) continue;
        int at = ncw < CW_MAX ? ncw : CW_MAX - 1;
        while (at > 0 && cw[at - 1].seq < best) { cw[at] = cw[at - 1]; at--; }
        cw_fill(&cw[at], s, ch, best, pg, pages);
        if (ncw < CW_MAX) ncw++;
    }
}

static void cw_remove(int i) {
    if (i < 0 || i >= ncw) return;
    hide_set(titles[cw[i].ti].chs[0].path, cw[i].seq);
    prog_save();
    prog_gen++;
    toast("Removed from Continue Reading");
}

static void draw_card(int ti, int x, int y, int w, int h, int sel) {
    Title *tl = &titles[ti];
    if (tl->cover_state == 0 && cover_budget > 0) { cover_budget--; ensure_cover(tl); }
    if (tl->cover) {
        SDL_Rect d = { x, y, w, h };
        tex_col(tl->cover, 255, 255, 255, 255);
        SDL_RenderCopy(ren, tl->cover, NULL, &d);
    } else {
        char fc[8]; first_char(tl->name, fc);
        rrect(x, y, w, h, 10, C_PANEL, 255);
        textc(F_H2, x + w / 2, y + h / 2, AL_C, fc, C_DIM, 255);
    }
    const char *b = badge_of(tl);
    int bw = text_w(F_SMALLB, b) + 16;
    rrect(x + 8, y + 8, bw, 26, 9, 18, 18, 24, 215);
    textc(F_SMALLB, x + 8 + bw / 2, y + 21, AL_C, b, C_TEXT, 255);
    if (tl->kind == K_MANGA || tl->nchs > 1) {
        char e[24];
        snprintf(e, sizeof e, tl->kind == K_MANGA ? "%d CH" : "%d VOL", tl->nchs);
        int ew = text_w(F_SMALLB, e) + 14;
        rrect(x + w - ew - 8, y + h - 32, ew, 24, 8, 18, 18, 24, 215);
        textc(F_SMALLB, x + w - ew / 2 - 8, y + h - 20, AL_C, e, C_SOFT, 255);
    }
    title_prog(tl);
    if (tl->pg_f > 0.001f) {
        int bw2 = w - 16, fw = (int)(bw2 * clampf(tl->pg_f, 0, 1));
        rrect(x + 8, y + h - 6, bw2, 4, 2, 0, 0, 0, 190);
        if (fw >= 4) rrect(x + 8, y + h - 6, fw, 4, 2, C_ACCENT, 255);
    }
    if (tl->nchs > 0 && tl->pg_done >= tl->nchs) {
        disc(x + w - 21, y + 21, 13, C_ACCENT, 235);
        gc(C_TEXT, 255); check_mark(x + w - 21, y + 21, 0.75f);
    }

    if (sel) rrect_line(x - 5, y - 5, w + 10, h + 10, 15, 3, C_ACCENT, 255);
    if (sel) text(F_SMALLB, x, y + h + 10, AL_L, tl->card_name, C_ACCLT, 255);
    else     text(F_SMALLB, x, y + h + 10, AL_L, tl->card_name, C_TEXT, 255);
}

/* ===== Input ===== */
static int pressed(unsigned b) { return (btn & b) && !(btn_old & b); }

static int nav_rep(int i, int held, float dt) {
    if (!held) { rep_on[i] = 0; return 0; }
    if (!rep_on[i]) { rep_on[i] = 1; rep_t[i] = 0.35f; return 1; }
    rep_t[i] -= dt;
    if (rep_t[i] <= 0) { rep_t[i] += 0.07f; return 1; }
    return 0;
}
static int n_up(float dt)    { return nav_rep(0, (btn & SCE_CTRL_UP) || st_up, dt); }
static int n_down(float dt)  { return nav_rep(1, (btn & SCE_CTRL_DOWN) || st_down, dt); }
static int n_left(float dt)  { return nav_rep(2, (btn & SCE_CTRL_LEFT) || st_left, dt); }
static int n_right(float dt) { return nav_rep(3, (btn & SCE_CTRL_RIGHT) || st_right, dt); }

static int pad_lx = 128, pad_ly = 128, pad_rx = 128, pad_ry = 128;   /* raw sticks (reader panning) */
static void read_input(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositive(0, &pad, 1);
    btn_old = btn;
    btn = pad.buttons;
    pad_lx = pad.lx; pad_ly = pad.ly; pad_rx = pad.rx; pad_ry = pad.ry;
    /* left OR right stick moves the selection */
    st_up    = pad.ly < 50  || pad.ry < 50;
    st_down  = pad.ly > 205 || pad.ry > 205;
    st_left  = pad.lx < 50  || pad.rx < 50;
    st_right = pad.lx > 205 || pad.rx > 205;
}

static SDL_TouchID touch_front_id(void) {   /* front panel ID: 0 on old SDL2, 1 since 2.30.7 */
    static int done;
    static SDL_TouchID id;
    if (done) return id;
    int n = SDL_GetNumTouchDevices();
    if (n <= 0) return 0;
    id = SDL_GetTouchDevice(0);              /* SDL adds the front panel first */
    for (int i = 0; i < n; i++) {
        const char *nm = SDL_GetTouchName(i);
        if (nm && !strcasecmp(nm, "Front")) { id = SDL_GetTouchDevice(i); break; }
    }
    done = 1;
    return id;
}

static void handle_touch_event(SDL_Event *ev) {
    if (ev->type != SDL_FINGERDOWN && ev->type != SDL_FINGERMOTION && ev->type != SDL_FINGERUP) return;
    if (ev->tfinger.touchId != touch_front_id()) return;   /* front panel only */
    float x = ev->tfinger.x * SCREEN_W, y = ev->tfinger.y * SCREEN_H;
    if (ev->type == SDL_FINGERDOWN) {
        t_down = 1; t_axis = 0; t_x0 = t_lx = x; t_y0 = t_ly = y;
    } else if (ev->type == SDL_FINGERMOTION && t_down) {
        if (!t_axis) {
            float dx = x - t_x0, dy = y - t_y0;
            if (fabsf(dx) > 12 || fabsf(dy) > 12) t_axis = fabsf(dx) > fabsf(dy) ? 2 : 1;
        }
        if (t_axis == 1) drag_dy += t_ly - y;
        if (t_axis == 2) drag_dx += t_lx - x;
        t_lx = x; t_ly = y;
    } else if (ev->type == SDL_FINGERUP && t_down) {
        if (!t_axis) { tap_pending = 1; tap_x = t_x0; tap_y = t_y0; }
        t_down = 0; t_axis = 0;
    }
}

/* ===== END OF PART 2 ===== */
/* ===== Actions ===== */
static int title_resume_ch(Title *tl) {   /* chapter "Read Now" opens: last read, or the next one */
    int best = -1;
    unsigned seq = 0;
    for (int i = 0; i < tl->nchs; i++) {
        Prog *p = prog_find(tl->chs[i].path);
        if (p && p->seq > seq) { seq = p->seq; best = i; }
    }
    if (best < 0) return 0;
    Prog *p = prog_find(tl->chs[best].path);
    if (p && p->done && best + 1 < tl->nchs) return best + 1;
    return best;
}

/* ===== Manga reader (R-1) ===== */
#define RD_SLOTS   3              /* decoded pages kept: current, next, previous */
#define RD_TILE    2048           /* tall pages are split into textures this high */
#define RD_TILES   6
#define RD_MAXW    1200           /* wider pages are scaled down */
#define RD_MAXPX   (1200 * 4096)  /* pixel budget per page */
#define RD_PAGES   1000
#define RD_NAME    256
#define RD_PW      544            /* portrait view: the Vita turned sideways */
#define RD_PH      960
#define RD_LAST    (-1)           /* start page: the last page */
#define RD_AUTO    (-2)           /* start page: ask or resume (Resume setting) */
#define RD_RESUME  (-3)           /* start page: always resume */
#define RD_BAR_X   40

enum { RS_FREE, RS_WANT, RS_BUSY, RS_SURF, RS_READY, RS_FAIL };
typedef struct {
    int pg, state, w, h, nt, th[RD_TILES];
    unsigned gen;
    SDL_Surface *s;
    SDL_Texture *t[RD_TILES];
} RdSlot;

static RdSlot rd_slot[RD_SLOTS];
static SDL_mutex *rd_mx;
static SDL_sem *rd_sem;
static SDL_Thread *rd_thr;
static SDL_Texture *rd_rt;            /* portrait render target */
static int rd_rt_tried;
static char (*rd_files)[RD_NAME];
static char rd_dir[512], rd_title[160], rd_chlab[160], rd_b1[160], rd_b2[200];
static int in_reader, rd_ti, rd_ch, rd_n, rd_page, rd_ui, rd_ask, rd_end, rd_btn;
static int rd_webtoon, rd_portrait, rd_snap, rd_resume_pg;
static float rd_zoom = 1, rd_px, rd_py, rd_ty, rd_load_t, rd_save_t, rd_idle, rd_tick_t;
/* touch, in view coordinates */
static int rd_fd, rd_fmoved, rd_scrub, rd_tap, rd_swipe;
static SDL_FingerID rd_fid;
static Uint32 rd_ft0;
static float rd_fx0, rd_fy0, rd_flx, rd_fly, rd_dx, rd_dy, rd_tx, rd_tyy;

static void details_retarget(void);

static int rd_vw(void) { return rd_portrait ? RD_PW : SCREEN_W; }
static int rd_vh(void) { return rd_portrait ? RD_PH : SCREEN_H; }

static int rd_sniff(const char *dir, const char *n) {   /* look at the first bytes of the file */
    char p[800];
    unsigned char b[12];
    snprintf(p, sizeof p, "%s/%s", dir, n);
    SceUID fd = sceIoOpen(p, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    int r = sceIoRead(fd, b, sizeof b);
    sceIoClose(fd);
    if (r < 4) return 0;
    if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return 1;                  /* JPEG */
    if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return 1;     /* PNG */
    if (b[0] == 'G' && b[1] == 'I' && b[2] == 'F' && b[3] == '8') return 1;      /* GIF */
    if (b[0] == 'B' && b[1] == 'M') return 1;                                    /* BMP */
    if (r >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) return 1; /* WebP */
    return 0;
}

static int rd_is_img(const char *dir, const char *n) {
    if (ends_with_ci(n, ".jpg") || ends_with_ci(n, ".jpeg") || ends_with_ci(n, ".png") ||
        ends_with_ci(n, ".webp") || ends_with_ci(n, ".bmp") || ends_with_ci(n, ".gif")) return 1;
    static const char *const skip[] = { ".txt", ".xml", ".json", ".nomedia", ".tmp", ".part",
                                        ".db", ".ini", ".epub", ".pdf", ".zip", ".cbz", ".mp4" };
    for (int i = 0; i < (int)(sizeof skip / sizeof *skip); i++) if (ends_with_ci(n, skip[i])) return 0;
    return rd_sniff(dir, n);   /* .image, no extension, anything else: check the contents */
}

static int rd_name_cmp(const void *a, const void *b) {   /* natural order: 2.jpg before 10.jpg */
    const unsigned char *x = a, *y = b;
    while (*x && *y) {
        if (isdigit(*x) && isdigit(*y)) {
            while (*x == '0') x++;
            while (*y == '0') y++;
            const unsigned char *xs = x, *ys = y;
            while (isdigit(*x)) x++;
            while (isdigit(*y)) y++;
            int lx = (int)(x - xs), ly = (int)(y - ys);
            if (lx != ly) return lx - ly;
            int c = strncmp((const char *)xs, (const char *)ys, lx);
            if (c) return c;
        } else {
            int c = tolower(*x) - tolower(*y);
            if (c) return c;
            x++; y++;
        }
    }
    return *x - *y;
}

static int rd_count(const char *dir) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) return 0;
    int n = 0;
    SceIoDirent de;
    for (;;) {
        memset(&de, 0, sizeof de);
        if (sceIoDread(d, &de) <= 0) break;
        if (de.d_name[0] != '.' && !SCE_S_ISDIR(de.d_stat.st_mode) && rd_is_img(dir, de.d_name)) n++;
    }
    sceIoDclose(d);
    return n;
}

static int rd_list(const char *dir) {   /* call with rd_mx held */
    SceUID d = sceIoDopen(dir);
    if (d < 0) return 0;
    int n = 0;
    SceIoDirent de;
    while (n < RD_PAGES) {
        memset(&de, 0, sizeof de);
        if (sceIoDread(d, &de) <= 0) break;
        if (de.d_name[0] == '.' || SCE_S_ISDIR(de.d_stat.st_mode) || !rd_is_img(dir, de.d_name)) continue;
        snprintf(rd_files[n++], RD_NAME, "%s", de.d_name);
    }
    sceIoDclose(d);
    qsort(rd_files, n, sizeof *rd_files, rd_name_cmp);
    return n;
}

/* ----- page loading (background thread decodes, main thread uploads) ----- */
static SDL_Surface *rd_decode(const char *path) {
    SDL_Surface *s = IMG_Load(path);
    if (!s) return NULL;
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(s);
    if (!c) return NULL;
    float k = 1;
    if (c->w > RD_MAXW) k = (float)RD_MAXW / c->w;
    float px = (float)c->w * c->h * k * k;
    if (px > RD_MAXPX) k *= sqrtf(RD_MAXPX / px);
    if (c->h * k > RD_TILE * RD_TILES) k = (float)(RD_TILE * RD_TILES) / c->h;
    if (k >= 0.999f) return c;
    int w = (int)(c->w * k), h = (int)(c->h * k);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    SDL_Surface *o = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!o) { SDL_FreeSurface(c); return NULL; }
    SDL_SetSurfaceBlendMode(c, SDL_BLENDMODE_NONE);
    if (SDL_SoftStretchLinear(c, NULL, o, NULL) < 0) SDL_BlitScaled(c, NULL, o, NULL);
    SDL_FreeSurface(c);
    return o;
}

static int rd_thread(void *u) {
    (void)u;
    for (;;) {
        SDL_SemWait(rd_sem);
        for (;;) {
            int si = -1, best = 1 << 30;
            unsigned gen = 0;
            char path[800];
            SDL_LockMutex(rd_mx);
            for (int i = 0; i < RD_SLOTS; i++) {
                if (rd_slot[i].state != RS_WANT) continue;
                int d = abs(rd_slot[i].pg - rd_page);
                if (d < best) { best = d; si = i; }
            }
            if (si >= 0) {
                rd_slot[si].state = RS_BUSY;
                gen = rd_slot[si].gen;
                snprintf(path, sizeof path, "%s/%s", rd_dir, rd_files[rd_slot[si].pg]);
            }
            SDL_UnlockMutex(rd_mx);
            if (si < 0) break;
            SDL_Surface *s = rd_decode(path);
            SDL_LockMutex(rd_mx);
            RdSlot *sl = &rd_slot[si];
            if (sl->gen == gen && sl->state == RS_BUSY) { sl->s = s; sl->state = s ? RS_SURF : RS_FAIL; s = NULL; }
            SDL_UnlockMutex(rd_mx);
            if (s) SDL_FreeSurface(s);   /* page was dropped while decoding */
        }
    }
    return 0;
}

static void rd_slot_free(RdSlot *sl) {   /* call with rd_mx held */
    for (int i = 0; i < sl->nt; i++) if (sl->t[i]) SDL_DestroyTexture(sl->t[i]);
    memset(sl->t, 0, sizeof sl->t);
    sl->nt = 0;
    if (sl->s) SDL_FreeSurface(sl->s);
    sl->s = NULL; sl->state = RS_FREE; sl->pg = -1; sl->gen++;
}

static RdSlot *rd_slot_of(int pg) {
    for (int i = 0; i < RD_SLOTS; i++)
        if (rd_slot[i].state != RS_FREE && rd_slot[i].pg == pg) return &rd_slot[i];
    return NULL;
}
static RdSlot *rd_cur(void) {
    RdSlot *s = rd_slot_of(rd_page);
    return s && s->state == RS_READY ? s : NULL;
}

static void rd_want(void) {   /* keep current, next and previous page loaded */
    int want[RD_SLOTS] = { rd_page, rd_page + 1, rd_page - 1 }, posted = 0;
    SDL_LockMutex(rd_mx);
    for (int k = 0; k < RD_SLOTS; k++) {
        int pg = want[k];
        if (pg < 0 || pg >= rd_n || rd_slot_of(pg)) continue;
        RdSlot *v = NULL;
        int vd = -1;
        for (int i = 0; i < RD_SLOTS; i++) {
            RdSlot *sl = &rd_slot[i];
            if (sl->state == RS_FREE) { v = sl; break; }
            int d = abs(sl->pg - rd_page);
            if (d > 1 && d > vd) { vd = d; v = sl; }
        }
        if (!v) continue;
        rd_slot_free(v);
        v->pg = pg; v->state = RS_WANT; posted = 1;
    }
    SDL_UnlockMutex(rd_mx);
    if (posted) SDL_SemPost(rd_sem);
}

static void rd_upload(void) {   /* one decoded page per frame becomes textures, nearest first */
    SDL_LockMutex(rd_mx);
    RdSlot *b = NULL;
    for (int i = 0; i < RD_SLOTS; i++) {
        RdSlot *sl = &rd_slot[i];
        if (sl->state == RS_SURF && (!b || abs(sl->pg - rd_page) < abs(b->pg - rd_page))) b = sl;
    }
    if (b) {
        SDL_Surface *s = b->s;
        int ok = 1;
        b->w = s->w; b->h = s->h; b->nt = 0;
        for (int y = 0; y < s->h && b->nt < RD_TILES; y += RD_TILE) {
            int h = s->h - y < RD_TILE ? s->h - y : RD_TILE;
            SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, s->w, h);
            if (!t) { ok = 0; break; }
            SDL_UpdateTexture(t, NULL, (Uint8 *)s->pixels + (size_t)y * s->pitch, s->pitch);
            b->t[b->nt] = t; b->th[b->nt] = h; b->nt++;
        }
        SDL_FreeSurface(s);
        b->s = NULL;
        if (!ok || b->nt == 0) {
            for (int i = 0; i < b->nt; i++) SDL_DestroyTexture(b->t[i]);
            memset(b->t, 0, sizeof b->t);
            b->nt = 0; b->state = RS_FAIL;
        } else b->state = RS_READY;
    }
    SDL_UnlockMutex(rd_mx);
}

/* ----- view ----- */
static float rd_scale(RdSlot *s) {
    float k = (float)rd_vw() / s->w;
    if (!rd_webtoon && !cfg_fit) { float k2 = (float)rd_vh() / s->h; if (k2 < k) k = k2; }
    return k * rd_zoom;
}
static void rd_clamp(RdSlot *s) {
    float k = rd_scale(s), mx = s->w * k - rd_vw(), my = s->h * k - rd_vh();
    rd_px = mx > 0 ? clampf(rd_px, 0, mx) : 0;
    rd_py = my > 0 ? clampf(rd_py, 0, my) : 0;
    rd_ty = my > 0 ? clampf(rd_ty, 0, my) : 0;
}
static void rd_set_zoom(float z, RdSlot *s) {   /* zoom around the middle of the screen */
    if (!s) { rd_zoom = z; return; }
    float vw = rd_vw(), vh = rd_vh(), k0 = rd_scale(s), dw = s->w * k0, dh = s->h * k0;
    float ox = dw < vw ? (vw - dw) / 2 : -rd_px, oy = dh < vh ? (vh - dh) / 2 : -rd_py;
    float cx = (vw / 2 - ox) / k0, cy = (vh / 2 - oy) / k0;
    rd_zoom = z;
    float k1 = rd_scale(s);
    rd_px = cx * k1 - vw / 2;
    rd_py = rd_ty = cy * k1 - vh / 2;
    rd_clamp(s);
}

static int rd_target(void) {   /* portrait needs a render target; made once */
    if (!rd_rt && !rd_rt_tried) {
        rd_rt_tried = 1;
        rd_rt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, RD_PW, RD_PH);
        if (rd_rt) SDL_SetTextureBlendMode(rd_rt, SDL_BLENDMODE_NONE);
        else toast("Portrait isn't supported, using landscape");
    }
    return rd_rt != NULL;
}

/* ----- chapters and pages ----- */
static void rd_goto(int p, int snap) {   /* snap 1 = show the top, 2 = the bottom */
    if (p < 0 || p >= rd_n) return;
    rd_page = p; rd_snap = snap; rd_load_t = 0;
    prog_set(rd_dir, p, rd_n);
}

static int rd_has_next(void) { return rd_ch + 1 < titles[rd_ti].nchs; }

static int rd_load(int ti, int ch, int start) {   /* 0 = that chapter has no pages (nothing changed) */
    Title *tl = &titles[ti];
    Chapter *c = &tl->chs[ch];
    if (rd_count(c->path) == 0) return 0;
    prog_save();
    SDL_LockMutex(rd_mx);
    for (int i = 0; i < RD_SLOTS; i++) rd_slot_free(&rd_slot[i]);
    snprintf(rd_dir, sizeof rd_dir, "%s", c->path);
    rd_n = rd_list(rd_dir);
    rd_page = 0;
    SDL_UnlockMutex(rd_mx);
    if (rd_n == 0) return 0;
    rd_ti = ti; rd_ch = ch;
    rd_webtoon = !strcasecmp(tl->type, "WEBTOON");
    fit_text(F_BODYB, tl->name, rd_vw() - 100, rd_title, sizeof rd_title);
    fit_text(F_SMALL, c->label, rd_vw() - 100, rd_chlab, sizeof rd_chlab);
    rd_ask = rd_end = 0; rd_btn = 0;
    rd_px = rd_py = rd_ty = 0; rd_load_t = 0;
    int pg = 0;
    Prog *p = prog_find(c->path);
    if (start == RD_LAST) pg = rd_n - 1;
    else if (start >= 0) pg = start < rd_n ? start : rd_n - 1;
    else if (p && !p->done && p->page > 0 && p->page < rd_n) {
        pg = p->page;
        if (start == RD_AUTO && !cfg_resume) {
            rd_ask = 1; rd_resume_pg = pg;
            snprintf(rd_b1, sizeof rd_b1, "Resume reading?");
            snprintf(rd_b2, sizeof rd_b2, "You stopped on page %d of %d.", pg + 1, rd_n);
        }
    }
    rd_page = pg;
    rd_snap = start == RD_LAST ? 2 : 1;
    if (!rd_ask) prog_set(rd_dir, pg, rd_n);
    return 1;
}

static void rd_close(void) {
    SDL_LockMutex(rd_mx);
    for (int i = 0; i < RD_SLOTS; i++) rd_slot_free(&rd_slot[i]);
    rd_n = 0;
    SDL_UnlockMutex(rd_mx);
    prog_save();
    cfg_save();
    in_reader = 0; rd_ask = rd_end = 0;
    toast_t = 0;
    if (in_details && d_ti == rd_ti && rd_ch < titles[rd_ti].nchs) {   /* Details: select the chapter you read */
        d_focus = 1; d_sel = rd_ch;
        details_retarget();
    }
}

static void nv_open(int ti, int ch, int start);
static void rd_open(int ti, int ch, int start) {
    if (ti < 0 || ti >= ntitles) return;
    Title *tl = &titles[ti];
    if (ch < 0 || ch >= tl->nchs) { toast(tl->kind == K_MANGA ? "No chapters in this title yet" : "No files in this title yet"); return; }
    if (tl->kind == K_NOVEL) { nv_open(ti, ch, start); return; }
    if (!rd_thr) { toast("The reader couldn't start"); return; }
    rd_portrait = cfg_orient && rd_target();
    if (!rd_load(ti, ch, start)) { toast("No pages in %s yet", tl->chs[ch].label); return; }
    in_reader = 1; rd_ui = 0; rd_zoom = 1;
    rd_fd = rd_scrub = rd_tap = rd_swipe = 0; rd_dx = rd_dy = 0;
    rd_idle = rd_save_t = rd_tick_t = 0;
}

static void read_chapter(int ti, int ch) { rd_open(ti, ch, RD_AUTO); }

static void rd_end_box(void) {
    if (rd_has_next()) {
        char t[200];
        snprintf(t, sizeof t, "End of %s", titles[rd_ti].chs[rd_ch].label);
        fit_text(F_SUB, t, 400, rd_b1, sizeof rd_b1);
        snprintf(t, sizeof t, "Up next: %s", titles[rd_ti].chs[rd_ch + 1].label);
        fit_text(F_BODY, t, 400, rd_b2, sizeof rd_b2);
    } else {
        snprintf(rd_b1, sizeof rd_b1, "You're all caught up");
        snprintf(rd_b2, sizeof rd_b2, "That was the last chapter you have.");
    }
    rd_end = 1; rd_btn = 0;
}

static void rd_finish(void) {   /* next pressed on the last page */
    if (rd_has_next() && cfg_auto) {
        if (rd_load(rd_ti, rd_ch + 1, 0)) { toast("%s", titles[rd_ti].chs[rd_ch].label); return; }
        toast("The next chapter has no pages");
    }
    rd_end_box();
}

static void rd_next(void) {
    if (rd_page < rd_n - 1) rd_goto(rd_page + 1, 1);
    else rd_finish();
}

static void rd_prev(int snap) {
    if (rd_page > 0) { rd_goto(rd_page - 1, snap); return; }
    if (rd_ch > 0 && rd_load(rd_ti, rd_ch - 1, RD_LAST)) { toast("%s", titles[rd_ti].chs[rd_ch].label); return; }
    toast(rd_ch > 0 ? "The previous chapter has no pages" : "This is the first page");
}

/* ----- resume / end-of-chapter box ----- */
static int rd_box_n(void) { return rd_ask ? 2 : (rd_has_next() ? 2 : 1); }
static void rd_box_geo(int *x, int *y, int *w, int *h) {
    *w = rd_vw() - 40 < 460 ? rd_vw() - 40 : 460; *h = 190;
    *x = (rd_vw() - *w) / 2; *y = (rd_vh() - *h) / 2;
}
static void rd_box_btn(int i, int *x, int *y, int *w, int *h) {
    int bx, by, bw, bh, n = rd_box_n();
    rd_box_geo(&bx, &by, &bw, &bh);
    *w = (bw - 48 - (n - 1) * 16) / n; *h = 48;
    *x = bx + 24 + i * (*w + 16); *y = by + bh - 24 - 48;
}
static void rd_box_pick(int i) {
    if (rd_ask) { rd_ask = 0; rd_goto(i == 0 ? rd_resume_pg : 0, 1); return; }
    if (rd_has_next() && i == 0) {
        if (rd_load(rd_ti, rd_ch + 1, 0)) toast("%s", titles[rd_ti].chs[rd_ch].label);
        else toast("The next chapter has no pages");
        return;
    }
    rd_close();
}

/* ----- touch ----- */
static int rd_bar_y(void) { return rd_vh() - 66; }
static int rd_on_bar(float x, float y) { return fabsf(y - rd_bar_y()) < 28 && x > 10 && x < rd_vw() - 10; }
static void rd_scrub_to(float x) {
    if (rd_n < 2) return;
    float f = clampf((x - RD_BAR_X) / (float)(rd_vw() - 2 * RD_BAR_X), 0, 1);
    int p = (int)(f * (rd_n - 1) + 0.5f);
    if (p != rd_page) rd_goto(p, 1);
}

static void rd_touch_event(SDL_Event *ev) {
    if (ev->type != SDL_FINGERDOWN && ev->type != SDL_FINGERMOTION && ev->type != SDL_FINGERUP) return;
    if (ev->tfinger.touchId != touch_front_id()) return;
    float sx = ev->tfinger.x * SCREEN_W, sy = ev->tfinger.y * SCREEN_H, x, y;
    if (rd_portrait) { x = RD_PW - sy; y = sx; } else { x = sx; y = sy; }
    if (ev->type == SDL_FINGERDOWN) {
        if (rd_fd) return;   /* one finger only */
        rd_fd = 1; rd_fid = ev->tfinger.fingerId; rd_fmoved = 0; rd_ft0 = SDL_GetTicks();
        rd_fx0 = rd_flx = x; rd_fy0 = rd_fly = y;
        rd_scrub = rd_ui && !rd_ask && !rd_end && rd_on_bar(x, y);
        if (rd_scrub) rd_scrub_to(x);
        return;
    }
    if (!rd_fd || ev->tfinger.fingerId != rd_fid) return;
    if (ev->type == SDL_FINGERMOTION) {
        if (fabsf(x - rd_fx0) > 14 || fabsf(y - rd_fy0) > 14) rd_fmoved = 1;
        if (rd_scrub) rd_scrub_to(x);
        else if (rd_fmoved) { rd_dx += x - rd_flx; rd_dy += y - rd_fly; }
        rd_flx = x; rd_fly = y;
        return;
    }
    rd_fd = 0;   /* finger up */
    if (rd_scrub) { rd_scrub = 0; return; }
    float mx = x - rd_fx0, my = y - rd_fy0;
    if (!rd_fmoved && SDL_GetTicks() - rd_ft0 < 500) { rd_tap = 1; rd_tx = x; rd_tyy = y; }
    else if (fabsf(mx) > 90 && fabsf(mx) > fabsf(my) * 1.5f) rd_swipe = mx < 0 ? 1 : -1;
}

static void rd_stick(int rx, int ry, float *sx, float *sy) {
    float x = (rx - 128) / 127.0f, y = (ry - 128) / 127.0f, m = sqrtf(x * x + y * y);
    if (m < 0.25f) return;
    float s = (m - 0.25f) / 0.75f;
    if (s > 1) s = 1;
    s *= s;
    x = x / m * s; y = y / m * s;
    if (rd_portrait) { float t = x; x = -y; y = t; }
    *sx += x; *sy += y;
}

/* ----- update ----- */
static void update_reader(float dt) {
    rd_want();
    rd_upload();
    RdSlot *cur = rd_cur();
    int vw = rd_vw(), vh = rd_vh();
    rd_load_t = cur ? 0 : rd_load_t + dt;
    if (cur && rd_snap) {
        rd_px = 0;
        rd_py = rd_ty = rd_snap == 2 ? 1e9f : 0;
        rd_snap = 0;
        rd_clamp(cur);
    }

    /* keep the Vita awake while reading (up to 3 minutes after the last input) */
    if (btn != btn_old || rd_fd) rd_idle = 0; else rd_idle += dt;
    rd_tick_t -= dt;
    if (rd_idle < 180 && rd_tick_t <= 0) { sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); rd_tick_t = 5; }
    rd_save_t += dt;
    if (rd_save_t > 20) { rd_save_t = 0; prog_save(); }

    /* portrait turns the D-pad with the picture */
    unsigned U = SCE_CTRL_UP, D = SCE_CTRL_DOWN, L = SCE_CTRL_LEFT, R = SCE_CTRL_RIGHT;
    if (rd_portrait) { U = SCE_CTRL_LEFT; D = SCE_CTRL_RIGHT; L = SCE_CTRL_DOWN; R = SCE_CTRL_UP; }
    int up = nav_rep(0, (btn & U) != 0, dt), down = nav_rep(1, (btn & D) != 0, dt);
    int left = nav_rep(2, (btn & L) != 0, dt), right = nav_rep(3, (btn & R) != 0, dt);
    int tap = rd_tap, swipe = rd_swipe;
    float tx = rd_tx, ty = rd_tyy, ddx = rd_dx, ddy = rd_dy;
    rd_tap = rd_swipe = 0; rd_dx = rd_dy = 0;

    if (rd_ask || rd_end) {
        int n = rd_box_n(), pick = -1;
        if (left && rd_btn > 0) rd_btn--;
        if (right && rd_btn < n - 1) rd_btn++;
        if (pressed(SCE_CTRL_CIRCLE)) { rd_close(); return; }
        if (pressed(SCE_CTRL_CROSS)) pick = rd_btn;
        for (int i = 0; tap && i < n; i++) {
            int x, y, w, h;
            rd_box_btn(i, &x, &y, &w, &h);
            if (tx >= x && tx < x + w && ty >= y && ty < y + h) pick = i;
        }
        if (pick >= 0) rd_box_pick(pick);
        return;
    }

    if (pressed(SCE_CTRL_CIRCLE)) { rd_close(); return; }
    if (pressed(SCE_CTRL_CROSS)) rd_ui = !rd_ui;
    if (pressed(SCE_CTRL_TRIANGLE)) rd_set_zoom(rd_zoom > 1.5f ? 1.0f : 2.0f, cur);
    if (pressed(SCE_CTRL_SQUARE) && !rd_webtoon) {
        cfg_fit = !cfg_fit;
        rd_px = rd_py = rd_ty = 0;
        toast(cfg_fit ? "Page fit: width" : "Page fit: screen");
    }
    if (tap) {
        if (rd_ui && tx < 80 && ty < 80) { rd_close(); return; }
        if (tx < vw * 0.3f) { rd_prev(1); return; }
        if (tx > vw * 0.7f) { rd_next(); return; }
        rd_ui = !rd_ui;
    }
    if (pressed(SCE_CTRL_RTRIGGER) || right) { rd_next(); return; }
    if (pressed(SCE_CTRL_LTRIGGER) || left) { rd_prev(1); return; }
    if (!cur) {
        if (pressed(D)) rd_next();
        else if (pressed(U)) rd_prev(2);
        return;
    }

    float k = rd_scale(cur), dw = cur->w * k, dh = cur->h * k, maxy = dh > vh ? dh - vh : 0;
    if (swipe && dw <= vw + 1) { if (swipe > 0) rd_next(); else rd_prev(1); return; }
    if (down) {   /* scroll down; at the bottom a fresh press turns the page */
        if (rd_ty >= maxy - 2) { if (pressed(D)) { rd_next(); return; } }
        else rd_ty = fminf(rd_ty + vh * 0.8f, maxy);
    }
    if (up) {
        if (rd_ty <= 2) { if (pressed(U)) { rd_prev(2); return; } }
        else rd_ty = fmaxf(rd_ty - vh * 0.8f, 0);
    }

    float sx = 0, sy = 0;
    rd_stick(pad_lx, pad_ly, &sx, &sy);
    rd_stick(pad_rx, pad_ry, &sx, &sy);
    if (sx != 0 || sy != 0 || ddx != 0 || ddy != 0) {
        rd_px += sx * 1100 * dt - ddx;
        rd_py += sy * 1100 * dt - ddy;
        rd_ty = rd_py;
        rd_idle = 0;
    }
    rd_py += (rd_ty - rd_py) * lerpk(dt);
    rd_clamp(cur);
}

/* ----- draw ----- */
static void rd_hint(int *x, int cy, int kind, const char *label) {
    btn_icon(kind, *x + 8, cy);
    int w = textc(F_SMALL, *x + 22, cy, AL_L, label, C_SOFT, 255);
    *x += 22 + w + 20;
}

static void rd_draw_box(int vw, int vh) {
    fill(0, 0, vw, vh, 0, 0, 0, 150);
    int bx, by, bw, bh, n = rd_box_n();
    rd_box_geo(&bx, &by, &bw, &bh);
    rrect(bx, by, bw, bh, 20, C_PANEL, 250);
    rrect_line(bx, by, bw, bh, 20, 1, C_LINE, 255);
    text(F_SUB, bx + 24, by + 24, AL_L, rd_b1, C_TEXT, 255);
    text(F_BODY, bx + 24, by + 62, AL_L, rd_b2, C_SOFT, 255);
    for (int i = 0; i < n; i++) {
        int x, y, w, h, ic;
        const char *lab;
        rd_box_btn(i, &x, &y, &w, &h);
        if (rd_ask) { lab = i == 0 ? "Resume" : "Start over"; ic = i == 0 ? IC_PLAY : IC_REFRESH; }
        else if (n == 2) { lab = i == 0 ? "Next chapter" : "Back"; ic = i == 0 ? IC_RIGHT : IC_LEFT; }
        else { lab = "Back"; ic = IC_LEFT; }
        button(x, y, w, h, i == 0, ic, lab, rd_btn == i, 255);
    }
}

static void rd_draw_toast(int vw, int vh) {
    if (toast_t <= 0) return;
    int a = toast_t < 0.3f ? (int)(toast_t / 0.3f * 255) : 255;
    int w = text_w(F_BODYB, toast_msg) + 56, x = (vw - w) / 2, y = rd_ui ? vh - 196 : vh - 96;
    rrect(x, y, w, 44, 22, C_PANEL2, a * 245 / 255);
    rrect_line(x, y, w, 44, 22, 1, C_LINE, a);
    disc(x + 24, y + 22, 5, C_ACCENT, a);
    textc(F_BODYB, x + 38, y + 22, AL_L, toast_msg, C_TEXT, a);
}

static void draw_reader(void) {
    int port = rd_portrait && rd_rt, vw = rd_vw(), vh = rd_vh();
    if (port) SDL_SetRenderTarget(ren, rd_rt);
    fill(0, 0, vw, vh, 8, 8, 10, 255);

    RdSlot *cur = rd_cur();
    if (cur) {
        float k = rd_scale(cur), dw = cur->w * k, dh = cur->h * k;
        int ox = (int)(dw < vw ? (vw - dw) / 2 : -rd_px);
        float oy = dh < vh ? (vh - dh) / 2 : -rd_py;
        int ty = 0;
        for (int i = 0; i < cur->nt; i++) {
            int y0 = (int)(oy + ty * k), y1 = (int)(oy + (ty + cur->th[i]) * k);
            ty += cur->th[i];
            if (y1 < 0 || y0 > vh) continue;
            SDL_Rect d = { ox, y0, (int)(dw + 0.5f), y1 - y0 };
            SDL_RenderCopy(ren, cur->t[i], NULL, &d);
        }
    } else {
        RdSlot *s = rd_slot_of(rd_page);
        if (s && s->state == RS_FAIL) {
            textc(F_SUB, vw / 2, vh / 2 - 14, AL_C, "Couldn't open this page", C_SOFT, 255);
            if (rd_page < rd_n) textc(F_SMALL, vw / 2, vh / 2 + 18, AL_C, rd_files[rd_page], C_DIM, 255);
        } else if (rd_load_t > 0.25f) {
            dango_icon(vw / 2, vh / 2 - 20, 1.6f);
            textc(F_SUB, vw / 2, vh / 2 + 34, AL_C, "Loading page...", C_SOFT, 255);
        }
    }

    char pg[32];
    if (rd_ui && !rd_ask && !rd_end) {
        grad_v(0, 0, vw, 100, 0, 0, 0, 230, 0);
        disc(38, 38, 22, C_PANEL, 230);
        gc(C_TEXT, 255); icon(IC_LEFT, 37, 38, 0.85f, C_PANEL);
        text(F_BODYB, 74, 16, AL_L, rd_title, C_TEXT, 255);
        text(F_SMALL, 74, 42, AL_L, rd_chlab, C_SOFT, 255);
        grad_v(0, vh - 140, vw, 140, 0, 0, 0, 0, 235);
        snprintf(pg, sizeof pg, "Page %d / %d", rd_page + 1, rd_n);
        textc(F_BODYB, vw / 2, vh - 98, AL_C, pg, C_TEXT, 255);
        int bw = vw - 2 * RD_BAR_X, by = rd_bar_y();
        float f = rd_n > 1 ? (float)rd_page / (rd_n - 1) : 1;
        rrect(RD_BAR_X, by - 3, bw, 6, 3, 255, 255, 255, 50);
        if (bw * f >= 6) rrect(RD_BAR_X, by - 3, (int)(bw * f), 6, 3, C_ACCENT, 255);
        disc(RD_BAR_X + bw * f, by, 9, C_TEXT, 255);
        int hx = 20, hy = vh - 24;
        rd_hint(&hx, hy, BI_DLR, "Page");
        rd_hint(&hx, hy, BI_TRI, rd_zoom > 1.5f ? "Zoom out" : "Zoom");
        if (!rd_webtoon) rd_hint(&hx, hy, BI_SQ, cfg_fit ? "Fit screen" : "Fit width");
        rd_hint(&hx, hy, BI_X, "Hide");
        rd_hint(&hx, hy, BI_O, "Back");
    } else if (!rd_ask && !rd_end) {
        snprintf(pg, sizeof pg, "%d / %d", rd_page + 1, rd_n);
        int w = text_w(F_SMALLB, pg) + 20;
        rrect(vw - w - 12, vh - 38, w, 26, 13, 0, 0, 0, 150);
        textc(F_SMALLB, vw - 12 - w / 2, vh - 25, AL_C, pg, C_SOFT, 220);
    }
    if (rd_ask || rd_end) rd_draw_box(vw, vh);
    rd_draw_toast(vw, vh);

    if (port) {
        SDL_SetRenderTarget(ren, NULL);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_Rect d = { (SCREEN_W - RD_PW) / 2, (SCREEN_H - RD_PH) / 2, RD_PW, RD_PH };
        SDL_RenderCopyEx(ren, rd_rt, NULL, &d, 270, NULL, SDL_FLIP_NONE);
    }
}

static void rd_init(void) {
    rd_files = malloc(sizeof(*rd_files) * RD_PAGES);
    rd_mx = SDL_CreateMutex();
    rd_sem = SDL_CreateSemaphore(0);
    for (int i = 0; i < RD_SLOTS; i++) rd_slot[i].pg = -1;
    if (rd_files && rd_mx && rd_sem)
        rd_thr = SDL_CreateThreadWithStackSize(rd_thread, "rd_load", 256 * 1024, NULL);
}

/* ===== Novel reader (R-2a): EPUB + TXT, text drawn to page textures ===== */
#define NV_SIZES  6
#define NV_WT     0x2100        /* glyph widths are cached for code points below this */
#define NV_PROG   10000         /* novel progress = a fraction of 10000 (survives text size changes) */
#define NV_TEX    3             /* page textures kept: current, next, previous */
#define NV_THEMES 4
static const int nv_sizes[NV_SIZES] = { 20, 22, 24, 26, 30, 34 };
static const int nv_lines[3] = { 115, 130, 150 };                              /* line height, % of the font's */
static const int nv_mland[3] = { 44, 76, 116 }, nv_mport[3] = { 18, 30, 46 };  /* side margins */
static const char *const nv_line_names[3] = { "Tight", "Normal", "Wide" };
static const char *const nv_margin_names[3] = { "Narrow", "Normal", "Wide" };
typedef struct { const char *name; int bg[3], fg[3], dim[3]; } NvTheme;
static const NvTheme nv_themes[NV_THEMES] = {
    { "Black",     { 0, 0, 0 },       { 232, 232, 236 }, { 110, 110, 122 } },
    { "Dark Grey", { 30, 30, 34 },    { 222, 222, 228 }, { 120, 120, 132 } },
    { "Sepia",     { 244, 236, 216 }, { 64, 50, 36 },    { 150, 132, 108 } },
    { "White",     { 250, 250, 250 }, { 24, 24, 28 },    { 140, 140, 146 } },
};
static const NvTheme *nv_th(void) { return &nv_themes[cfg_nvtheme >= 0 && cfg_nvtheme < NV_THEMES ? cfg_nvtheme : 0]; }
static int nv_light(void) { return nv_th()->bg[0] > 128; }
#define NV_BG  nv_th()->bg[0], nv_th()->bg[1], nv_th()->bg[2]
#define NV_FG  nv_th()->fg[0], nv_th()->fg[1], nv_th()->fg[2]
#define NV_DIM nv_th()->dim[0], nv_th()->dim[1], nv_th()->dim[2]
#define LIT_REG  "app0:literata.ttf"
#define LIT_BOLD "app0:literata_bold.ttf"
#define LIT_ITAL "app0:literata_italic.ttf"
static int nvm_on, nvm_sel;   /* reader menu (R-2b-2) */

enum { NF_REG, NF_BOLD, NF_ITAL, NF_HEAD, NF_COUNT };
static TTF_Font *nvf[NF_COUNT];
static int nvf_px;
static unsigned short nv_wt[NF_COUNT][NV_WT];   /* width + 1, 0 = not measured yet */

static NvBook nv, nv_nb;
static int in_novel, nv_ti, nv_vol, nv_sec = -1, nv_page, nv_ui, nv_ask, nv_end, nv_btn, nv_jump, nv_st_old;
static int *nv_pg, nv_npg, nv_pgcap, nv_resume_off;   /* page start offsets in the current section */
static int nv_lh, nv_mx, nv_top, nv_bot;
static float nv_idle, nv_save_t, nv_tick_t, nv_scrub_f;
static char nv_title[160], nv_sub[160], nv_b1[160], nv_b2[200];
typedef struct { int sec, page; unsigned used; SDL_Texture *t; } NvTex;
static NvTex nv_tex[NV_TEX];
static int nv_fd, nv_fmoved, nv_scrub, nv_tap, nv_swipe;
static SDL_FingerID nv_fid;
static Uint32 nv_ft0;
static float nv_fx0, nv_fy0, nv_tx, nv_ty;

/* ----- fonts and measuring ----- */
static int nvf_face = -1, nv_nolit;   /* face loaded (0 Outfit, 1 Literata), Literata not in the VPK */
static void nv_fonts_close(void) {
    for (int i = 0; i < NF_COUNT; i++) { if (nvf[i]) TTF_CloseFont(nvf[i]); nvf[i] = NULL; }
    nvf_px = 0; nvf_face = -1;
}
static int nv_fonts(void) {
    if (cfg_nvsize < 0 || cfg_nvsize >= NV_SIZES) cfg_nvsize = 2;
    int px = nv_sizes[cfg_nvsize];
    nv_nolit = !file_exists(LIT_REG);
    int face = cfg_nvfont && !nv_nolit;
    if (nvf[NF_REG] && nvf_px == px && nvf_face == face) return 1;
    nv_fonts_close();
    memset(nv_wt, 0, sizeof nv_wt);
    const char *reg = face ? LIT_REG : FONT_REG;
    const char *bold = face ? (file_exists(LIT_BOLD) ? LIT_BOLD : LIT_REG) : (file_exists(FONT_BOLD) ? FONT_BOLD : FONT_REG);
    const char *ital = face && file_exists(LIT_ITAL) ? LIT_ITAL : NULL;   /* NULL = slanted regular */
    nvf[NF_REG]  = TTF_OpenFont(reg, px);
    nvf[NF_BOLD] = TTF_OpenFont(bold, px);
    nvf[NF_ITAL] = TTF_OpenFont(ital ? ital : reg, px);
    nvf[NF_HEAD] = TTF_OpenFont(bold, px * 5 / 4);
    for (int i = 0; i < NF_COUNT; i++) if (!nvf[i]) { nv_fonts_close(); return 0; }
    if (!ital) TTF_SetFontStyle(nvf[NF_ITAL], TTF_STYLE_ITALIC);
    for (int i = 0; i < NF_COUNT; i++) TTF_SetFontKerning(nvf[i], 0);   /* so measured widths match drawing */
    nvf_px = px; nvf_face = face;
    return 1;
}
static void nv_metrics(void) {
    nv_lh = TTF_FontLineSkip(nvf[NF_REG]) * nv_lines[cfg_nvline] / 100;
    nv_mx = rd_portrait ? nv_mport[cfg_nvmargin] : nv_mland[cfg_nvmargin];
    nv_top = rd_portrait ? 44 : 30;
    nv_bot = rd_portrait ? 60 : 44;
}

static int nv_font_of(int b, int it, int head) { return head ? NF_HEAD : b ? NF_BOLD : it ? NF_ITAL : NF_REG; }
static int nv_cw(int f, unsigned cp) {
    if (cp < NV_WT && nv_wt[f][cp]) return nv_wt[f][cp] - 1;
    int adv = 0;
#if SDL_TTF_MAJOR_VERSION > 2 || (SDL_TTF_MAJOR_VERSION == 2 && (SDL_TTF_MINOR_VERSION > 0 || SDL_TTF_PATCHLEVEL >= 18))
    if (TTF_GlyphMetrics32(nvf[f], cp, NULL, NULL, NULL, NULL, &adv) < 0) adv = 0;
#else
    if (cp > 0xFFFF || TTF_GlyphMetrics(nvf[f], (Uint16)cp, NULL, NULL, NULL, NULL, &adv) < 0) adv = 0;
#endif
    if (cp < NV_WT) nv_wt[f][cp] = (unsigned short)(adv + 1);
    return adv;
}
static unsigned nv_u8(const char *s, int *n) {
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) { *n = 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { *n = 2; return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *n = 3; return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *n = 4; return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    *n = 1;
    return '?';
}

/* ----- layout ----- */
static void nv_style_at(int pos, int *b, int *it, int *head, int *cen) {   /* style in the middle of a paragraph */
    const char *t = nv.text;
    int ps = pos;
    while (ps > 0 && t[ps - 1] != '\n') ps--;
    *head = t[ps] == NV_HEAD; *cen = t[ps] == NV_CENTER;
    *b = *it = 0;
    for (int k = ps; k < pos; k++) {
        if (t[k] == NV_B_ON) *b = 1; else if (t[k] == NV_B_OFF) *b = 0;
        else if (t[k] == NV_I_ON) *it = 1; else if (t[k] == NV_I_OFF) *it = 0;
    }
}

/* one line from lp: returns where the next line starts, *tend = end of the drawn text, *lw = width */
static int nv_line(int lp, int end, int head, int *b, int *it, int maxw, int *tend, int *lw) {
    const char *t = nv.text;
    int i = lp, w = 0, words = 0, cb = *b, ci = *it, spw = nv_cw(head ? NF_HEAD : NF_REG, ' ');
    *tend = lp;
    while (i < end && t[i] != '\n') {
        int j = i, ww = 0, sb = cb, si = ci, cut = -1, cutw = 0, csb = 0, csi = 0;
        while (j < end && t[j] != ' ' && t[j] != '\n') {
            char c = t[j];
            if (c >= 1 && c <= 6) {
                if (c == NV_B_ON) sb = 1; else if (c == NV_B_OFF) sb = 0;
                else if (c == NV_I_ON) si = 1; else if (c == NV_I_OFF) si = 0;
                j++;
                continue;
            }
            int n;
            unsigned cp = nv_u8(t + j, &n);
            int cw = nv_cw(nv_font_of(sb, si, head), cp);
            if (cut < 0 && ww > 0 && ww + cw > maxw) { cut = j; cutw = ww; csb = sb; csi = si; }
            ww += cw;
            j += n;
        }
        if (words && w + spw + ww > maxw) break;   /* this word starts the next line */
        if (!words && cut >= 0) {                  /* one word wider than the line: split it */
            *tend = cut; w = cutw; cb = csb; ci = csi; i = cut;
            break;
        }
        w += (words ? spw : 0) + ww;
        words++;
        cb = sb; ci = si; i = j; *tend = j;
        if (i < end && t[i] == ' ') i++;
    }
    *b = cb; *it = ci; *lw = w;
    return i;
}

static void nv_run(SDL_Surface *dst, int f, char *run, int *rn, int *rx, int *rw, int y, int lh) {
    if (*rn > 0) {
        run[*rn] = 0;
        SDL_Color fg = { NV_FG, 255 };
        SDL_Surface *ts = TTF_RenderUTF8_Blended(nvf[f], run, fg);
        if (ts) {
            SDL_Rect r = { *rx, y + (lh - TTF_FontHeight(nvf[f])) / 2, ts->w, ts->h };
            SDL_BlitSurface(ts, NULL, dst, &r);
            SDL_FreeSurface(ts);
        }
    }
    *rx += *rw; *rn = 0; *rw = 0;
}

static void nv_draw_line(SDL_Surface *dst, int a, int e, int head, int x, int y, int lh, int b, int it, float ex) {
    const char *t = nv.text;
    char run[512];
    int rn = 0, rw = 0, rx = x, spw = nv_cw(head ? NF_HEAD : NF_REG, ' ');
    float acc = 0;
    for (int k = a; k < e; ) {
        char c = t[k];
        if (c >= 1 && c <= 6) {
            if (c <= 4) {
                nv_run(dst, nv_font_of(b, it, head), run, &rn, &rx, &rw, y, lh);
                if (c == NV_B_ON) b = 1; else if (c == NV_B_OFF) b = 0; else if (c == NV_I_ON) it = 1; else it = 0;
            }
            k++;
            continue;
        }
        if (c == ' ' && ex > 0) {   /* justified: each space gets a bit wider */
            nv_run(dst, nv_font_of(b, it, head), run, &rn, &rx, &rw, y, lh);
            acc += ex;
            int add = (int)acc;
            acc -= add;
            rx += spw + add;
            k++;
            continue;
        }
        int n;
        unsigned cp = nv_u8(t + k, &n);
        if (rn + n >= (int)sizeof run - 1) nv_run(dst, nv_font_of(b, it, head), run, &rn, &rx, &rw, y, lh);
        memcpy(run + rn, t + k, n);
        rn += n;
        rw += c == ' ' ? spw : nv_cw(nv_font_of(b, it, head), cp);
        k += n;
    }
    nv_run(dst, nv_font_of(b, it, head), run, &rn, &rx, &rw, y, lh);
}

/* ----- pictures (R-2b) ----- */
static SDL_Surface *nv_img_surface(int idx) {   /* decode picture idx from the EPUB, see-through parts made white */
    int len = 0;
    unsigned char *d = nv_img_data(&nv, idx, &len);
    if (!d) return NULL;
    SDL_RWops *rw = SDL_RWFromConstMem(d, len);
    SDL_Surface *s = rw ? IMG_Load_RW(rw, 1) : NULL;
    free(d);
    if (!s) return NULL;
    int alpha = s->format->Amask != 0 || SDL_HasColorKey(s);
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(s);
    if (!c || !alpha) return c;
    SDL_Surface *o = SDL_CreateRGBSurfaceWithFormat(0, c->w, c->h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!o) return c;
    if (nv_light()) SDL_FillRect(o, NULL, SDL_MapRGBA(o->format, NV_BG, 255));   /* light themes: the page colour */
    else SDL_FillRect(o, NULL, SDL_MapRGBA(o->format, 255, 255, 255, 255));      /* dark themes: white, so line art stays visible */
    SDL_SetSurfaceBlendMode(c, SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(c, NULL, o, NULL);
    SDL_FreeSurface(c);
    return o;
}

static void nv_draw_img(SDL_Surface *dst, int idx) {   /* the picture, as big as fits, centred */
    int bx = 12, by = 12, bw = dst->w - 24, bh = dst->h - nv_bot - 12;
    SDL_Surface *s = nv_img_surface(idx);
    if (!s) {
        SDL_Color fg = { 150, 150, 160, 255 };
        SDL_Surface *ts = TTF_RenderUTF8_Blended(nvf[NF_ITAL], "This picture couldn't be loaded", fg);
        if (ts) {
            SDL_Rect r = { (dst->w - ts->w) / 2, (dst->h - ts->h) / 2, ts->w, ts->h };
            SDL_BlitSurface(ts, NULL, dst, &r);
            SDL_FreeSurface(ts);
        }
        return;
    }
    float k = fminf((float)bw / s->w, (float)bh / s->h);
    if (k > 2) k = 2;
    int w = (int)(s->w * k), h = (int)(s->h * k);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    SDL_Rect r = { bx + (bw - w) / 2, by + (bh - h) / 2, w, h };
    SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);
    if (SDL_SoftStretchLinear(s, NULL, dst, &r) < 0) SDL_BlitScaled(s, NULL, dst, &r);
    SDL_FreeSurface(s);
}

/* lays out one page from pos (draws it when dst != NULL); returns where the next page starts */
static int nv_layout(int pos, int end, SDL_Surface *dst) {
    const char *t = nv.text;
    int vw = rd_vw(), vh = rd_vh(), maxw = vw - 2 * nv_mx, ybot = vh - nv_bot, y = nv_top, first = 1;
    int b = 0, it = 0, head = 0, cen = 0, pstart = pos == 0 || t[pos - 1] == '\n';
    if (!pstart) nv_style_at(pos, &b, &it, &head, &cen);
    while (pos < end) {
        if (t[pos] == '\n') { pos++; pstart = 1; continue; }
        if (pstart && t[pos] == NV_IMG) {   /* a picture gets a page of its own */
            if (!first) break;
            int e2 = pos;
            while (e2 < end && t[e2] != '\n') e2++;
            if (dst) nv_draw_img(dst, atoi(t + pos + 1));
            return e2 < end ? e2 + 1 : end;
        }
        int lp = pos, lb = b, li = it, lhd = head, lcen = cen, gap = 0;
        if (pstart) {
            lhd = t[lp] == NV_HEAD; lcen = t[lp] == NV_CENTER;
            if (lhd || lcen) lp++;
            lb = li = 0;
            if (!first) gap = lhd ? nv_lh : nv_lh / 2;
        }
        int lh = lhd ? nv_lh * 5 / 4 : nv_lh;
        if (!first && y + gap + lh > ybot) break;
        y += gap;
        int sb = lb, si = li, tend, lw;
        int np = nv_line(lp, end, lhd, &lb, &li, maxw, &tend, &lw);
        if (dst) {
            int x = nv_mx + ((lhd || lcen) && lw < maxw ? (maxw - lw) / 2 : 0);
            float ex = 0;
            if (cfg_nvjust && !lhd && !lcen && np < end && t[np] != '\n' && lw < maxw) {   /* not the paragraph's last line */
                int nsp = 0;
                for (int k = lp; k < tend; k++) if (t[k] == ' ') nsp++;
                if (nsp > 0) ex = (float)(maxw - lw) / nsp;
                if (ex > nv_cw(NF_REG, ' ') * 4) ex = 0;   /* huge gaps look worse than a ragged line */
            }
            nv_draw_line(dst, lp, tend, lhd, x, y, lh, sb, si, ex);
        }
        y += lh;
        first = 0;
        b = lb; it = li; head = lhd; cen = lcen; pstart = 0;
        pos = np > lp ? np : lp + 1;
        if (pos < end && t[pos] == '\n') { pos++; pstart = 1; }
    }
    return pos;
}

/* ----- page textures ----- */
static void nv_tex_clear(void) {
    for (int i = 0; i < NV_TEX; i++) {
        if (nv_tex[i].t) SDL_DestroyTexture(nv_tex[i].t);
        nv_tex[i].t = NULL; nv_tex[i].sec = -1; nv_tex[i].used = 0;
    }
}
static NvTex *nv_tex_find(int page) {
    for (int i = 0; i < NV_TEX; i++)
        if (nv_tex[i].t && nv_tex[i].sec == nv_sec && nv_tex[i].page == page) return &nv_tex[i];
    return NULL;
}
static SDL_Texture *nv_page_tex(int page) {
    if (page < 0 || page >= nv_npg) return NULL;
    NvTex *e = nv_tex_find(page);
    if (e) { e->used = frame_no; return e->t; }
    int lru = 0;
    for (int i = 0; i < NV_TEX; i++) {
        if (!nv_tex[i].t) { lru = i; break; }
        if (nv_tex[i].used < nv_tex[lru].used) lru = i;
    }
    e = &nv_tex[lru];
    if (e->t) SDL_DestroyTexture(e->t);
    e->t = NULL; e->sec = -1;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, rd_vw(), rd_vh(), 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return NULL;
    SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, NV_BG, 255));
    nv_layout(nv_pg[page], nv.ch[nv_sec].end, s);
    e->t = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
    if (!e->t) return NULL;
    e->sec = nv_sec; e->page = page; e->used = frame_no;
    return e->t;
}
static void nv_prefetch(void) {   /* draw the next (or previous) page while you read */
    if (!nv_tex_find(nv_page)) return;
    if (nv_page + 1 < nv_npg && !nv_tex_find(nv_page + 1)) nv_page_tex(nv_page + 1);
    else if (nv_page > 0 && !nv_tex_find(nv_page - 1)) nv_page_tex(nv_page - 1);
}

/* ----- sections, pages, progress ----- */
static void nv_paginate(int s) {
    NvChap *c = &nv.ch[s];
    int pos = c->off;
    nv_sec = s; nv_npg = 0;
    do {
        if (nv_npg == nv_pgcap) {
            int nc = nv_pgcap ? nv_pgcap * 2 : 256;
            int *np = realloc(nv_pg, nc * sizeof(int));
            if (!np) break;
            nv_pg = np; nv_pgcap = nc;
        }
        nv_pg[nv_npg++] = pos;
        int next = nv_layout(pos, c->end, NULL);
        pos = next > pos ? next : pos + 1;
    } while (pos < c->end);
    const char *st = c->title[0] ? c->title : titles[nv_ti].chs[nv_vol].label;
    fit_text(F_SMALL, st, rd_vw() - 200, nv_sub, sizeof nv_sub);
}
static int nv_find_page(int off) {
    int lo = 0, hi = nv_npg - 1;
    while (lo < hi) { int m = (lo + hi + 1) / 2; if (nv_pg[m] <= off) lo = m; else hi = m - 1; }
    return lo > 0 ? lo : 0;
}
static void nv_goto_off(int off) {
    if (off >= nv.len) off = nv.len - 1;
    if (off < 0) off = 0;
    int s = 0;
    while (s + 1 < nv.nch && nv.ch[s + 1].off <= off) s++;
    if (s != nv_sec || nv_npg == 0) nv_paginate(s);
    nv_page = nv_find_page(off);
}
static void nv_save_pos(void) {
    if (nv_npg <= 0 || nv.len <= 0) return;
    int last = nv_sec == nv.nch - 1 && nv_page == nv_npg - 1;
    int f = (int)((long long)nv_pg[nv_page] * NV_PROG / nv.len);
    if (last) f = NV_PROG - 1;
    else if (f > NV_PROG - 2) f = NV_PROG - 2;
    prog_set(titles[nv_ti].chs[nv_vol].path, f, NV_PROG);
}
static float nv_frac(void) {
    if (nv.len <= 0 || nv_npg <= 0) return 0;
    if (nv_sec == nv.nch - 1 && nv_page == nv_npg - 1) return 1;
    return (float)nv_pg[nv_page] / (float)nv.len;
}

/* ----- contents (R-2b) ----- */
#define NVC_Y    86
#define NVC_ROW  52
static int nvc_on, nvc_n, nvc_sel, nvc_cur, nvc_ready;
static int *nvc_off;
static char (*nvc_lab)[100];
static float nvc_scroll, nvc_target;

static void rd_hint_key(int *x, int cy, const char *key, const char *label) {
    int kw = text_w(F_SMALLB, key) + 14;
    rrect_line(*x, cy - 10, kw, 20, 6, 1, C_SOFT, 220);
    textc(F_SMALLB, *x + kw / 2, cy, AL_C, key, C_SOFT, 255);
    int w = textc(F_SMALL, *x + kw + 8, cy, AL_L, label, C_SOFT, 255);
    *x += kw + 8 + w + 20;
}

static void nvc_free(void) {
    free(nvc_off); free(nvc_lab);
    nvc_off = NULL; nvc_lab = NULL;
    nvc_n = 0; nvc_ready = 0; nvc_on = 0;
}

static void nvc_build(void) {   /* the book's own Contents, else its titled sections */
    int titled = 0, w = rd_vw() - 120, max = nv.ntoc > 0 ? nv.ntoc : nv.nch;
    nvc_free();
    nvc_ready = 1;
    if (max <= 0) return;
    nvc_off = malloc(max * sizeof(int));
    nvc_lab = malloc(max * sizeof *nvc_lab);
    if (!nvc_off || !nvc_lab) { nvc_free(); nvc_ready = 1; return; }
    if (nv.ntoc == 0) for (int i = 0; i < nv.nch; i++) if (nv.ch[i].title[0]) titled++;
    for (int i = 0; i < max; i++) {
        char t[120];
        if (nv.ntoc > 0) {
            nvc_off[nvc_n] = nv.toc[i].off;
            snprintf(t, sizeof t, "%s", nv.toc[i].title[0] ? nv.toc[i].title : "Untitled");
        } else {
            if (titled && !nv.ch[i].title[0]) continue;
            nvc_off[nvc_n] = nv.ch[i].off;
            if (nv.ch[i].title[0]) snprintf(t, sizeof t, "%s", nv.ch[i].title);
            else snprintf(t, sizeof t, "Section %d", i + 1);
        }
        fit_text(F_BODY, t, w, nvc_lab[nvc_n], sizeof nvc_lab[0]);
        nvc_n++;
    }
}

static void nvc_retarget(void) {
    float view = (float)(rd_vh() - 50 - NVC_Y), top = (float)nvc_sel * NVC_ROW, maxs = nvc_n * NVC_ROW - view;
    if (top < nvc_target) nvc_target = top;
    if (top + NVC_ROW > nvc_target + view) nvc_target = top + NVC_ROW - view;
    nvc_target = clampf(nvc_target, 0, maxs > 0 ? maxs : 0);
}

static void nvc_open(void) {
    if (nv_npg <= 0) return;
    if (!nvc_ready) nvc_build();
    if (nvc_n == 0) { toast("This book has no contents list"); return; }
    int end = (nv_page + 1 < nv_npg ? nv_pg[nv_page + 1] : nv.ch[nv_sec].end) - 1, best = -1;
    nvc_cur = 0;
    for (int i = 0; i < nvc_n; i++)   /* the entry you're in = the last one that starts on or before this page */
        if (nvc_off[i] <= end && nvc_off[i] >= best) { best = nvc_off[i]; nvc_cur = i; }
    nvc_sel = nvc_cur; nvc_on = 1;
    nvc_target = 0; nvc_retarget(); nvc_scroll = nvc_target;
}

static void nvc_go(int i) {
    if (i < 0 || i >= nvc_n) return;
    nvc_on = 0;
    nv_goto_off(nvc_off[i]);
    nv_save_pos();
}

static void nvc_update(float dt, int tap, float tx, float ty, float dy) {
    unsigned U = SCE_CTRL_UP, D = SCE_CTRL_DOWN, L = SCE_CTRL_LEFT, R = SCE_CTRL_RIGHT;
    int vu = st_up, vd = st_down, vl = st_left, vr = st_right;
    if (rd_portrait) {
        U = SCE_CTRL_LEFT; D = SCE_CTRL_RIGHT; L = SCE_CTRL_DOWN; R = SCE_CTRL_UP;
        vu = st_left; vd = st_right; vl = st_down; vr = st_up;
    }
    int up = nav_rep(0, (btn & U) || vu, dt), down = nav_rep(1, (btn & D) || vd, dt);
    int left = nav_rep(2, (btn & L) || vl || (btn & SCE_CTRL_LTRIGGER), dt);
    int right = nav_rep(3, (btn & R) || vr || (btn & SCE_CTRL_RTRIGGER), dt);
    if (pressed(SCE_CTRL_CIRCLE) || pressed(SCE_CTRL_START)) { nvc_on = 0; return; }
    int moved = 0;
    if (up && nvc_sel > 0) { nvc_sel--; moved = 1; }
    if (down && nvc_sel < nvc_n - 1) { nvc_sel++; moved = 1; }
    if (left) { nvc_sel = nvc_sel > 10 ? nvc_sel - 10 : 0; moved = 1; }
    if (right) { nvc_sel = nvc_sel + 10 < nvc_n ? nvc_sel + 10 : nvc_n - 1; moved = 1; }
    if (pressed(SCE_CTRL_CROSS)) { nvc_go(nvc_sel); return; }
    if (tap) {
        if (tx < 80 && ty < 80) { nvc_on = 0; return; }
        if (ty >= NVC_Y && ty < rd_vh() - 50) {
            int i = (int)((ty - NVC_Y + nvc_scroll) / NVC_ROW);
            if (i >= 0 && i < nvc_n) { nvc_sel = i; nvc_go(i); return; }
        }
    }
    float view = (float)(rd_vh() - 50 - NVC_Y), maxs = nvc_n * NVC_ROW - view;
    if (maxs < 0) maxs = 0;
    if (dy != 0) { nvc_scroll = clampf(nvc_scroll - dy, 0, maxs); nvc_target = nvc_scroll; }
    else {
        if (moved) nvc_retarget();
        nvc_scroll += (nvc_target - nvc_scroll) * lerpk(dt);
    }
}

static void nvc_draw(int vw, int vh) {
    int y1 = vh - 50, rh = NVC_ROW - 8;
    fill(0, 0, vw, vh, C_BG, 255);
    int first = (int)(nvc_scroll / NVC_ROW);
    if (first < 0) first = 0;
    for (int i = first; i < nvc_n; i++) {
        int y = NVC_Y + i * NVC_ROW - (int)nvc_scroll;
        if (y > y1) break;
        int sel = i == nvc_sel;
        rrect(24, y, vw - 48, rh, 12, sel ? 42 : 28, sel ? 42 : 28, sel ? 56 : 36, 255);
        if (sel) rrect_line(24, y, vw - 48, rh, 12, 2, C_ACCENT, 255);
        if (i == nvc_cur) {
            disc(44, y + rh / 2, 5, C_ACCENT, 255);
            textc(F_BODY, 60, y + rh / 2, AL_L, nvc_lab[i], C_ACCLT, 255);
        } else textc(F_BODY, 60, y + rh / 2, AL_L, nvc_lab[i], C_TEXT, 255);
    }
    fill(0, 0, vw, NVC_Y - 8, C_BG, 255);      /* header and footer drawn over the list */
    disc(38, 38, 22, C_PANEL, 255);
    gc(C_TEXT, 255); icon(IC_LEFT, 37, 38, 0.85f, C_PANEL);
    text(F_H2, 74, 20, AL_L, "Contents", C_TEXT, 255);
    char cnt[32];
    snprintf(cnt, sizeof cnt, "%d / %d", nvc_sel + 1, nvc_n);
    textc(F_SMALLB, vw - 30, 38, AL_R, cnt, C_DIM, 255);
    fill(0, y1, vw, vh - y1, C_BG, 255);
    int hx = 20, hy = vh - 24;
    rd_hint(&hx, hy, BI_X, "Go");
    rd_hint(&hx, hy, BI_DLR, "Jump 10");
    rd_hint(&hx, hy, BI_O, "Back");
}

/* ----- cover from the EPUB (R-2b): saved once, when a book without a cover is opened ----- */
static void nv_save_cover(int ti, int vol) {
    Title *tl = &titles[ti];
    if (tl->cover_path[0] || !ends_with_ci(tl->chs[vol].path, ".epub")) return;
    const char *src = nv.cover[0] ? nv.cover : nv.nimg > 0 ? nv.img[0] : NULL;
    if (!src) return;
    int len = 0;
    unsigned char *d = nv_zip_get(nv.path, src, &len);
    if (!d) return;
    const char *ext = len > 3 && d[0] == 0xFF && d[1] == 0xD8 ? ".jpg"
                    : len > 4 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' ? ".png" : NULL;
    char out[512];
    FILE *f = NULL;
    if (ext) {
        if (!strcmp(tl->dir, NOVEL_ROOT)) snprintf(out, sizeof out, "%s/%s%s", NOVEL_ROOT, tl->chs[vol].file, ext);
        else {
            snprintf(out, sizeof out, "%s/meta", tl->dir);
            sceIoMkdir(out, 0777);
            snprintf(out, sizeof out, "%s/meta/cover%s", tl->dir, ext);
        }
        f = fopen(out, "wb");
    }
    int ok = f && fwrite(d, 1, len, f) == (size_t)len;
    if (f && fclose(f) != 0) ok = 0;
    free(d);
    if (!ok) { if (f) remove(out); return; }
    snprintf(tl->cover_path, sizeof tl->cover_path, "%s", out);
    if (!tl->banner_path[0]) snprintf(tl->banner_path, sizeof tl->banner_path, "%s", out);
    tl->cover_state = 0;   /* the library loads it next time it's drawn */
}

/* ----- opening and closing ----- */
static void nv_loading(void) {
    SDL_SetRenderTarget(ren, NULL);
    fill(0, 0, SCREEN_W, SCREEN_H, C_BG, 255);
    dango_icon(SCREEN_W / 2, 230, 2.0f);
    textc(F_SUB, SCREEN_W / 2, 300, AL_C, "Opening book...", C_SOFT, 255);
    SDL_RenderPresent(ren);
}

static int nv_load_vol(int ti, int vol, int start) {   /* start: 0 = beginning, RD_AUTO, RD_RESUME */
    Title *tl = &titles[ti];
    if (vol < 0 || vol >= tl->nchs) return 0;
    Chapter *c = &tl->chs[vol];
    if (ends_with_ci(c->path, ".pdf")) { toast("PDF isn't supported. Convert it to EPUB with Calibre"); return 0; }
    nv_loading();
    if (!nv_load(c->path, &nv_nb)) {
        toast("%s", nv_nb.err[0] ? nv_nb.err : "Couldn't open this book");
        nv_free(&nv_nb);
        return 0;
    }
    prog_save();
    nv_free(&nv);
    nv = nv_nb;
    memset(&nv_nb, 0, sizeof nv_nb);
    nvc_free();
    nv_ti = ti; nv_vol = vol; nv_sec = -1; nv_npg = 0;
    nv_ask = nv_end = 0; nv_btn = 0;
    nv_tex_clear();
    fit_text(F_BODYB, tl->name, rd_vw() - 200, nv_title, sizeof nv_title);
    nv_save_cover(ti, vol);
    int off = 0;
    Prog *p = prog_find(c->path);
    if (start != 0 && p && !p->done && p->pages == NV_PROG && p->page > 0) {
        off = (int)(((long long)p->page * 2 + 1) * nv.len / (2 * NV_PROG));
        if (start == RD_AUTO && !cfg_resume) {
            nv_ask = 1; nv_resume_off = off;
            snprintf(nv_b1, sizeof nv_b1, "Resume reading?");
            snprintf(nv_b2, sizeof nv_b2, "You stopped at %d%% of this book.", p->page * 100 / NV_PROG);
        }
    }
    nv_goto_off(off);
    if (!nv_ask) nv_save_pos();
    return 1;
}

static int nv_has_next(void) { return nv_vol + 1 < titles[nv_ti].nchs; }

static void nv_end_box(void) {
    Title *tl = &titles[nv_ti];
    if (nv_has_next()) {
        char t[200];
        snprintf(t, sizeof t, "End of %s", tl->chs[nv_vol].label);
        fit_text(F_SUB, t, 400, nv_b1, sizeof nv_b1);
        snprintf(t, sizeof t, "Up next: %s", tl->chs[nv_vol + 1].label);
        fit_text(F_BODY, t, 400, nv_b2, sizeof nv_b2);
    } else {
        snprintf(nv_b1, sizeof nv_b1, "The end");
        snprintf(nv_b2, sizeof nv_b2, "You've finished this book.");
    }
    nv_end = 1; nv_btn = 0;
}

static void nv_close(void) {
    nv_tex_clear();
    nvc_free();
    prog_save();
    cfg_save();
    in_novel = 0; nv_ask = nv_end = 0; nv_scrub = nv_fd = 0; nvm_on = 0;
    toast_t = 0;
    nv_free(&nv);
    nv_sec = -1; nv_npg = 0;
    if (in_details && d_ti == nv_ti && nv_vol < titles[nv_ti].nchs) {   /* Details: select the volume you read */
        d_focus = 1; d_sel = nv_vol;
        details_retarget();
    }
}

static void nv_finish(void) {   /* next pressed on the last page */
    if (nv_has_next() && cfg_auto && nv_load_vol(nv_ti, nv_vol + 1, RD_RESUME)) {
        toast("%s", titles[nv_ti].chs[nv_vol].label);
        return;
    }
    nv_end_box();
}
static void nv_next(void) {
    if (nv_npg <= 0) return;
    if (nv_page < nv_npg - 1) { nv_page++; nv_save_pos(); return; }
    if (nv_sec < nv.nch - 1) { nv_paginate(nv_sec + 1); nv_page = 0; nv_save_pos(); return; }
    nv_finish();
}
static void nv_prev(void) {
    if (nv_npg <= 0) return;
    if (nv_page > 0) { nv_page--; nv_save_pos(); return; }
    if (nv_sec > 0) { nv_paginate(nv_sec - 1); nv_page = nv_npg - 1; nv_save_pos(); return; }
    toast("This is the start of the book");
}

static void nv_relayout(void) {   /* font, size, spacing or margins changed: keep your place */
    if (nv_npg <= 0) return;
    int off = nv_pg[nv_page];
    nv_metrics();
    nv_tex_clear();
    nv_paginate(nv_sec);
    nv_page = nv_find_page(off);
}

static void nv_resize(int d) {
    int s = cfg_nvsize + d;
    if (s < 0 || s >= NV_SIZES) { toast(d > 0 ? "That's the biggest text size" : "That's the smallest text size"); return; }
    if (nv_npg <= 0) return;
    int old = cfg_nvsize;
    cfg_nvsize = s;
    if (!nv_fonts()) { cfg_nvsize = old; nv_fonts(); toast("Couldn't change the text size"); }
    else toast("Text size %d", nv_sizes[s]);
    nv_relayout();
}

/* ----- reader menu (R-2b-2) ----- */
enum { NM_THEME, NM_FONT, NM_SIZE, NM_LINE, NM_MARGIN, NM_JUST, NM_N };
#define NVM_ROW 56
#define NVM_PW  170
static const char *const nvm_names[NM_N] = { "Theme", "Font", "Text size", "Line spacing", "Margins", "Justify" };

static int nv_step(int v, int d, int n, int wrap) {
    v += d;
    if (v < 0) v = wrap ? n - 1 : 0;
    if (v >= n) v = wrap ? 0 : n - 1;
    return v;
}

static void nvm_geo(int *x, int *y, int *w, int *h) {   /* right side in landscape, bottom in portrait */
    int vw = rd_vw(), vh = rd_vh();
    *w = vw - 40 < 420 ? vw - 40 : 420;
    *h = 64 + NM_N * NVM_ROW + 44;
    *x = rd_portrait ? (vw - *w) / 2 : vw - *w - 20;
    *y = rd_portrait ? vh - *h - 20 : (vh - *h) / 2;
}

static void nvm_open(void) { if (nv_npg <= 0) return; nvm_on = 1; nvm_sel = 0; }
static void nvm_close(void) { nvm_on = 0; cfg_save(); }

static void nvm_change(int r, int d, int wrap) {
    if (r == NM_THEME) { cfg_nvtheme = nv_step(cfg_nvtheme, d, NV_THEMES, 1); nv_tex_clear(); return; }
    if (r == NM_JUST) { cfg_nvjust = !cfg_nvjust; nv_tex_clear(); return; }
    if (r == NM_FONT) {
        if (nv_nolit) { cfg_nvfont = 0; toast("Literata isn't installed (res/literata.ttf)"); return; }
        int old = cfg_nvfont;
        cfg_nvfont = !cfg_nvfont;
        if (!nv_fonts()) { cfg_nvfont = old; nv_fonts(); toast("Couldn't load that font"); return; }
    } else if (r == NM_SIZE) {
        int s = nv_step(cfg_nvsize, d, NV_SIZES, wrap), old = cfg_nvsize;
        if (s == old) return;
        cfg_nvsize = s;
        if (!nv_fonts()) { cfg_nvsize = old; nv_fonts(); toast("Couldn't change the text size"); return; }
    } else if (r == NM_LINE) {
        int v = nv_step(cfg_nvline, d, 3, wrap);
        if (v == cfg_nvline) return;
        cfg_nvline = v;
    } else if (r == NM_MARGIN) {
        int v = nv_step(cfg_nvmargin, d, 3, wrap);
        if (v == cfg_nvmargin) return;
        cfg_nvmargin = v;
    } else return;
    nv_relayout();
}

static void nvm_value(int r, char *o, int n) {
    switch (r) {
    case NM_THEME:  snprintf(o, n, "%s", nv_th()->name); break;
    case NM_FONT:   snprintf(o, n, "%s", nvf_face == 1 ? "Literata" : "Outfit"); break;
    case NM_SIZE:   snprintf(o, n, "%d", nv_sizes[cfg_nvsize]); break;
    case NM_LINE:   snprintf(o, n, "%s", nv_line_names[cfg_nvline]); break;
    case NM_MARGIN: snprintf(o, n, "%s", nv_margin_names[cfg_nvmargin]); break;
    default:        snprintf(o, n, "%s", cfg_nvjust ? "On" : "Off"); break;
    }
}

static void nvm_update(float dt, int tap, float tx, float ty) {
    unsigned U = SCE_CTRL_UP, D = SCE_CTRL_DOWN, L = SCE_CTRL_LEFT, R = SCE_CTRL_RIGHT;
    int vu = st_up, vd = st_down;
    if (rd_portrait) { U = SCE_CTRL_LEFT; D = SCE_CTRL_RIGHT; L = SCE_CTRL_DOWN; R = SCE_CTRL_UP; vu = st_left; vd = st_right; }
    if (pressed(SCE_CTRL_CIRCLE) || pressed(SCE_CTRL_SELECT)) { nvm_close(); return; }
    if (nav_rep(0, (btn & U) || vu, dt) && nvm_sel > 0) nvm_sel--;
    if (nav_rep(1, (btn & D) || vd, dt) && nvm_sel < NM_N - 1) nvm_sel++;
    if (pressed(L) || pressed(SCE_CTRL_LTRIGGER)) nvm_change(nvm_sel, -1, 0);
    if (pressed(R) || pressed(SCE_CTRL_RTRIGGER)) nvm_change(nvm_sel, 1, 0);
    if (pressed(SCE_CTRL_CROSS)) nvm_change(nvm_sel, 1, 1);
    if (pressed(SCE_CTRL_TRIANGLE)) nvm_change(NM_SIZE, 1, 0);
    if (pressed(SCE_CTRL_SQUARE)) nvm_change(NM_SIZE, -1, 0);
    if (!tap) return;
    int x, y, w, h;
    nvm_geo(&x, &y, &w, &h);
    if (tx < x || tx >= x + w || ty < y || ty >= y + h) { nvm_close(); return; }   /* tap outside closes */
    if (ty < y + 64) return;
    int i = (int)((ty - y - 64) / NVM_ROW), px = x + w - 24 - NVM_PW;
    if (i < 0 || i >= NM_N) return;
    nvm_sel = i;
    nvm_change(i, tx >= px && tx < px + NVM_PW / 2 ? -1 : 1, 1);   /* left half of the value = back */
}

static void nvm_draw(void) {
    int x, y, w, h;
    nvm_geo(&x, &y, &w, &h);
    rrect(x + 4, y + 8, w, h, 20, 0, 0, 0, 110);
    rrect(x, y, w, h, 20, C_PANEL, 250);
    rrect_line(x, y, w, h, 20, 1, C_LINE, 255);
    gc(C_ACCLT, 255); icon(IC_GEAR, x + 36, y + 34, 0.8f, C_PANEL);
    textc(F_SUB, x + 60, y + 34, AL_L, "Reader", C_TEXT, 255);
    for (int i = 0; i < NM_N; i++) {
        int ry = y + 64 + i * NVM_ROW, rh = NVM_ROW - 8, cy = ry + rh / 2, sel = i == nvm_sel;
        int px = x + w - 24 - NVM_PW;
        char v[32];
        if (sel) {
            rrect(x + 12, ry, w - 24, rh, 12, 42, 42, 56, 255);
            rrect_line(x + 12, ry, w - 24, rh, 12, 2, C_ACCENT, 255);
        }
        textc(F_BODYB, x + 28, cy, AL_L, nvm_names[i], C_TEXT, 255);
        if (i == NM_THEME) {   /* a little preview of the page colours */
            const NvTheme *th = nv_th();
            rrect(px - 46, cy - 14, 36, 28, 8, th->bg[0], th->bg[1], th->bg[2], 255);
            rrect_line(px - 46, cy - 14, 36, 28, 8, 1, C_LINE, 255);
            textc(F_SMALLB, px - 28, cy, AL_C, "Aa", th->fg[0], th->fg[1], th->fg[2], 255);
        }
        nvm_value(i, v, sizeof v);
        if (sel) {
            rrect(px, cy - 16, NVM_PW, 32, 16, C_TINT, 255);
            textc(F_SMALLB, px + NVM_PW / 2, cy, AL_C, v, C_ACCLT, 255);
            gc(C_ACCLT, 255);
            icon(IC_LEFT, px + 16, cy, 0.6f, 0, 0, 0);
            icon(IC_RIGHT, px + NVM_PW - 16, cy, 0.6f, 0, 0, 0);
        } else {
            rrect(px, cy - 16, NVM_PW, 32, 16, C_PANEL2, 255);
            textc(F_SMALLB, px + NVM_PW / 2, cy, AL_C, v, C_SOFT, 255);
        }
    }
    int hx = x + 20, hy = y + h - 24;
    rd_hint(&hx, hy, BI_DLR, "Change");
    rd_hint(&hx, hy, BI_O, "Close");
}

/* ----- resume / end box ----- */
static int nv_box_n(void) { return nv_ask || nv_has_next() ? 2 : 1; }
static void nv_box_btn(int i, int *x, int *y, int *w, int *h) {
    int bx, by, bw, bh, n = nv_box_n();
    rd_box_geo(&bx, &by, &bw, &bh);
    *w = (bw - 48 - (n - 1) * 16) / n; *h = 48;
    *x = bx + 24 + i * (*w + 16); *y = by + bh - 24 - 48;
}
static void nv_box_pick(int i) {
    if (nv_ask) { nv_ask = 0; nv_goto_off(i == 0 ? nv_resume_off : 0); nv_save_pos(); return; }
    if (nv_has_next() && i == 0) {
        nv_end = 0;
        if (nv_load_vol(nv_ti, nv_vol + 1, RD_RESUME)) toast("%s", titles[nv_ti].chs[nv_vol].label);
        else nv_end_box();
        return;
    }
    nv_close();
}

static void nv_open(int ti, int ch, int start) {
    if (!nv_fonts()) { toast("The novel reader couldn't load its fonts"); return; }
    rd_portrait = cfg_orient && rd_target();
    nv_metrics();
    if (!nv_load_vol(ti, ch, start)) return;
    in_novel = 1; nv_ui = 0; nv_jump = 0; nv_st_old = 0;
    nv_fd = nv_scrub = nv_tap = nv_swipe = 0;
    nv_idle = nv_save_t = nv_tick_t = 0;
}

/* ----- touch ----- */
static int nv_bar_y(void) { return rd_vh() - 66; }
static float nv_bar_f(float x) { return clampf((x - RD_BAR_X) / (float)(rd_vw() - 2 * RD_BAR_X), 0, 1); }

static float nv_fly, nv_dragy;   /* finger drag (Contents list) */
static void nv_touch_event(SDL_Event *ev) {
    if (ev->type != SDL_FINGERDOWN && ev->type != SDL_FINGERMOTION && ev->type != SDL_FINGERUP) return;
    if (ev->tfinger.touchId != touch_front_id()) return;
    float sx = ev->tfinger.x * SCREEN_W, sy = ev->tfinger.y * SCREEN_H, x, y;
    if (rd_portrait) { x = RD_PW - sy; y = sx; } else { x = sx; y = sy; }
    if (ev->type == SDL_FINGERDOWN) {
        if (nv_fd) return;   /* one finger only */
        nv_fd = 1; nv_fid = ev->tfinger.fingerId; nv_fmoved = 0; nv_ft0 = SDL_GetTicks();
        nv_fx0 = x; nv_fy0 = nv_fly = y;
        nv_scrub = nv_ui && !nv_ask && !nv_end && !nvc_on && !nvm_on && fabsf(y - nv_bar_y()) < 28 && x > 10 && x < rd_vw() - 10;
        if (nv_scrub) nv_scrub_f = nv_bar_f(x);
        return;
    }
    if (!nv_fd || ev->tfinger.fingerId != nv_fid) return;
    if (ev->type == SDL_FINGERMOTION) {
        if (fabsf(x - nv_fx0) > 14 || fabsf(y - nv_fy0) > 14) nv_fmoved = 1;
        if (nv_scrub) nv_scrub_f = nv_bar_f(x);
        else if (nv_fmoved) nv_dragy += y - nv_fly;
        nv_fly = y;
        return;
    }
    nv_fd = 0;   /* finger up */
    if (nv_scrub) { nv_scrub = 0; nv_jump = 1; return; }   /* jump when you let go */
    float mx = x - nv_fx0, my = y - nv_fy0;
    if (!nv_fmoved && SDL_GetTicks() - nv_ft0 < 500) { nv_tap = 1; nv_tx = x; nv_ty = y; }
    else if (fabsf(mx) > 90 && fabsf(mx) > fabsf(my) * 1.5f) nv_swipe = mx < 0 ? 1 : -1;
}

/* ----- update ----- */
static void update_novel(float dt) {
    int vw = rd_vw();
    if (btn != btn_old || nv_fd) nv_idle = 0; else nv_idle += dt;
    nv_tick_t -= dt;
    if (nv_idle < 300 && nv_tick_t <= 0) { sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); nv_tick_t = 5; }
    nv_save_t += dt;
    if (nv_save_t > 20) { nv_save_t = 0; prog_save(); }

    unsigned U = SCE_CTRL_UP, D = SCE_CTRL_DOWN, L = SCE_CTRL_LEFT, R = SCE_CTRL_RIGHT;
    if (rd_portrait) { U = SCE_CTRL_LEFT; D = SCE_CTRL_RIGHT; L = SCE_CTRL_DOWN; R = SCE_CTRL_UP; }
    int sn = rd_portrait ? st_up : st_right, sp = rd_portrait ? st_down : st_left;
    int st = sn ? 1 : sp ? -1 : 0, flick = st != nv_st_old ? st : 0;   /* one page per stick push */
    nv_st_old = st;
    int tap = nv_tap, swipe = nv_swipe;
    float tx = nv_tx, ty = nv_ty, dy = nv_dragy;
    nv_tap = nv_swipe = 0; nv_dragy = 0;

    if (nv_ask || nv_end) {
        int n = nv_box_n(), pick = -1;
        if ((pressed(L) || flick < 0) && nv_btn > 0) nv_btn--;
        if ((pressed(R) || flick > 0) && nv_btn < n - 1) nv_btn++;
        if (pressed(SCE_CTRL_CIRCLE)) { nv_close(); return; }
        if (pressed(SCE_CTRL_CROSS)) pick = nv_btn;
        for (int i = 0; tap && i < n; i++) {
            int x, y, w, h;
            nv_box_btn(i, &x, &y, &w, &h);
            if (tx >= x && tx < x + w && ty >= y && ty < y + h) pick = i;
        }
        if (pick >= 0) nv_box_pick(pick);
        return;
    }
    if (nvc_on) { nvc_update(dt, tap, tx, ty, dy); return; }
    if (nvm_on) { nvm_update(dt, tap, tx, ty); return; }
    if (nv_jump) { nv_jump = 0; nv_goto_off((int)(nv_scrub_f * nv.len)); nv_save_pos(); }
    if (pressed(SCE_CTRL_CIRCLE)) { nv_close(); return; }
    if (pressed(SCE_CTRL_START)) { nvc_open(); return; }
    if (pressed(SCE_CTRL_SELECT)) { nvm_open(); return; }
    if (pressed(SCE_CTRL_CROSS)) nv_ui = !nv_ui;
    if (pressed(SCE_CTRL_TRIANGLE)) nv_resize(1);
    if (pressed(SCE_CTRL_SQUARE)) nv_resize(-1);
    if (tap) {
        if (nv_ui && tx < 80 && ty < 80) { nv_close(); return; }
        if (nv_ui && tx > vw - 64 && ty < 80) { nvc_open(); return; }
        if (nv_ui && tx > vw - 116 && ty < 80) { nvm_open(); return; }
        if (tx < vw * 0.3f) { nv_prev(); return; }
        if (tx > vw * 0.7f) { nv_next(); return; }
        nv_ui = !nv_ui;
    }
    if (swipe > 0 || flick > 0 || pressed(SCE_CTRL_RTRIGGER) || pressed(R) || pressed(D)) { nv_next(); return; }
    if (swipe < 0 || flick < 0 || pressed(SCE_CTRL_LTRIGGER) || pressed(L) || pressed(U)) { nv_prev(); return; }
    if (nv_idle > 0.2f) nv_prefetch();
}

/* ----- draw ----- */
static void nv_draw_box(int vw, int vh) {
    fill(0, 0, vw, vh, 0, 0, 0, 150);
    int bx, by, bw, bh, n = nv_box_n();
    rd_box_geo(&bx, &by, &bw, &bh);
    rrect(bx, by, bw, bh, 20, C_PANEL, 250);
    rrect_line(bx, by, bw, bh, 20, 1, C_LINE, 255);
    text(F_SUB, bx + 24, by + 24, AL_L, nv_b1, C_TEXT, 255);
    text(F_BODY, bx + 24, by + 62, AL_L, nv_b2, C_SOFT, 255);
    for (int i = 0; i < n; i++) {
        int x, y, w, h, ic;
        const char *lab;
        nv_box_btn(i, &x, &y, &w, &h);
        if (nv_ask) { lab = i == 0 ? "Resume" : "Start over"; ic = i == 0 ? IC_PLAY : IC_REFRESH; }
        else if (n == 2) { lab = i == 0 ? "Next volume" : "Back"; ic = i == 0 ? IC_RIGHT : IC_LEFT; }
        else { lab = "Back"; ic = IC_LEFT; }
        button(x, y, w, h, i == 0, ic, lab, nv_btn == i, 255);
    }
}

static void draw_novel(void) {
    int port = rd_portrait && rd_rt, vw = rd_vw(), vh = rd_vh(), box = nv_ask || nv_end;
    if (port) SDL_SetRenderTarget(ren, rd_rt);
    fill(0, 0, vw, vh, NV_BG, 255);
    SDL_Texture *pt = nvc_on ? NULL : nv_page_tex(nv_page);
    if (pt) { SDL_Rect d = { 0, 0, vw, vh }; SDL_RenderCopy(ren, pt, NULL, &d); }
    else if (!nvc_on) textc(F_SUB, vw / 2, vh / 2, AL_C, "Couldn't draw this page", NV_DIM, 255);

    float f = nv_scrub ? nv_scrub_f : nv_frac();
    char pg[80];
    if (nvc_on) nvc_draw(vw, vh);
    else if (nvm_on) nvm_draw();
    else if (nv_ui && !box) {
        if (nv_light()) {   /* light pages: solid bars so the white text stays readable */
            fill(0, 0, vw, 84, C_BAR, 245);
            fill(0, vh - 140, vw, 140, C_BAR, 245);
        } else {
            grad_v(0, 0, vw, 100, 0, 0, 0, 230, 0);
            grad_v(0, vh - 140, vw, 140, 0, 0, 0, 0, 235);
        }
        disc(38, 38, 22, C_PANEL, 230);
        gc(C_TEXT, 255); icon(IC_LEFT, 37, 38, 0.85f, C_PANEL);
        disc(vw - 90, 38, 22, C_PANEL, 230);
        gc(C_TEXT, 255); icon(IC_GEAR, vw - 90, 38, 0.85f, C_PANEL);
        disc(vw - 38, 38, 22, C_PANEL, 230);
        gc(C_TEXT, 255); icon(IC_MENU, vw - 38, 38, 0.85f, C_PANEL);
        text(F_BODYB, 74, 16, AL_L, nv_title, C_TEXT, 255);
        text(F_SMALL, 74, 42, AL_L, nv_sub, C_SOFT, 255);
        if (nv_scrub) snprintf(pg, sizeof pg, "Go to %d%%", (int)(f * 100 + 0.5f));
        else snprintf(pg, sizeof pg, "Page %d / %d  \xE2\x80\xA2  %d%%", nv_page + 1, nv_npg, (int)(f * 100));
        textc(F_BODYB, vw / 2, vh - 98, AL_C, pg, C_TEXT, 255);
        int bw = vw - 2 * RD_BAR_X, by = nv_bar_y();
        rrect(RD_BAR_X, by - 3, bw, 6, 3, 255, 255, 255, 50);
        if (bw * f >= 6) rrect(RD_BAR_X, by - 3, (int)(bw * f), 6, 3, C_ACCENT, 255);
        disc(RD_BAR_X + bw * f, by, 9, C_TEXT, 255);
        int hx = 20, hy = vh - 24;
        rd_hint(&hx, hy, BI_DLR, "Page");
        rd_hint_key(&hx, hy, "START", "Contents");
        rd_hint_key(&hx, hy, "SELECT", "Reader menu");
        if (!rd_portrait) rd_hint(&hx, hy, BI_X, "Hide");
        rd_hint(&hx, hy, BI_O, "Back");
    } else if (!box) {
        snprintf(pg, sizeof pg, "%d%%", (int)(f * 100));
        textc(F_SMALL, vw / 2, vh - nv_bot / 2, AL_C, pg, NV_DIM, 255);
    }
    if (box) nv_draw_box(vw, vh);
    rd_ui = nv_ui && !nvm_on;   /* rd_draw_toast puts the toast above the overlay when it's shown */
    rd_draw_toast(vw, vh);

    if (port) {
        SDL_SetRenderTarget(ren, NULL);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_Rect d = { (SCREEN_W - RD_PW) / 2, (SCREEN_H - RD_PH) / 2, RD_PW, RD_PH };
        SDL_RenderCopyEx(ren, rd_rt, NULL, &d, 270, NULL, SDL_FLIP_NONE);
    }
}

static void set_feature(int i) {
    if (ntitles == 0) { wtex_free(&hero_syn); return; }
    feat = i; feat_t = 0; hero_a = 0;
    Title *tl = &titles[i];
    ensure_cover(tl);
    fit_text(F_HERO, tl->name, 600, hero_title, sizeof hero_title);
    title_meta(tl, hero_meta, sizeof hero_meta);
    genres_fmt(tl->genres, hero_genres, sizeof hero_genres);
    wtex_make(&hero_syn, F_BODY, tl->synopsis, 540, 2);
}
static void step_feature(int d) { if (ntitles > 0) set_feature((feat + d + ntitles) % ntitles); }

static void close_details(void) {
    wtex_free(&d_syn);
    in_details = 0;
}

static void open_details(int ti) {
    Title *tl = &titles[ti];
    if (in_details) close_details();
    in_details = 1; d_ti = ti; d_focus = 0; d_btn = 0; d_sel = 0;
    d_scroll = d_target = 0;
    ensure_cover(tl);
    fit_text(F_HERO, tl->name, SCREEN_W - 260, d_title, sizeof d_title);
    title_meta(tl, d_meta, sizeof d_meta);
    genres_fmt(tl->genres, d_genres, sizeof d_genres);
    wtex_make(&d_syn, F_BODY, tl->synopsis[0] ? tl->synopsis : "No description yet. Press Square to fetch info from MangaDex." , 690, 3);
}

static void go_page(int p) {
    if (in_details) close_details();
    page = p;
    drawer_open = 0;
}

static void do_rescan(void) {
    prog_gen++;   /* re-check reading progress for the new title list */
    if (in_details) close_details();
    scan_library();
    h_focus = h_btn = 0;
    for (int r = 0; r < 3; r++) { row_sel[r] = 0; row_scroll[r] = row_target[r] = 0; }
    g_sel[0] = g_sel[1] = 0; g_scroll[0] = g_scroll[1] = g_target[0] = g_target[1] = 0;
    h_scroll = h_target = 0;
    set_feature(0);
    toast("Library: %d manga, %d novels", nmanga, nnovel);
}

/* ===== Sources state (O-2) ===== */
static int   in_sources, in_online, src_seen, src_tab, src_off, src_sel, src_focus, src_btn = 1, src_ready;
static float src_scroll, src_target, src_tab_x;
static char  src_q[128], src_qfit[140], kb_buf[256];
static int   kb_on, kb_seen;
static SDL_Texture *src_tex[MD_PAGE];
static int   src_tex_st[MD_PAGE];          /* 0 waiting, 1 loaded, 2 none */
static char  src_name[MD_PAGE][128];
static int   on_ix, on_focus, on_btn, on_sel, on_mark = -1;
static float on_scroll, on_target;
static char  on_title[128], on_meta[260], on_genres[200];
static WTex  on_syn;
static char  on_lab[MD_CH_MAX][48], on_sub[MD_CH_MAX][128];
static unsigned char on_fit[MD_CH_MAX];
static int   dl_fails;
static char  dl_last[200], dl_fit[200];

static void sources_open(void);
static void sources_close(void);
static int  fetch_ti = -1, fetch_busy;   /* Fetch info: local title being matched */
static void fetch_open(int ti);
static void fetch_pick(int i);

static void open_drawer(void) { drawer_open = 1; drawer_sel = in_sources ? 3 : page; }

static void drawer_pick(int i) {
    if (i <= 2) { sources_close(); go_page(i); }
    else if (i == 3) { drawer_open = 0; sources_open(); }
    else if (i == 4) { drawer_open = 0; sources_close(); do_rescan(); }
    else { drawer_open = 0; settings_open(); }
}

/* ===== Home ===== */
static int hrow[3], nhrow;   /* visible rows, top to bottom (HR_ values) */

static int hr_n(int t) { return t == HR_CW ? ncw : (t == HR_NOVEL ? nnovel : nmanga); }
static int hr_slot(int t) {
    for (int i = 0; i < nhrow; i++) if (hrow[i] == t) return i;
    return -1;
}
static int home_step(int s, int d) {   /* next row with cards, -1 = none */
    for (int i = s + d; i >= 0 && i < nhrow; i += d)
        if (hr_n(hrow[i]) > 0) return i;
    return -1;
}
static void home_rows(void) {
    cw_refresh();
    nhrow = 0;
    if (ncw > 0) hrow[nhrow++] = HR_CW;
    hrow[nhrow++] = HR_MANGA;
    hrow[nhrow++] = HR_NOVEL;
}
static int row_y_of(int s) { return HERO_H + 16 + s * SEC_H; }

static void row_retarget(int t) {
    int n = hr_n(t);
    float left = (float)row_sel[t] * (CARD_W + CARD_GAP), view = SCREEN_W - 48;
    if (left < row_target[t]) row_target[t] = left;
    if (left + CARD_W > row_target[t] + view) row_target[t] = left + CARD_W - view;
    float maxs = n * (CARD_W + CARD_GAP) - CARD_GAP - view;
    if (maxs < 0) maxs = 0;
    row_target[t] = clampf(row_target[t], 0, maxs);
}

static void cw_play(int i) {   /* Continue Reading: open straight away */
    if (i < 0 || i >= ncw) return;
    rd_open(cw[i].ti, cw[i].ch, RD_RESUME);
}

static void update_home(float dt) {
    home_rows();
    int up = n_up(dt), down = n_down(dt), left = n_left(dt), right = n_right(dt), moved = 0;
    if (h_focus > 0 && hr_slot(h_focus - 1) < 0) {   /* the focused row went away */
        int s = home_step(-1, 1);
        h_focus = s >= 0 ? hrow[s] + 1 : 0;
        moved = 1;
    }
    if (h_focus == 0) {
        if (ntitles > 0) {
            if (left)  { if (h_btn == 1) h_btn = 0; else step_feature(-1); }
            if (right) { if (h_btn == 0) h_btn = 1; else step_feature(1); }
        }
        if (down) { int s = home_step(-1, 1); if (s >= 0) { h_focus = hrow[s] + 1; moved = 1; } }
        if (pressed(SCE_CTRL_CROSS) && ntitles > 0) {
            if (h_btn == 0) read_chapter(feat, title_resume_ch(&titles[feat]));
            else open_details(feat);
            return;
        }
    } else {
        int t = h_focus - 1, s = hr_slot(t), n = hr_n(t);
        if (row_sel[t] > n - 1) { row_sel[t] = n > 0 ? n - 1 : 0; row_retarget(t); }
        if (left && row_sel[t] > 0) { row_sel[t]--; row_retarget(t); }
        if (right && row_sel[t] < n - 1) { row_sel[t]++; row_retarget(t); }
        if (up) { int p = home_step(s, -1); h_focus = p >= 0 ? hrow[p] + 1 : 0; moved = 1; }
        if (down) { int q = home_step(s, 1); if (q >= 0) { h_focus = hrow[q] + 1; moved = 1; } }
        if (n > 0 && pressed(SCE_CTRL_CROSS)) {
            if (t == HR_CW) cw_play(row_sel[t]);
            else open_details((t == HR_NOVEL ? novel_ix : manga_ix)[row_sel[t]]);
            return;
        }
        if (t == HR_CW && n > 0 && pressed(SCE_CTRL_TRIANGLE)) cw_remove(row_sel[t]);
    }

    float maxs = row_y_of(nhrow) - SCREEN_H + 10;
    if (maxs < 0) maxs = 0;
    if (drag_dy != 0) {
        h_scroll = clampf(h_scroll + drag_dy, 0, maxs);
        h_target = h_scroll;
    } else {
        if (moved) h_target = h_focus == 0 ? 0 : row_y_of(hr_slot(h_focus - 1)) - BAR_H - 14;
        h_target = clampf(h_target, 0, maxs);
        h_scroll += (h_target - h_scroll) * lerpk(dt);
    }

    for (int s = 0; s < nhrow; s++) {
        int t = hrow[s], n = hr_n(t);
        float maxr = n * (CARD_W + CARD_GAP) - CARD_GAP - (SCREEN_W - 48);
        if (maxr < 0) maxr = 0;
        int cy = row_y_of(s) + 76 - (int)h_scroll;
        if (drag_dx != 0 && t_y0 >= cy && t_y0 < cy + CARD_H + 30) {
            row_scroll[t] = clampf(row_scroll[t] + drag_dx, 0, maxr);
            row_target[t] = row_scroll[t];
        } else row_scroll[t] += (row_target[t] - row_scroll[t]) * lerpk(dt);
    }
}

static void draw_hero(int y) {
    if (ntitles == 0) {
        fill(0, y, SCREEN_W, HERO_H, 18, 18, 26, 255);
        grad_v(0, y + HERO_H - 120, SCREEN_W, 120, C_BG, 0, 255);
        dango_icon(820, y + 190, 3.0f);
        text(F_HERO, 48, y + 100, AL_L, "Welcome to aniread", C_TEXT, 255);
        text(F_BODY, 48, y + 166, AL_L, "Put each manga in its own folder in ux0:data/aniread/manga/", C_SOFT, 255);
        text(F_BODY, 48, y + 194, AL_L, "and each novel in ux0:data/aniread/novels/", C_SOFT, 255);
        text(F_BODY, 48, y + 222, AL_L, "then press Triangle to rescan.", C_SOFT, 255);
        return;
    }
    Title *tl = &titles[feat];
    int a = (int)(hero_a * 255);
    draw_backdrop(feat, y, HERO_H, a);
    const int tx = 48;
    text(F_HERO, tx, y + 84, AL_L, hero_title, C_TEXT, a);
    text(F_BODY, tx, y + 142, AL_L, hero_meta, C_SOFT, a);
    int gx = tx;
    if (hero_genres[0]) gx += textc(F_BODY, tx, y + 190, AL_L, hero_genres, C_DIM, a) + 16;
    if (tl->rating[0]) rating_pill(gx, y + 190, tl->rating, a);
    wtex_draw(&hero_syn, tx, y + 218, C_DIM, a);
    int by = y + 270;
    button(tx, by, 196, 48, 1, IC_BOOK, "Read Now", h_focus == 0 && h_btn == 0, a);
    button(tx + 214, by, 156, 48, 0, IC_INFO, "Details", h_focus == 0 && h_btn == 1, a);
    hit(tx, by, 196, 48, H_HPLAY, 0, 1);
    hit(tx + 214, by, 156, 48, H_HDETAILS, 0, 1);
    if (ntitles > 1) {
        circle_btn(SCREEN_W - 104, by + 24, IC_LEFT, H_HPREV, 1);
        circle_btn(SCREEN_W - 48, by + 24, IC_RIGHT, H_HNEXT, 1);
    }
}

static void draw_cw_card(int i, int x, int y, int w, int h, int sel) {
    CWItem *c = &cw[i];
    Title *tl = &titles[c->ti];
    if (tl->cover_state == 0 && cover_budget > 0) { cover_budget--; ensure_cover(tl); }
    if (tl->cover) {
        SDL_Rect d = { x, y, w, h };
        tex_col(tl->cover, 255, 255, 255, 255);
        SDL_RenderCopy(ren, tl->cover, NULL, &d);
    } else {
        char fc[8]; first_char(tl->name, fc);
        rrect(x, y, w, h, 10, C_PANEL, 255);
        textc(F_H2, x + w / 2, y + h / 2, AL_C, fc, C_DIM, 255);
    }
    int tw = text_w(F_SMALLB, c->tag) + 16;
    rrect(x + 8, y + 8, tw, 26, 9, 18, 18, 24, 215);
    textc(F_SMALLB, x + 8 + tw / 2, y + 21, AL_C, c->tag, C_TEXT, 255);
    int lw = text_w(F_SMALLB, c->left) + 14;
    rrect(x + 8, y + h - 34, lw, 24, 8, 18, 18, 24, 215);
    textc(F_SMALLB, x + 8 + lw / 2, y + h - 22, AL_C, c->left, C_SOFT, 255);
    if (c->pages > 0 && c->page > 0) {
        int bw = w - 16, fw = (int)(bw * clampf((float)c->page / (float)c->pages, 0, 1));
        rrect(x + 8, y + h - 7, bw, 4, 2, 0, 0, 0, 190);
        if (fw >= 4) rrect(x + 8, y + h - 7, fw, 4, 2, C_ACCENT, 255);
    }
    if (sel) {
        disc(x + w / 2, y + h / 2, 24, C_ACCENT, 235);
        gc(C_TEXT, 255); icon(IC_PLAY, x + w / 2 + 2, y + h / 2, 1.0f, 108, 99, 255);
        rrect_line(x - 5, y - 5, w + 10, h + 10, 15, 3, C_ACCENT, 255);
        text(F_SMALLB, x, y + h + 10, AL_L, tl->card_name, C_ACCLT, 255);
    } else text(F_SMALLB, x, y + h + 10, AL_L, tl->card_name, C_TEXT, 255);
}

static void draw_row(int s, int y) {
    int t = hrow[s], n = hr_n(t), foc = (h_focus == t + 1), cy = y + 76;
    if (t == HR_CW) {
        sec_head(24, y, "PICK UP WHERE YOU LEFT OFF", "Continue Reading");
        for (int i = 0; i < n; i++) {
            int x = 24 + i * (CARD_W + CARD_GAP) - (int)row_scroll[t];
            if (x + CARD_W < 0 || x > SCREEN_W) continue;
            draw_cw_card(i, x, cy, CARD_W, CARD_H, foc && row_sel[t] == i);
            hit(x, cy, CARD_W, CARD_H + 30, H_ROWCARD, (t << 16) | i, 1);
        }
        return;
    }
    int *ix = t == HR_NOVEL ? novel_ix : manga_ix;
    sec_head(24, y, "YOUR LIBRARY", t == HR_NOVEL ? "Novels" : "Manga");
    if (n > 0) {
        int px = pill_btn(SCREEN_W - 24, y + 34, "See all", 1);
        hit(px, y + 16, SCREEN_W - 24 - px, 36, H_SEEALL, t == HR_NOVEL ? PG_NOVELS : PG_MANGA, 1);
    }
    if (n == 0) {
        rrect(24, cy, SCREEN_W - 48, 110, 16, C_PANEL, 255);
        textc(F_BODY, SCREEN_W / 2, cy + 55, AL_C,
              t == HR_NOVEL ? "No novels yet. Add them to ux0:data/aniread/novels/"
                            : "No manga yet. Add title folders to ux0:data/aniread/manga/", C_DIM, 255);
        return;
    }
    for (int i = 0; i < n; i++) {
        int x = 24 + i * (CARD_W + CARD_GAP) - (int)row_scroll[t];
        if (x + CARD_W < 0 || x > SCREEN_W) continue;
        draw_card(ix[i], x, cy, CARD_W, CARD_H, foc && row_sel[t] == i);
        hit(x, cy, CARD_W, CARD_H + 30, H_ROWCARD, (t << 16) | i, 1);
    }
}

static void draw_home(void) {
    home_rows();
    fill(0, 0, SCREEN_W, SCREEN_H, C_BG, 255);
    int oy = -(int)h_scroll;
    draw_hero(oy);
    for (int s = 0; s < nhrow; s++) {
        int y = row_y_of(s) + oy;
        if (y + SEC_H < BAR_H || y > SCREEN_H) continue;
        draw_row(s, y);
    }
}

/* ===== Manga / Novels grid pages ===== */
static void update_grid(int k, float dt) {
    int *ix = k ? novel_ix : manga_ix, n = k ? nnovel : nmanga, moved = 0;
    int *sel = &g_sel[k];
    if (n > 0) {
        if (n_left(dt) && *sel > 0) { (*sel)--; moved = 1; }
        if (n_right(dt) && *sel < n - 1) { (*sel)++; moved = 1; }
        if (n_up(dt) && *sel - GCOLS >= 0) { *sel -= GCOLS; moved = 1; }
        if (n_down(dt)) {
            if (*sel + GCOLS < n) *sel += GCOLS;
            else if (*sel / GCOLS < (n - 1) / GCOLS) *sel = n - 1;
            moved = 1;
        }
        if (pressed(SCE_CTRL_CROSS)) { open_details(ix[*sel]); return; }
    }
    int nr = (n + GCOLS - 1) / GCOLS;
    float maxs = GRID_Y + nr * GROW_H - (SCREEN_H - 10);
    if (maxs < 0) maxs = 0;
    if (drag_dy != 0) {
        g_scroll[k] = clampf(g_scroll[k] + drag_dy, 0, maxs);
        g_target[k] = g_scroll[k];
    } else {
        if (moved) {
            int top = GRID_Y + (*sel / GCOLS) * GROW_H;
            if (*sel < GCOLS) g_target[k] = 0;
            else if (top - g_target[k] < BAR_H + 20) g_target[k] = top - BAR_H - 20;
            if (top + GROW_H - g_target[k] > SCREEN_H - 10) g_target[k] = top + GROW_H - (SCREEN_H - 10);
            g_target[k] = clampf(g_target[k], 0, maxs);
        }
        g_scroll[k] += (g_target[k] - g_scroll[k]) * lerpk(dt);
    }
}

static void draw_grid(int k) {
    int *ix = k ? novel_ix : manga_ix, n = k ? nnovel : nmanga;
    int oy = -(int)g_scroll[k];
    fill(0, 0, SCREEN_W, SCREEN_H, C_BG, 255);
    const int x0 = (SCREEN_W - (GCOLS * GCARD_W + (GCOLS - 1) * GGAP)) / 2;
    sec_head(x0, 78 + oy, "LIBRARY", k ? "Novels" : "Manga");
    char c[32];
    snprintf(c, sizeof c, k ? "%d NOVELS" : "%d TITLES", n);
    pill_btn(SCREEN_W - x0, 108 + oy, c, 0);
    if (n == 0) {
        textc(F_BODY, SCREEN_W / 2, 280, AL_C,
              k ? "Add novels to ux0:data/aniread/novels/" : "Add title folders to ux0:data/aniread/manga/", C_DIM, 255);
        return;
    }
    for (int i = 0; i < n; i++) {
        int x = x0 + (i % GCOLS) * (GCARD_W + GGAP);
        int y = GRID_Y + (i / GCOLS) * GROW_H + oy;
        if (y + GROW_H < BAR_H || y > SCREEN_H) continue;
        draw_card(ix[i], x, y, GCARD_W, GCARD_H, g_sel[k] == i);
        hit(x, y, GCARD_W, GCARD_H + 30, H_GRIDCARD, i, 1);
    }
}

/* ===== END OF PART 3 ===== */
/* ===== Details page ===== */
static void details_retarget(void) {
    Title *tl = &titles[d_ti];
    float maxs = D_LIST_Y + tl->nchs * D_ROW_EP - (SCREEN_H - 40);
    if (maxs < 0) maxs = 0;
    if (d_focus == 0 || tl->nchs == 0) { d_target = 0; return; }
    int top = D_LIST_Y + d_sel * D_ROW_EP;
    if (top - d_target < BAR_H + 16) d_target = top - BAR_H - 16;
    if (top + D_ROW_EP - d_target > SCREEN_H - 40) d_target = top + D_ROW_EP - (SCREEN_H - 40);
    d_target = clampf(d_target, 0, maxs);
}

static void update_details(float dt) {
    Title *tl = &titles[d_ti];
    int up = n_up(dt), down = n_down(dt), left = n_left(dt), right = n_right(dt), moved = 0;
    if (pressed(SCE_CTRL_CIRCLE)) { close_details(); return; }
    if (pressed(SCE_CTRL_SQUARE)) { fetch_open(d_ti); return; }
    if (d_focus == 0) {
        if (left && d_btn > 0) d_btn--;
        if (right && d_btn < 1) d_btn++;
        if (down && tl->nchs > 0) { d_focus = 1; moved = 1; }
        if (pressed(SCE_CTRL_CROSS)) {
            if (d_btn == 0) read_chapter(d_ti, title_resume_ch(tl));
            else { close_details(); return; }
        }
    } else {
        if (up) { if (d_sel == 0) d_focus = 0; else d_sel--; moved = 1; }
        if (down && d_sel < tl->nchs - 1) { d_sel++; moved = 1; }
        if (left) { d_sel = d_sel > 10 ? d_sel - 10 : 0; moved = 1; }
        if (right) { d_sel = d_sel + 10 < tl->nchs ? d_sel + 10 : tl->nchs - 1; moved = 1; }
        if (pressed(SCE_CTRL_CROSS)) read_chapter(d_ti, d_sel);
    }
    float maxs = tl->nchs > 0 ? D_LIST_Y + tl->nchs * D_ROW_EP - (SCREEN_H - 40) : 0;
    if (maxs < 0) maxs = 0;
    if (drag_dy != 0) {
        d_scroll = clampf(d_scroll + drag_dy, 0, maxs);
        d_target = d_scroll;
    } else {
        if (moved) details_retarget();
        d_scroll += (d_target - d_scroll) * lerpk(dt);
    }
}

static void draw_details(void) {
    Title *tl = &titles[d_ti];
    int oy = -(int)d_scroll;
    fill(0, 0, SCREEN_W, SCREEN_H, C_BG, 255);
    draw_backdrop(d_ti, oy, 410, 255);

    int px = 40, py = 84 + oy, pw = 160, ph = 240;
    rrect(px + 4, py + 8, pw, ph, 12, 0, 0, 0, 120);
    if (tl->cover) {
        SDL_Rect d = { px, py, pw, ph };
        tex_col(tl->cover, 255, 255, 255, 255);
        SDL_RenderCopy(ren, tl->cover, NULL, &d);
    } else {
        char fc[8]; first_char(tl->name, fc);
        rrect(px, py, pw, ph, 12, C_PANEL, 255);
        textc(F_HERO, px + pw / 2, py + ph / 2, AL_C, fc, C_DIM, 255);
    }

    int ix = 228;
    text(F_HERO, ix, 80 + oy, AL_L, d_title, C_TEXT, 255);
    text(F_BODY, ix, 136 + oy, AL_L, d_meta, C_SOFT, 255);
    int gx = ix;
    if (d_genres[0]) gx += textc(F_BODY, ix, 182 + oy, AL_L, d_genres, C_DIM, 255) + 16;
    if (tl->rating[0]) rating_pill(gx, 182 + oy, tl->rating, 255);
    wtex_draw(&d_syn, ix, 210 + oy, C_DIM, 255);
    int by = 296 + oy;
    button(ix, by, 196, 48, 1, IC_BOOK, "Read Now", d_focus == 0 && d_btn == 0, 255);
    button(ix + 214, by, 130, 48, 0, IC_LEFT, "Back", d_focus == 0 && d_btn == 1, 255);
    hit(ix, by, 196, 48, H_DPLAY, 0, 1);
    hit(ix + 214, by, 130, 48, H_DBACK, 0, 1);

    int manga = tl->kind == K_MANGA;
    if (tl->nchs == 0) {
        rrect(40, 400 + oy, SCREEN_W - 80, 90, 16, C_PANEL, 255);
        textc(F_BODY, SCREEN_W / 2, 445 + oy, AL_C, manga ? "No chapters yet. Add chapter folders to this title's chapters folder."
                                                          : "No files yet. Add .epub, .pdf or .txt files to this folder.", C_DIM, 255);
        return;
    }
    sec_head(40, 380 + oy, manga ? "CHAPTERS" : "VOLUMES", manga ? "Chapters" : "Volumes");
    char cnt[48];
    snprintf(cnt, sizeof cnt, manga ? "%d CHAPTERS" : "%d VOLUMES", tl->nchs);
    pill_btn(SCREEN_W - 40, 414 + oy, cnt, 0);

    int first = (BAR_H - (D_LIST_Y + oy)) / D_ROW_EP - 1;
    if (first < 0) first = 0;
    for (int i = first; i < tl->nchs; i++) {
        int y = D_LIST_Y + i * D_ROW_EP + oy;
        if (y > SCREEN_H) break;
        if (y + D_ROW_EP < BAR_H) continue;
        Chapter *c = &tl->chs[i];
        int sel = (d_focus == 1 && i == d_sel);
        rrect(40, y, SCREEN_W - 80, D_ROW_EP - 8, 14, sel ? 42 : 28, sel ? 42 : 28, sel ? 56 : 36, 255);
        if (sel) rrect_line(40, y, SCREEN_W - 80, D_ROW_EP - 8, 14, 2, C_ACCENT, 255);
        rrect(52, y + 9, 56, 40, 10, 16, 16, 22, 255);
        char num[16];
        if (c->number > 0) snprintf(num, sizeof num, "%d", c->number); else snprintf(num, sizeof num, "-");
        if (sel) textc(F_SUB, 80, y + 29, AL_C, num, C_ACCLT, 255);
        else     textc(F_SUB, 80, y + 29, AL_C, num, C_SOFT, 255);
        text(F_BODYB, 124, y + 7, AL_L, c->label, C_TEXT, 255);
        text(F_SMALL, 124, y + 32, AL_L, c->sub, C_DIM, 255);
        Prog *p = prog_find(c->path);
        if (p && p->page > 0 && p->pages > 0) {
            int fw = (int)(52 * clampf((float)p->page / (float)p->pages, 0, 1));
            rrect(54, y + 44, 52, 3, 1, 255, 255, 255, 40);
            if (fw >= 2) rrect(54, y + 44, fw, 3, 1, C_ACCENT, 255);
        }
        if (p && p->done && !sel) {
            disc(SCREEN_W - 74, y + 29, 14, C_TINT, 255);
            gc(C_ACCLT, 255); check_mark(SCREEN_W - 74, y + 29, 0.8f);
        }
        if (sel) {
            disc(SCREEN_W - 74, y + 29, 17, C_ACCENT, 255);
            gc(C_TEXT, 255); icon(IC_PLAY, SCREEN_W - 72, y + 29, 0.65f, 108, 99, 255);
        }
        hit(40, y, SCREEN_W - 80, D_ROW_EP - 8, H_CH, i, 1);
    }
}

/* ===== Top bar, hints, drawer, toast ===== */
static const char *tab_names[3] = { "Home", "Manga", "Novels" };

static void draw_topbar(void) {
    fill(0, 0, SCREEN_W, BAR_H, C_BAR, 240);
    fill(0, BAR_H - 1, SCREEN_W, 1, C_LINE, 255);
    disc(38, 30, 22, C_PANEL, 255);
    gc(C_TEXT, 255); icon(IC_MENU, 38, 30, 0.9f, C_PANEL);
    hit(14, 6, 48, 48, H_MENU, 0, 0);

    rrect(70, 8, 178, 44, 22, C_PANEL, 255);
    dango_icon(100, 30, 0.75f);
    int w = textc(F_LOGO, 124, 29, AL_L, "ani", C_TEXT, 255);
    textc(F_LOGO, 124 + w, 29, AL_L, "read", C_ACCLT, 255);

    const int nx = (SCREEN_W - 308) / 2;
    rrect(nx, 8, 308, 44, 22, C_PANEL, 255);
    rrect(nx + 4 + (int)tab_x, 12, 100, 36, 18, C_PANEL2, 255);
    for (int i = 0; i < 3; i++) {
        int cx = nx + 4 + i * 100 + 50;
        if (i == page) textc(F_BODYB, cx, 30, AL_C, tab_names[i], C_TEXT, 255);
        else           textc(F_BODYB, cx, 30, AL_C, tab_names[i], C_DIM, 255);
        hit(nx + 4 + i * 100, 8, 100, 44, H_TAB, i, 0);
    }

    disc(SCREEN_W - 94, 30, 22, C_PANEL, 255);
    gc(C_TEXT, 255); icon(IC_REFRESH, SCREEN_W - 94, 30, 0.85f, C_PANEL);
    hit(SCREEN_W - 118, 6, 48, 48, H_REFRESH, 0, 0);
    disc(SCREEN_W - 40, 30, 22, C_PANEL, 255);
    gc(C_TEXT, 255); icon(IC_GEAR, SCREEN_W - 40, 30, 0.85f, C_PANEL);
    hit(SCREEN_W - 64, 6, 48, 48, H_SETTINGS, 0, 0);
}

static void draw_hints(void) {
    grad_v(0, SCREEN_H - 46, SCREEN_W, 46, C_BG, 0, 235);
    int x = 20;
    if (in_settings) {
        x = hint(x, BI_X, st_sel == ST_CLEAR ? (st_confirm ? "Confirm" : "Clear") : "Change");
        if (st_sel < ST_CLEAR) x = hint(x, BI_DLR, "Change");
        x = hint(x, BI_O, "Back");
    } else if (drawer_open) {
        x = hint(x, BI_X, "Select");
        x = hint(x, BI_O, "Close");
    } else if (in_sources && in_online) {
        if (on_focus == 1 && on_sel < md_nch) {
            x = hint(x, BI_X, md_ch[on_sel].have ? "Downloaded" : "Download");
            x = hint(x, BI_SQ, on_mark < 0 ? "Range start" : "Range end");
            if (on_mark >= 0) x = hint(x, BI_TRI, "Cancel range");
            else x = hint(x, BI_DLR, "Jump 10");
        } else x = hint(x, BI_X, "Select");
        x = hint(x, BI_O, "Back");
    } else if (in_sources) {
        x = hint(x, BI_X, fetch_ti >= 0 ? "Use this" : "Open");
        x = hint(x, BI_SQ, "Search");
        if (fetch_ti < 0) x = hint_key(x, "L / R", "Tabs");
        x = hint(x, BI_O, "Back");
    } else if (in_details) {
        x = hint(x, BI_X, "Read");
        x = hint(x, BI_SQ, "Fetch info");
        x = hint(x, BI_O, "Back");
        if (d_focus == 1) x = hint(x, BI_DLR, "Jump 10");
    } else {
        int cwf = (page == PG_HOME && h_focus == HR_CW + 1);
        x = hint(x, BI_X, cwf ? "Continue" : "Select");
        x = hint_key(x, "L / R", "Tabs");
        x = hint(x, BI_TRI, cwf ? "Remove" : "Rescan");
        x = hint_key(x, "START", "Menu");
    }
}

#define DRAWER_N 6
static const char *drawer_names[DRAWER_N] = { "Home", "Manga", "Novels", "Sources", "Rescan library", "Settings" };
static const int   drawer_icons[DRAWER_N] = { IC_HOME, IC_BOOK, IC_NOVEL, IC_DOWN, IC_REFRESH, IC_GEAR };

static void draw_drawer(void) {
    if (drawer_t <= 0.01f) return;
    float e = 1 - (1 - drawer_t) * (1 - drawer_t) * (1 - drawer_t);
    fill(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, (int)(160 * drawer_t));
    int x = (int)(-DRAWER_W * (1 - e));
    if (drawer_open) {
        hit(0, 0, SCREEN_W, SCREEN_H, H_DRAWER_OUT, 0, 0);
        hit(x, 0, DRAWER_W, SCREEN_H, H_NONE, 0, 0);
    }
    fill(x, 0, DRAWER_W, SCREEN_H, 15, 15, 19, 255);
    fill(x + DRAWER_W - 1, 0, 1, SCREEN_H, C_LINE, 255);
    dango_icon(x + 46, 46, 1.1f);
    int w = textc(F_H2, x + 80, 46, AL_L, "ani", C_TEXT, 255);
    textc(F_H2, x + 80 + w, 46, AL_L, "read", C_ACCLT, 255);
    fill(x, 94, DRAWER_W, 1, C_LINE, 255);
    for (int i = 0; i < DRAWER_N; i++) {
        int y = 116 + i * 56, sel = (i == drawer_sel);
        if (sel) rrect(x + 16, y, DRAWER_W - 32, 46, 16, C_TINT, 255);
        if (sel) gc(C_ACCLT, 255); else gc(C_SOFT, 255);
        if (sel) icon(drawer_icons[i], x + 48, y + 23, 0.9f, C_TINT);
        else     icon(drawer_icons[i], x + 48, y + 23, 0.9f, 15, 15, 19);
        if (sel) textc(F_SUB, x + 80, y + 23, AL_L, drawer_names[i], C_ACCLT, 255);
        else     textc(F_SUB, x + 80, y + 23, AL_L, drawer_names[i], C_SOFT, 255);
        if (drawer_open) hit(x + 16, y, DRAWER_W - 32, 46, H_DRAWER_ITEM, i, 0);
    }
    disc(x + 30, SCREEN_H - 30, 5, 60, 200, 120, 255);
    textc(F_BODY, x + 44, SCREEN_H - 30, AL_L, APP_VERSION, C_SOFT, 255);
}

static void update_drawer(float dt) {
    if (n_up(dt) && drawer_sel > 0) drawer_sel--;
    if (n_down(dt) && drawer_sel < DRAWER_N - 1) drawer_sel++;
    n_left(dt); n_right(dt);
    if (pressed(SCE_CTRL_CROSS)) drawer_pick(drawer_sel);
    else if (pressed(SCE_CTRL_CIRCLE) || pressed(SCE_CTRL_START)) drawer_open = 0;
}

static void draw_toast(void) {
    if (toast_t <= 0) return;
    int a = toast_t < 0.3f ? (int)(toast_t / 0.3f * 255) : 255;
    int w = text_w(F_BODYB, toast_msg) + 56, x = (SCREEN_W - w) / 2, y = SCREEN_H - 96;
    rrect(x, y, w, 44, 22, C_PANEL2, a * 245 / 255);
    rrect_line(x, y, w, 44, 22, 1, C_LINE, a);
    disc(x + 24, y + 22, 5, C_ACCENT, a);
    textc(F_BODYB, x + 38, y + 22, AL_L, toast_msg, C_TEXT, a);
}

/* ===== Settings screen ===== */
#define ST_Y     92
#define ST_ROW   68
static const char *const st_names[ST_N] = { "Auto next chapter", "Hold the Vita", "Resume", "Page fit", "Clear reading history" };
static const char *const st_desc[ST_N] = {
    "Go to the next chapter when you finish one",
    "Landscape, or turned sideways like a book (portrait)",
    "What happens when you open a started chapter",
    "How manga pages fill the screen",
    "Removes progress, checkmarks and Continue Reading" };

static void settings_open(void) { in_settings = 1; st_sel = 0; st_confirm = 0; drawer_open = 0; }
static void settings_close(void) { cfg_save(); in_settings = 0; st_confirm = 0; }

static void st_change(int i) {
    if (i == 0) cfg_auto = !cfg_auto;
    else if (i == 1) cfg_orient = !cfg_orient;
    else if (i == 2) cfg_resume = !cfg_resume;
    else if (i == 3) cfg_fit = !cfg_fit;
}

static void st_activate(int i) {
    if (i == ST_CLEAR) {
        if (!st_confirm) { st_confirm = 1; return; }
        st_confirm = 0;
        prog_clear();
        toast("Reading history cleared");
        return;
    }
    st_change(i);
}

static void update_settings(float dt) {
    if (pressed(SCE_CTRL_CIRCLE)) { settings_close(); return; }
    int up = n_up(dt), down = n_down(dt), left = n_left(dt), right = n_right(dt);
    if (up && st_sel > 0) { st_sel--; st_confirm = 0; }
    if (down && st_sel < ST_N - 1) { st_sel++; st_confirm = 0; }
    if (st_sel < ST_CLEAR && (left || right)) st_change(st_sel);
    if (pressed(SCE_CTRL_CROSS)) st_activate(st_sel);
}

static const char *st_value(int i) {
    if (i == 0) return cfg_auto ? "On" : "Off";
    if (i == 1) return cfg_orient ? "Portrait" : "Landscape";
    if (i == 2) return cfg_resume ? "Always resume" : "Ask";
    if (i == 3) return cfg_fit ? "Fit width" : "Fit screen";
    return st_confirm ? "Press again to clear" : "Clear";
}

static void draw_settings(void) {
    fill(0, 0, SCREEN_W, SCREEN_H, C_BG, 255);
    disc(40, 40, 22, C_PANEL, 255);
    gc(C_TEXT, 255); icon(IC_LEFT, 39, 40, 0.85f, 0, 0, 0);
    hit(14, 14, 52, 52, H_ST_BACK, 0, 0);
    text(F_H2, 78, 22, AL_L, "Settings", C_TEXT, 255);
    for (int i = 0; i < ST_N; i++) {
        int y = ST_Y + i * ST_ROW, sel = (i == st_sel), rh = ST_ROW - 8;
        rrect(40, y, SCREEN_W - 80, rh, 14, sel ? 42 : 28, sel ? 42 : 28, sel ? 56 : 36, 255);
        if (sel) rrect_line(40, y, SCREEN_W - 80, rh, 14, 2, C_ACCENT, 255);
        text(F_BODYB, 64, y + 7, AL_L, st_names[i], C_TEXT, 255);
        text(F_SMALL, 64, y + 33, AL_L, st_desc[i], C_DIM, 255);
        const char *v = st_value(i);
        int pw = 220, px = SCREEN_W - 64 - pw, cy = y + rh / 2;
        if (i == ST_CLEAR && st_confirm) {
            rrect(px, cy - 17, pw, 34, 17, 190, 60, 72, 255);
            textc(F_SMALLB, px + pw / 2, cy, AL_C, v, C_TEXT, 255);
        } else if (sel) {
            rrect(px, cy - 17, pw, 34, 17, C_TINT, 255);
            textc(F_SMALLB, px + pw / 2, cy, AL_C, v, C_ACCLT, 255);
        } else {
            rrect(px, cy - 17, pw, 34, 17, C_PANEL2, 255);
            textc(F_SMALLB, px + pw / 2, cy, AL_C, v, C_SOFT, 255);
        }
        if (i < ST_CLEAR && sel) {
            gc(C_ACCLT, 255);
            icon(IC_LEFT, px + 18, cy, 0.6f, 0, 0, 0);
            icon(IC_RIGHT, px + pw - 18, cy, 0.6f, 0, 0, 0);
        }
        hit(40, y, SCREEN_W - 80, rh, H_ST_ROW, i, 0);
    }
    text(F_SMALL, 40, ST_Y + ST_N * ST_ROW + 8, AL_L, "Settings are saved when you leave this screen.", C_DIM, 255);
}

/* ===== Sources: MangaDex browse + title page (O-2) ===== */
static const char *const src_tabs[3] = { "Popular", "Latest", "Search" };

static const MdJob *qj(int i) { return (const MdJob *)&md_q[i]; }
static int on_qstate(const char *id) {   /* queue state of a chapter, -1 = not queued */
    for (int i = 0; i < md_nq; i++) if (!strcmp(qj(i)->c.id, id)) return qj(i)->state;
    return -1;
}
static int src_blank(void) { return src_tab == 2 && !src_q[0]; }
static int src_pages(void) {   /* MangaDex stops at 10,000 results */
    int t = md_total > 10000 ? 10000 : md_total;
    return t > 0 ? (t + MD_PAGE - 1) / MD_PAGE : 1;
}

static void src_free_thumbs(void) {
    for (int i = 0; i < MD_PAGE; i++) {
        if (src_tex[i]) SDL_DestroyTexture(src_tex[i]);
        src_tex[i] = NULL; src_tex_st[i] = 0;
    }
    src_ready = 0;
}
static void src_load(void) {
    src_free_thumbs();
    src_sel = 0; src_focus = 0; src_btn = 1; src_scroll = src_target = 0;
    md_browse(src_tab == 0 ? MD_POPULAR : src_tab == 1 ? MD_LATEST : MD_SEARCH, src_q, src_off);
}
static void src_set_tab(int t) {
    src_tab = t; src_off = 0;
    if (src_blank()) { src_free_thumbs(); src_sel = 0; src_focus = 0; src_scroll = src_target = 0; }
    else src_load();
}
static void src_page(int d) {
    int p = src_off / MD_PAGE + d;
    if (p < 0 || p >= src_pages()) return;
    src_off = p * MD_PAGE;
    src_load();
}
static void src_prep(void) {   /* card names, once per result page */
    if (src_ready || md_res_state != MD_READY) return;
    for (int i = 0; i < md_nres; i++)
        fit_text(F_SMALLB, md_res[i].title, GCARD_W + 4, src_name[i], sizeof src_name[i]);
    src_ready = 1;
}
static void src_thumbs(void) {   /* one cover texture per frame */
    if (md_res_state != MD_READY) return;
    for (int i = 0; i < md_nres; i++) {
        if (src_tex_st[i]) continue;
        if (md_res[i].thumb_ok == 1) {
            src_tex[i] = load_image_fit(md_res[i].thumb, 140, 210, 12);
            src_tex_st[i] = src_tex[i] ? 1 : 2;
            return;
        }
        if (md_res[i].thumb_ok < 0) src_tex_st[i] = 2;
    }
}

/* on-screen keyboard (SDL opens the Vita IME) */
static void kb_open(void) {
    if (kb_on) return;
    if (!SDL_HasScreenKeyboardSupport()) { toast("No on-screen keyboard"); return; }
    kb_buf[0] = 0; kb_on = 1; kb_seen = 0;
    SDL_StartTextInput();
}
static void kb_event(SDL_Event *ev) {
    if (kb_on && ev->type == SDL_TEXTINPUT)
        strncat(kb_buf, ev->text.text, sizeof kb_buf - strlen(kb_buf) - 1);
}
static void kb_update(void) {
    if (!kb_on) return;
    if (SDL_IsScreenKeyboardShown(win)) { kb_seen = 1; return; }
    kb_on = 0;
    SDL_StopTextInput();
    char *s = kb_buf;
    while (*s == ' ') s++;
    int l = (int)strlen(s);
    while (l > 0 && s[l - 1] == ' ') s[--l] = 0;
    if (!l) { if (!kb_seen) toast("The keyboard didn't open"); return; }
    snprintf(src_q, sizeof src_q, "%s", s);
    char t[120];
    fit_text(F_H2, src_q, 560, t, sizeof t);
    snprintf(src_qfit, sizeof src_qfit, "\"%s\"", t);
    src_tab = 2; src_off = 0;
    src_load();
}

/* ----- online title page ----- */
static void online_open(int i) {
    if (i < 0 || i >= md_nres) return;
    if (fetch_ti >= 0) { fetch_pick(i); return; }
    const MdManga *m = (const MdManga *)&md_res[i];
    md_open(i);
    in_online = 1; on_ix = i; on_focus = 0; on_btn = 0; on_sel = 0; on_mark = -1;
    on_scroll = on_target = 0;
    memset(on_fit, 0, sizeof on_fit);
    fit_text(F_HERO, m->title, SCREEN_W - 260, on_title, sizeof on_title);
    on_meta[0] = 0;
    add_part(on_meta, sizeof on_meta, m->webtoon ? "WEBTOON" : "MANGA");
    if (m->status[0]) {
        char st[32]; snprintf(st, sizeof st, "%s", m->status);
        st[0] = (char)toupper((unsigned char)st[0]);
        add_part(on_meta, sizeof on_meta, st);
    }
    if (m->year) { char y[12]; snprintf(y, sizeof y, "%d", m->year); add_part(on_meta, sizeof on_meta, y); }
    if (m->author[0]) { char a[100]; snprintf(a, sizeof a, "by %s", m->author); add_part(on_meta, sizeof on_meta, a); }
    genres_fmt(m->genres, on_genres, sizeof on_genres);
    wtex_make(&on_syn, F_BODY, m->desc[0] ? m->desc : "No description on MangaDex.", 690, 3);
}
static void online_close(void) { wtex_free(&on_syn); in_online = 0; on_mark = -1; }

static void sources_open(void) {
    in_sources = 1; drawer_open = 0;
    if (in_details) close_details();
    if (!src_seen) { src_seen = 1; src_tab = 0; src_off = 0; src_load(); }
}
static void sources_close(void) {
    if (in_online) online_close();
    in_sources = 0;
    if (fetch_ti >= 0) { fetch_ti = -1; src_seen = 0; }   /* Sources reloads Popular next time */
}

static void on_prep(int i) {   /* row texts, made the first time a row is drawn */
    if (on_fit[i]) return;
    const MdChapter *c = (const MdChapter *)&md_ch[i];
    if (c->num[0]) snprintf(on_lab[i], sizeof on_lab[i], "Chapter %s", c->num);
    else snprintf(on_lab[i], sizeof on_lab[i], "Oneshot");
    char s[300] = "", pg[24];
    if (c->vol[0]) snprintf(s, sizeof s, "Vol. %s", c->vol);
    add_part(s, sizeof s, c->title);
    add_part(s, sizeof s, c->group);
    snprintf(pg, sizeof pg, "%d pages", c->pages);
    add_part(s, sizeof s, pg);
    fit_text(F_SMALL, s, 560, on_sub[i], sizeof on_sub[i]);
    on_fit[i] = 1;
}

static void on_queue(int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    int want = 0;
    for (int i = a; i <= b; i++)
        if (!md_ch[i].have && on_qstate(((const MdChapter *)&md_ch[i])->id) < 0) want++;
    int n = md_queue_range(a, b);
    if (n < want) toast("Queued %d of %d (the queue holds %d)", n, want, MD_Q_MAX);
    else if (n > 0) toast(n == 1 ? "Queued %d chapter. Keep aniread open" : "Queued %d chapters. Keep aniread open", n);
    else toast("Nothing new to download");
}

static void on_get(int i) {
    on_prep(i);
    const MdChapter *c = (const MdChapter *)&md_ch[i];
    if (c->have) { toast("Already downloaded: it's in your Manga"); return; }
    int st = on_qstate(c->id), r = md_queue(i);
    if (r < 0) toast("Download queue is full (%d)", MD_Q_MAX);
    else if (st == DQ_FAIL) toast("Retrying %s", on_lab[i]);
    else if (r == 0) toast("Already in the download queue");
    else toast("Queued %s. Keep aniread open", on_lab[i]);
}

static void online_retarget(int n) {
    float maxs = n > 0 ? D_LIST_Y + n * D_ROW_EP - (SCREEN_H - 40) : 0;
    if (maxs < 0) maxs = 0;
    if (on_focus == 0 || n == 0) { on_target = 0; return; }
    int top = D_LIST_Y + on_sel * D_ROW_EP;
    if (top - on_target < BAR_H + 16) on_target = top - BAR_H - 16;
    if (top + D_ROW_EP - on_target > SCREEN_H - 40) on_target = top + D_ROW_EP - (SCREEN_H - 40);
    on_target = clampf(on_target, 0, maxs);
}

static void update_online(float dt) {
    src_thumbs();
    int up = n_up(dt), down = n_down(dt), left = n_left(dt), right = n_right(dt), moved = 0;
    if (pressed(SCE_CTRL_CIRCLE)) { online_close(); return; }
    int n = md_ch_state == MD_READY ? md_nch : 0;
    if (on_sel > n - 1) on_sel = n > 0 ? n - 1 : 0;
    if (n == 0) on_focus = 0;
    if (on_focus == 0) {
        if (left && on_btn > 0) on_btn--;
        if (right && on_btn < 1) on_btn++;
        if (down && n > 0) { on_focus = 1; moved = 1; }
        if (pressed(SCE_CTRL_CROSS)) {
            if (on_btn == 1) { online_close(); return; }
            if (md_ch_state == MD_ERR) md_open(on_ix);
            else if (n > 0) on_queue(0, n - 1);
        }
    } else {
        if (up) { if (on_sel == 0) on_focus = 0; else on_sel--; moved = 1; }
        if (down && on_sel < n - 1) { on_sel++; moved = 1; }
        if (left) { on_sel = on_sel > 10 ? on_sel - 10 : 0; moved = 1; }
        if (right) { on_sel = on_sel + 10 < n ? on_sel + 10 : n - 1; moved = 1; }
        if (pressed(SCE_CTRL_CROSS)) on_get(on_sel);
        if (pressed(SCE_CTRL_SQUARE)) {
            if (on_mark < 0) { on_mark = on_sel; toast("Range start set. Press Square on the last chapter"); }
            else { on_queue(on_mark, on_sel); on_mark = -1; }
        }
        if (pressed(SCE_CTRL_TRIANGLE) && on_mark >= 0) { on_mark = -1; toast("Range cancelled"); }
    }
    float maxs = n > 0 ? D_LIST_Y + n * D_ROW_EP - (SCREEN_H - 40) : 0;
    if (maxs < 0) maxs = 0;
    if (drag_dy != 0) {
        on_scroll = clampf(on_scroll + drag_dy, 0, maxs);
        on_target = on_scroll;
    } else {
        if (moved) online_retarget(n);
        on_scroll += (on_target - on_scroll) * lerpk(dt);
    }
}

static void draw_online(void) {
    SDL_Texture *ct = src_tex[on_ix];
    int oy = -(int)on_scroll;
    fill(0, 0, SCREEN_W, SCREEN_H, C_BG, 255);
    if (ct) {
        int sh = 140 * 410 / SCREEN_W;
        SDL_Rect s = { 0, 84 - sh / 2, 140, sh }, d = { 0, oy, SCREEN_W, 410 };
        tex_col(ct, 85, 85, 93, 255);
        SDL_RenderCopy(ren, ct, &s, &d);
        tex_col(ct, 255, 255, 255, 255);
    }
    grad_h(0, oy, 640, 410, C_BG, 235, 0);
    grad_v(0, oy + 250, SCREEN_W, 160, C_BG, 0, 255);

    int px = 40, py = 84 + oy, pw = 160, ph = 240;
    rrect(px + 4, py + 8, pw, ph, 12, 0, 0, 0, 120);
    if (ct) { SDL_Rect d = { px, py, pw, ph }; SDL_RenderCopy(ren, ct, NULL, &d); }
    else {
        char fc[8]; first_char(on_title, fc);
        rrect(px, py, pw, ph, 12, C_PANEL, 255);
        textc(F_HERO, px + pw / 2, py + ph / 2, AL_C, fc, C_DIM, 255);
    }

    int ix = 228, err = md_ch_state == MD_ERR;
    char meta[300];
    snprintf(meta, sizeof meta, "%s", on_meta);
    if (md_ch_state == MD_READY) {
        char t[32];
        snprintf(t, sizeof t, md_nch == 1 ? "%d CHAPTER" : "%d CHAPTERS", md_nch);
        add_part(meta, sizeof meta, t);
    }
    text(F_HERO, ix, 80 + oy, AL_L, on_title, C_TEXT, 255);
    text(F_BODY, ix, 136 + oy, AL_L, meta, C_SOFT, 255);
    if (on_genres[0]) textc(F_BODY, ix, 182 + oy, AL_L, on_genres, C_DIM, 255);
    wtex_draw(&on_syn, ix, 210 + oy, C_DIM, 255);
    int by = 296 + oy;
    button(ix, by, 236, 48, 1, err ? IC_REFRESH : IC_DOWN, err ? "Try again" : "Download all", on_focus == 0 && on_btn == 0, 255);
    button(ix + 254, by, 130, 48, 0, IC_LEFT, "Back", on_focus == 0 && on_btn == 1, 255);
    hit(ix, by, 236, 48, H_ON_ALL, 0, 1);
    hit(ix + 254, by, 130, 48, H_ON_BACK, 0, 1);

    if (md_ch_state != MD_READY || md_nch == 0) {
        const char *msg = md_ch_state == MD_BUSY ? "Loading chapters..."
                        : err ? (md_err[0] ? md_err : "Couldn't load the chapter list")
                        : "No English chapters to download on MangaDex";
        rrect(40, 400 + oy, SCREEN_W - 80, 90, 16, C_PANEL, 255);
        textc(F_BODY, SCREEN_W / 2, 445 + oy, AL_C, msg, C_DIM, 255);
        return;
    }
    sec_head(40, 380 + oy, "MANGADEX  \xE2\x80\xA2  ENGLISH", "Chapters");
    if (on_mark >= 0) pill_btn(SCREEN_W - 40, 414 + oy, "RANGE: PICK THE LAST CHAPTER", 0);

    int n = md_nch, first = (BAR_H - (D_LIST_Y + oy)) / D_ROW_EP - 1;
    if (first < 0) first = 0;
    int ra = on_mark < on_sel ? on_mark : on_sel, rb = on_mark < on_sel ? on_sel : on_mark;
    for (int i = first; i < n; i++) {
        int y = D_LIST_Y + i * D_ROW_EP + oy;
        if (y > SCREEN_H) break;
        if (y + D_ROW_EP < BAR_H) continue;
        on_prep(i);
        const MdChapter *c = (const MdChapter *)&md_ch[i];
        int sel = (on_focus == 1 && i == on_sel), inr = on_mark >= 0 && i >= ra && i <= rb;
        if (inr && !sel) rrect(40, y, SCREEN_W - 80, D_ROW_EP - 8, 14, C_TINT, 255);
        else rrect(40, y, SCREEN_W - 80, D_ROW_EP - 8, 14, sel ? 42 : 28, sel ? 42 : 28, sel ? 56 : 36, 255);
        if (sel) rrect_line(40, y, SCREEN_W - 80, D_ROW_EP - 8, 14, 2, C_ACCENT, 255);
        rrect(52, y + 9, 56, 40, 10, 16, 16, 22, 255);
        const char *num = c->num[0] ? c->num : "-";
        if (sel) textc(F_BODYB, 80, y + 29, AL_C, num, C_ACCLT, 255);
        else     textc(F_BODYB, 80, y + 29, AL_C, num, C_SOFT, 255);
        text(F_BODYB, 124, y + 7, AL_L, on_lab[i], C_TEXT, 255);
        text(F_SMALL, 124, y + 32, AL_L, on_sub[i], C_DIM, 255);

        int st = c->have ? -2 : on_qstate(c->id), rx = SCREEN_W - 74, ry = y + 29;
        if (st == -2) {
            disc(rx, ry, 14, C_TINT, 255);
            gc(C_ACCLT, 255); check_mark(rx, ry, 0.8f);
        } else if (st == DQ_ACTIVE) {
            char t[24];
            snprintf(t, sizeof t, "%d / %d", md_dl_page, md_dl_pages);
            textc(F_SMALLB, rx + 18, ry - 6, AL_R, t, C_ACCLT, 255);
            int bw = 70, fw = md_dl_pages > 0 ? bw * md_dl_page / md_dl_pages : 0;
            rrect(rx + 18 - bw, ry + 8, bw, 4, 2, 255, 255, 255, 40);
            if (fw >= 4) rrect(rx + 18 - bw, ry + 8, fw, 4, 2, C_ACCENT, 255);
        } else if (st == DQ_WAIT) {
            textc(F_SMALLB, rx + 18, ry, AL_R, "QUEUED", C_SOFT, 255);
        } else if (st == DQ_FAIL) {
            textc(F_SMALLB, rx + 18, ry, AL_R, "FAILED", 240, 100, 100, 255);
        } else if (sel) {
            disc(rx, ry, 17, C_ACCENT, 255);
            gc(C_TEXT, 255); icon(IC_DOWN, rx, ry, 0.7f, 108, 99, 255);
        }
        hit(40, y, SCREEN_W - 80, D_ROW_EP - 8, H_ON_CH, i, 1);
    }
}

/* ----- browse grid ----- */
static void update_sources(float dt) {
    src_prep();
    src_thumbs();
    src_tab_x += (src_tab * 100 - src_tab_x) * lerpk(dt);
    int up = n_up(dt), down = n_down(dt), left = n_left(dt), right = n_right(dt), moved = 0;
    if (pressed(SCE_CTRL_CIRCLE)) { sources_close(); return; }
    if (fetch_ti >= 0 && (pressed(SCE_CTRL_LTRIGGER) || pressed(SCE_CTRL_RTRIGGER))) return;
    if (pressed(SCE_CTRL_LTRIGGER)) { src_set_tab((src_tab + 2) % 3); return; }
    if (pressed(SCE_CTRL_RTRIGGER)) { src_set_tab((src_tab + 1) % 3); return; }
    if (pressed(SCE_CTRL_SQUARE)) { kb_open(); return; }
    if (src_blank()) { if (pressed(SCE_CTRL_CROSS)) kb_open(); return; }
    if (md_res_state == MD_ERR && pressed(SCE_CTRL_CROSS)) { src_load(); return; }
    int n = md_res_state == MD_READY ? md_nres : 0, pages = src_pages();
    if (n == 0) src_focus = 0;
    if (n > 0 && src_focus == 0) {
        if (left && src_sel > 0) { src_sel--; moved = 1; }
        if (right && src_sel < n - 1) { src_sel++; moved = 1; }
        if (up && src_sel - GCOLS >= 0) { src_sel -= GCOLS; moved = 1; }
        if (down) {
            if (src_sel + GCOLS < n) src_sel += GCOLS;
            else if (src_sel / GCOLS < (n - 1) / GCOLS) src_sel = n - 1;
            else if (pages > 1) src_focus = 1;
            moved = 1;
        }
        if (pressed(SCE_CTRL_CROSS)) { online_open(src_sel); return; }
    } else if (n > 0) {
        if (left) src_btn = 0;
        if (right) src_btn = 1;
        if (up) { src_focus = 0; moved = 1; }
        if (pressed(SCE_CTRL_CROSS)) { src_page(src_btn ? 1 : -1); return; }
    }
    int nr = (n + GCOLS - 1) / GCOLS;
    float maxs = GRID_Y + nr * GROW_H + (pages > 1 ? 100 : 0) - (SCREEN_H - 10);
    if (maxs < 0) maxs = 0;
    if (drag_dy != 0) {
        src_scroll = clampf(src_scroll + drag_dy, 0, maxs);
        src_target = src_scroll;
    } else {
        if (moved) {
            if (src_focus == 1) src_target = maxs;
            else {
                int top = GRID_Y + (src_sel / GCOLS) * GROW_H;
                if (src_sel < GCOLS) src_target = 0;
                else if (top - src_target < BAR_H + 20) src_target = top - BAR_H - 20;
                if (top + GROW_H - src_target > SCREEN_H - 10) src_target = top + GROW_H - (SCREEN_H - 10);
            }
            src_target = clampf(src_target, 0, maxs);
        }
        src_scroll += (src_target - src_scroll) * lerpk(dt);
    }
}

static void draw_src_card(int i, int x, int y, int sel) {
    int w = GCARD_W, h = GCARD_H;
    if (src_tex[i]) {
        SDL_Rect d = { x, y, w, h };
        tex_col(src_tex[i], 255, 255, 255, 255);
        SDL_RenderCopy(ren, src_tex[i], NULL, &d);
    } else {
        char fc[8]; first_char(src_name[i][0] ? src_name[i] : "?", fc);
        rrect(x, y, w, h, 12, C_PANEL, 255);
        textc(F_H2, x + w / 2, y + h / 2, AL_C, fc, C_DIM, 255);
    }
    const char *b = md_res[i].webtoon ? "WEBTOON" : "MANGA";
    int bw = text_w(F_SMALLB, b) + 16;
    rrect(x + 8, y + 8, bw, 26, 9, 18, 18, 24, 215);
    textc(F_SMALLB, x + 8 + bw / 2, y + 21, AL_C, b, C_TEXT, 255);
    if (sel) rrect_line(x - 5, y - 5, w + 10, h + 10, 15, 3, C_ACCENT, 255);
    if (sel) text(F_SMALLB, x, y + h + 10, AL_L, src_name[i], C_ACCLT, 255);
    else     text(F_SMALLB, x, y + h + 10, AL_L, src_name[i], C_TEXT, 255);
}

static void src_panel(int y, const char *big, const char *small, const char *act) {
    const int x0 = (SCREEN_W - (GCOLS * GCARD_W + (GCOLS - 1) * GGAP)) / 2;
    rrect(x0, y, SCREEN_W - 2 * x0, 150, 16, C_PANEL, 255);
    textc(F_BODYB, SCREEN_W / 2, y + 44, AL_C, big, C_TEXT, 255);
    if (small) textc(F_SMALL, SCREEN_W / 2, y + 78, AL_C, small, C_DIM, 255);
    if (act) textc(F_SMALLB, SCREEN_W / 2, y + 110, AL_C, act, C_ACCLT, 255);
}

static void draw_sources(void) {
    int oy = -(int)src_scroll;
    const int x0 = (SCREEN_W - (GCOLS * GCARD_W + (GCOLS - 1) * GGAP)) / 2;
    fill(0, 0, SCREEN_W, SCREEN_H, C_BG, 255);
    sec_head(x0, 78 + oy, fetch_ti >= 0 ? "FETCH INFO  \xE2\x80\xA2  PICK THE MATCHING TITLE" : "MANGADEX  \xE2\x80\xA2  ENGLISH",
             src_tab == 2 && src_q[0] ? src_qfit : src_tabs[src_tab]);
    if (src_blank()) {
        src_panel(GRID_Y + oy, "Search MangaDex by title", "English, Safe and Suggestive titles only", "Press Square or X to open the keyboard");
        gc(C_ACCLT, 255); icon(IC_SEARCH, x0 + 50, GRID_Y + 75 + oy, 1.6f, C_PANEL);
        return;
    }
    if (md_res_state == MD_BUSY) {
        dango_icon(SCREEN_W / 2, 250, 1.6f);
        textc(F_SUB, SCREEN_W / 2, 310, AL_C, "Loading from MangaDex...", C_SOFT, 255);
        return;
    }
    if (md_res_state == MD_ERR) {
        src_panel(GRID_Y + oy, "Couldn't load MangaDex", md_err[0] ? md_err : "Check your Wi-Fi", "Press X to try again");
        return;
    }
    if (md_res_state != MD_READY) return;
    if (md_nres == 0) { src_panel(GRID_Y + oy, "No results", "Try another title or spelling", "Press Square to search again"); return; }

    char pg[48];
    snprintf(pg, sizeof pg, "PAGE %d / %d", src_off / MD_PAGE + 1, src_pages());
    pill_btn(SCREEN_W - x0, 108 + oy, pg, 0);
    for (int i = 0; i < md_nres; i++) {
        int x = x0 + (i % GCOLS) * (GCARD_W + GGAP);
        int y = GRID_Y + (i / GCOLS) * GROW_H + oy;
        if (y + GROW_H < BAR_H || y > SCREEN_H) continue;
        draw_src_card(i, x, y, src_focus == 0 && src_sel == i);
        hit(x, y, GCARD_W, GCARD_H + 30, H_SRC_CARD, i, 1);
    }
    if (src_pages() > 1) {
        int nr = (md_nres + GCOLS - 1) / GCOLS;
        int fy = GRID_Y + nr * GROW_H + 8 + oy, bw = 200, bx = SCREEN_W / 2 - bw - 9;
        button(bx, fy, bw, 48, 0, IC_LEFT, "Previous", src_focus == 1 && src_btn == 0, 255);
        button(bx + bw + 18, fy, bw, 48, 1, IC_RIGHT, "Next page", src_focus == 1 && src_btn == 1, 255);
        hit(bx, fy, bw, 48, H_SRC_PREV, 0, 1);
        hit(bx + bw + 18, fy, bw, 48, H_SRC_NEXT, 0, 1);
    }
}

static void draw_src_bar(void) {
    fill(0, 0, SCREEN_W, BAR_H, C_BAR, 240);
    fill(0, BAR_H - 1, SCREEN_W, 1, C_LINE, 255);
    disc(38, 30, 22, C_PANEL, 255);
    gc(C_TEXT, 255); icon(IC_LEFT, 37, 30, 0.85f, C_PANEL);
    hit(14, 6, 48, 48, H_SRC_BACK, 0, 0);
    textc(F_SUB, 74, 30, AL_L, fetch_ti >= 0 ? "Fetch info" : in_online ? "MangaDex" : "Sources", C_TEXT, 255);
    if (in_online) return;
    if (fetch_ti < 0) {
        const int nx = (SCREEN_W - 308) / 2;
        rrect(nx, 8, 308, 44, 22, C_PANEL, 255);
        rrect(nx + 4 + (int)src_tab_x, 12, 100, 36, 18, C_PANEL2, 255);
        for (int i = 0; i < 3; i++) {
            int cx = nx + 4 + i * 100 + 50;
            if (i == src_tab) textc(F_BODYB, cx, 30, AL_C, src_tabs[i], C_TEXT, 255);
            else              textc(F_BODYB, cx, 30, AL_C, src_tabs[i], C_DIM, 255);
            hit(nx + 4 + i * 100, 8, 100, 44, H_SRC_TAB, i, 0);
        }
    }
    disc(SCREEN_W - 40, 30, 22, C_PANEL, 255);
    gc(C_TEXT, 255); icon(IC_SEARCH, SCREEN_W - 40, 30, 0.85f, C_PANEL);
    hit(SCREEN_W - 64, 6, 48, 48, H_SRC_SEARCH, 0, 0);
}

/* ----- downloads: library refresh, fail notice, progress pill ----- */
static void lib_refresh(void) {   /* a chapter finished: rescan without losing your place */
    char keep[512] = "", fk[512] = "";
    int ds = d_sel, df = d_focus;
    float dsc = d_scroll;
    if (in_details) snprintf(keep, sizeof keep, "%s", titles[d_ti].dir);
    if (ntitles > 0) snprintf(fk, sizeof fk, "%s", titles[feat].dir);
    if (in_details) close_details();
    scan_library();
    prog_gen++;
    int nf = 0;
    for (int i = 0; i < ntitles; i++) if (fk[0] && !strcmp(titles[i].dir, fk)) { nf = i; break; }
    set_feature(nf);
    if (g_sel[0] >= nmanga) g_sel[0] = nmanga > 0 ? nmanga - 1 : 0;
    if (g_sel[1] >= nnovel) g_sel[1] = nnovel > 0 ? nnovel - 1 : 0;
    for (int i = 0; keep[0] && i < ntitles; i++) {
        if (strcmp(titles[i].dir, keep)) continue;
        open_details(i);
        Title *tl = &titles[i];
        d_sel = ds < tl->nchs ? ds : (tl->nchs > 0 ? tl->nchs - 1 : 0);
        d_focus = df && tl->nchs > 0;
        d_scroll = d_target = dsc;
        break;
    }
}

static void dl_update(void) {
    int fails = 0;
    for (int i = 0; i < md_nq; i++) if (qj(i)->state == DQ_FAIL) fails++;
    if (fails > dl_fails) toast("A download failed. Open the title and press X on it to retry");
    dl_fails = fails;
}

static void draw_dl(void) {
    int wait = 0;
    for (int i = 0; i < md_nq; i++) if (qj(i)->state == DQ_WAIT) wait++;
    if (!md_dl_on && wait == 0) return;
    const char *nm = "Starting download...";
    char pg[40] = "", more[24] = "";
    if (md_dl_on) {
        if (strcmp(md_dl_name, dl_last)) {
            snprintf(dl_last, sizeof dl_last, "%s", md_dl_name);
            fit_text(F_SMALLB, dl_last, 230, dl_fit, sizeof dl_fit);
        }
        nm = dl_fit;
        snprintf(pg, sizeof pg, "%d / %d", md_dl_page, md_dl_pages);
    }
    if (wait > 0) snprintf(more, sizeof more, "+%d", wait);
    int nw = text_w(F_SMALLB, nm), pw = pg[0] ? text_w(F_SMALLB, pg) + 10 : 0, mw = more[0] ? text_w(F_SMALL, more) + 10 : 0;
    int w = 36 + nw + 12 + pw + mw + 8, x = SCREEN_W - 16 - w, y = HINT_CY - 15;
    rrect(x, y, w, 30, 15, C_PANEL2, 245);
    rrect_line(x, y, w, 30, 15, 1, C_LINE, 255);
    gc(C_ACCLT, 255); icon(IC_DOWN, x + 20, HINT_CY, 0.62f, C_PANEL2);
    int tx = x + 36;
    tx += textc(F_SMALLB, tx, HINT_CY, AL_L, nm, C_TEXT, 255) + 12;
    if (pg[0]) tx += textc(F_SMALLB, tx, HINT_CY, AL_L, pg, C_ACCLT, 255) + 10;
    if (more[0]) textc(F_SMALL, tx, HINT_CY, AL_L, more, C_DIM, 255);
    if (md_dl_on && md_dl_pages > 0) {
        int bw = w - 30, fw = bw * md_dl_page / md_dl_pages;
        if (fw >= 4) rrect(x + 15, y + 26, fw, 3, 1, C_ACCENT, 255);
    }
}

/* ===== Fetch info: match a local title on MangaDex, save cover + info.txt ===== */
static void fetch_query(const char *s, char *o, int n) {   /* drop [tags] (years) and _ . from folder names */
    const char *s0 = s;
    int k = 0, depth = 0;
    for (; *s && k < n - 1; s++) {
        if (*s == '[' || *s == '(' || *s == '{') { depth++; continue; }
        if (*s == ']' || *s == ')' || *s == '}') { if (depth) depth--; continue; }
        if (depth) continue;
        char c = (*s == '_' || *s == '.') ? ' ' : *s;
        if (c == ' ' && (k == 0 || o[k - 1] == ' ')) continue;
        o[k++] = c;
    }
    while (k > 0 && o[k - 1] == ' ') k--;
    o[k] = 0;
    if (!k) snprintf(o, n, "%s", s0);
}

static void fetch_open(int ti) {
    if (ti < 0 || ti >= ntitles) return;
    Title *tl = &titles[ti];
    if (!strcmp(tl->dir, NOVEL_ROOT)) { toast("Move this novel into its own folder first"); return; }
    if (fetch_busy) { toast("Still saving the last title's info..."); return; }
    if (in_online) online_close();
    fetch_ti = ti;
    in_sources = 1; drawer_open = 0; src_seen = 1;
    fetch_query(tl->name, src_q, sizeof src_q);
    char t[120];
    fit_text(F_H2, src_q, 560, t, sizeof t);
    snprintf(src_qfit, sizeof src_qfit, "\"%s\"", t);
    src_tab = 2; src_off = 0;
    src_load();
}

static void fetch_pick(int i) {
    if (fetch_ti < 0 || i < 0 || i >= md_nres) return;
    Title *tl = &titles[fetch_ti];
    md_fetch_meta(i, tl->dir, tl->kind == K_NOVEL);
    fetch_busy = 1;
    sources_close();          /* back to the Details page */
    toast("Saving cover and info...");
}

static void fetch_poll(void) {   /* main loop: the background save finished */
    if (!fetch_busy || md_meta_state == 0) return;
    int st = md_meta_state;
    md_meta_state = 0; fetch_busy = 0;
    if (st == 3) { toast("Couldn't save the info (check md_log.txt)"); return; }
    lib_refresh();
    toast(st == 1 ? "Cover and info saved" : "Info saved, but the cover didn't download");
}

/* ===== Taps ===== */
static void handle_tap(void) {
    Hit *h = tap_hit();
    if (!h) return;
    int r, i;
    switch (h->id) {
    case H_MENU:     open_drawer(); break;
    case H_TAB:      go_page(h->arg); break;
    case H_REFRESH:  do_rescan(); break;
    case H_SETTINGS: settings_open(); break;
    case H_HPLAY:
        h_focus = 0; h_btn = 0;
        if (ntitles > 0) read_chapter(feat, title_resume_ch(&titles[feat]));
        break;
    case H_HDETAILS: h_focus = 0; h_btn = 1; if (ntitles > 0) open_details(feat); break;
    case H_HPREV:    step_feature(-1); break;
    case H_HNEXT:    step_feature(1); break;
    case H_ROWCARD:
        r = h->arg >> 16; i = h->arg & 0xFFFF;
        if (r < 0 || r > HR_NOVEL) break;
        h_focus = r + 1; row_sel[r] = i;
        if (r == HR_CW) cw_play(i);
        else open_details((r == HR_NOVEL ? novel_ix : manga_ix)[i]);
        break;
    case H_SEEALL:   go_page(h->arg); break;
    case H_GRIDCARD:
        g_sel[page - 1] = h->arg;
        open_details((page == PG_NOVELS ? novel_ix : manga_ix)[h->arg]);
        break;
    case H_DPLAY:    if (in_details) read_chapter(d_ti, title_resume_ch(&titles[d_ti])); break;
    case H_DBACK:    if (in_details) close_details(); break;
    case H_CH:
        if (in_details && h->arg < titles[d_ti].nchs) {
            d_focus = 1; d_sel = h->arg;
            read_chapter(d_ti, h->arg);
        }
        break;
    case H_DRAWER_ITEM: drawer_sel = h->arg; drawer_pick(h->arg); break;
    case H_DRAWER_OUT:  drawer_open = 0; break;
    case H_ST_BACK: if (in_settings) settings_close(); break;
    case H_ST_ROW:
        if (in_settings && h->arg >= 0 && h->arg < ST_N) {
            if (st_sel != h->arg) st_confirm = 0;
            st_sel = h->arg;
            st_activate(h->arg);
        }
        break;
    }
}

/* ===== Main ===== */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    sceIoMkdir(DATA_ROOT, 0777);
    sceIoMkdir(MANGA_ROOT, 0777);
    sceIoMkdir(NOVEL_ROOT, 0777);
    sceIoMkdir(IMPORT_ROOT, 0777);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { sceKernelExitProcess(0); return 0; }
    win = SDL_CreateWindow("aniread", 0, 0, SCREEN_W, SCREEN_H, 0);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
    TTF_Init();
    const char *bold = file_exists(FONT_BOLD) ? FONT_BOLD : FONT_REG;
    for (int i = 0; i < F_COUNT; i++) {
        fonts[i] = TTF_OpenFont(font_bold[i] ? bold : FONT_REG, font_px[i]);
        if (!fonts[i]) { sceKernelExitProcess(0); return 0; }   /* font missing from the vpk */
    }
    circ_tex = make_ring_tex(CIRC, CIRC / 2.0f, 0);
    dots_tex = make_dots();

    fill(0, 0, SCREEN_W, SCREEN_H, C_BG, 255);
    dango_icon(SCREEN_W / 2, 230, 2.0f);
    textc(F_SUB, SCREEN_W / 2, 300, AL_C, "Scanning library...", C_SOFT, 255);
    SDL_RenderPresent(ren);
    scan_library();
    prog_load();
    cfg_load();
    set_feature(0);
    md_init();
    rd_init();

    Uint32 last = SDL_GetTicks();
    for (;;) {
        Uint32 now = SDL_GetTicks();
        float dt = (now - last) / 1000.0f;
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        frame_no++;

        drag_dx = drag_dy = 0; tap_pending = 0;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (in_reader) rd_touch_event(&ev);
            else if (in_novel) nv_touch_event(&ev);
            handle_touch_event(&ev);
            kb_event(&ev);
        }
        read_input();
        kb_update();

        int reading = in_reader || in_novel;   /* before the updates: the tap that closes a reader isn't used twice */
        if (kb_on) { }                                   /* the keyboard has the buttons */
        else if (in_reader) update_reader(dt);
        else if (in_novel) update_novel(dt);
        else if (in_settings) update_settings(dt);
        else if (drawer_open) update_drawer(dt);
        else if (pressed(SCE_CTRL_START)) open_drawer();
        else if (in_sources) { if (in_online) update_online(dt); else update_sources(dt); }
        else if (in_details) update_details(dt);
        else if (pressed(SCE_CTRL_LTRIGGER)) go_page((page + 2) % 3);
        else if (pressed(SCE_CTRL_RTRIGGER)) go_page((page + 1) % 3);
        else if (pressed(SCE_CTRL_TRIANGLE) && !(page == PG_HOME && h_focus == HR_CW + 1)) do_rescan();
        else if (page == PG_HOME) update_home(dt);
        else update_grid(page - 1, dt);
        if (!kb_on && !reading) handle_tap();
        if (md_lib_changed && !in_reader && !in_novel) { md_lib_changed = 0; lib_refresh(); toast("Chapter downloaded: it's in your Manga now"); }
        dl_update();
        if (!in_reader && !in_novel) fetch_poll();

        if (toast_t > 0) toast_t -= dt;
        drawer_t = clampf(drawer_t + (drawer_open ? dt : -dt) * 5.0f, 0, 1);
        hero_a = clampf(hero_a + dt * 3.0f, 0, 1);
        tab_x += (page * 100 - tab_x) * lerpk(dt);
        if (!in_reader && !in_novel && !in_details && !in_settings && !in_sources && page == PG_HOME && !drawer_open && ntitles > 1) {
            feat_t += dt;
            if (feat_t > FEAT_TIME) step_feature(1);
        }

        cover_budget = 2;
        nhits = 0;
        if (in_reader) draw_reader();
        else if (in_novel) draw_novel();
        else {
            if (in_settings) draw_settings();
            else if (in_sources) { if (in_online) draw_online(); else draw_sources(); }
            else if (in_details) draw_details();
            else if (page == PG_HOME) draw_home();
            else draw_grid(page - 1);
            if (in_settings) { }
            else if (in_sources) draw_src_bar();
            else draw_topbar();
            draw_hints();
            draw_dl();
            draw_drawer();
            draw_toast();
        }
        SDL_RenderPresent(ren);
        memcpy(hits_prev, hits, nhits * sizeof(Hit));
        nhits_prev = nhits;
    }
    return 0;
}

/* ===== END OF FILE ===== */

const char vita_pad[4096] = {1};
