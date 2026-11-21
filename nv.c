/* nv.c - aniread novel loader (R-2a)
   EPUB (zip -> container.xml -> OPF spine -> XHTML) and TXT become one UTF-8 text.
   Paragraphs end with '\n'. Style codes are in nv.h. No SDL in this file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <zlib.h>
#include "nv.h"

#define NV_MAX_FILE  (48 * 1024 * 1024)   /* biggest single file we unpack or read */
#define NV_TXT_SPLIT 40000                /* long TXT sections are split about every 40 KB */

/* ===== Output buffer ===== */
typedef struct { char *p; int n, cap, fail; } Buf;

static int buf_room(Buf *b, int add) {
    if (b->fail) return 0;
    if (b->n + add + 1 <= b->cap) return 1;
    int nc = b->cap ? b->cap : 1 << 16;
    while (nc < b->n + add + 1) nc *= 2;
    char *np = realloc(b->p, nc);
    if (!np) { b->fail = 1; return 0; }
    b->p = np; b->cap = nc;
    return 1;
}
static void buf_put(Buf *b, const char *s, int n) {
    if (n <= 0 || !buf_room(b, n)) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void buf_c(Buf *b, char c) { buf_put(b, &c, 1); }

/* ===== UTF-8 ===== */
static int u8_enc(unsigned cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp < 0x110000) {
        o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    o[0] = '?';
    return 1;
}
static int u8_len(const unsigned char *p, const unsigned char *e) {   /* 0 = not valid UTF-8 */
    int n = p[0] < 0x80 ? 1 : (p[0] & 0xE0) == 0xC0 ? 2 : (p[0] & 0xF0) == 0xE0 ? 3 : (p[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (!n || p + n > e) return 0;
    for (int i = 1; i < n; i++) if ((p[i] & 0xC0) != 0x80) return 0;
    if (n == 2 && p[0] < 0xC2) return 0;
    return n;
}
static unsigned u8_dec(const unsigned char *p, int n) {
    if (n == 1) return p[0];
    if (n == 2) return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    if (n == 3) return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

/* ===== Text writer: collapses spaces, keeps paragraphs and bold/italic ===== */
typedef struct { char file[256], frag[96], title[96]; int off, fb; } TocT;   /* a Contents entry while loading */

typedef struct {
    Buf *o;
    int para, space;      /* bytes in this paragraph, a space is waiting */
    int bn, in;           /* open bold / italic tags */
    int ob, oi;           /* bold / italic switched on in the output */
    int head, center;     /* kind of paragraph */
    char *cap; int capn, capsz, capping, capsp;   /* first heading = section title */
    NvBook *b; const char *dir;                   /* pictures: the book and this file's folder */
    TocT *tt; int *fl, nfl, pend[16], npend;      /* Contents anchors in this file */
} Wr;

static void wr_resolve(Wr *w) {   /* anchors seen since the last text point here */
    for (int i = 0; i < w->npend; i++) w->tt[w->pend[i]].off = w->o->n;
    w->npend = 0;
}

static void wr_cp(Wr *w, unsigned cp) {
    if (cp < 0x20 || cp == 0x7F || cp == 0xAD || cp == 0xFEFF || (cp >= 0x200B && cp <= 0x200D) ||
        (cp >= 0xD800 && cp < 0xE000)) return;
    if (w->npend) wr_resolve(w);
    char u[4];
    int n = u8_enc(cp, u);
    if (!w->para) {
        if (w->head) buf_c(w->o, NV_HEAD);
        else if (w->center) buf_c(w->o, NV_CENTER);
    } else if (w->space) buf_c(w->o, ' ');
    int wb = w->bn > 0, wi = w->in > 0;
    if (wb != w->ob) { buf_c(w->o, wb ? NV_B_ON : NV_B_OFF); w->ob = wb; }
    if (wi != w->oi) { buf_c(w->o, wi ? NV_I_ON : NV_I_OFF); w->oi = wi; }
    buf_put(w->o, u, n);
    if (w->capping && w->cap && w->capn + n + 2 < w->capsz) {
        if (w->capn > 0 && (w->space || w->capsp)) w->cap[w->capn++] = ' ';
        memcpy(w->cap + w->capn, u, n);
        w->capn += n;
        w->cap[w->capn] = 0;
    }
    w->capsp = 0;
    w->para += n;
    w->space = 0;
}
static void wr_space(Wr *w) { if (w->para) w->space = 1; }
static void wr_break(Wr *w) {
    if (w->para) {
        if (w->ob) buf_c(w->o, NV_B_OFF);
        if (w->oi) buf_c(w->o, NV_I_OFF);
        buf_c(w->o, '\n');
    }
    w->ob = w->oi = 0; w->para = 0; w->space = 0; w->head = 0; w->center = 0;
}
static void wr_utf8(Wr *w, const char *s, int n) {
    const unsigned char *p = (const unsigned char *)s, *e = p + n;
    while (p < e) {
        if (*p == ' ' || *p == '\t') { wr_space(w); p++; continue; }
        int l = u8_len(p, e);
        if (!l) { p++; continue; }
        wr_cp(w, u8_dec(p, l));
        p += l;
    }
}

/* ===== Sections ===== */
static void sec_begin(NvBook *b, Buf *o) {
    if (b->nch > 0 && b->ch[b->nch - 1].off == o->n) { b->ch[b->nch - 1].title[0] = 0; return; }   /* still empty: reuse */
    if (b->nch >= NV_MAXCH) return;   /* full: the text joins the last section */
    b->ch[b->nch].off = o->n;
    b->ch[b->nch].title[0] = 0;
    b->nch++;
}
static void copy_title(char *d, int dn, const char *s, int n) {
    if (n > dn - 1) { n = dn - 1; while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--; }
    memcpy(d, s, n);
    d[n] = 0;
}

/* ===== XHTML ===== */
typedef struct { const char *n; unsigned cp; } Ent;
static const Ent ents[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
    { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "sbquo", 0x201A }, { "bdquo", 0x201E }, { "laquo", 0xAB },
    { "raquo", 0xBB }, { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "deg", 0xB0 },
    { "middot", 0xB7 }, { "bull", 0x2022 }, { "times", 0xD7 }, { "prime", 0x2032 }, { "eacute", 0xE9 },
    { "egrave", 0xE8 }, { "ecirc", 0xEA }, { "aacute", 0xE1 }, { "agrave", 0xE0 }, { "auml", 0xE4 },
    { "ouml", 0xF6 }, { "uuml", 0xFC }, { "iuml", 0xEF }, { "ccedil", 0xE7 }, { "szlig", 0xDF },
    { "thinsp", 0x2009 }, { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "shy", 0xAD }, { "zwnj", 0x200C },
    { "zwj", 0x200D }, { NULL, 0 } };

static unsigned ent_dec(const char *s, const char *e, int *used) {   /* s = just after '&' */
    const char *q = s;
    *used = 0;
    while (q < e && q - s < 12 && *q != ';') {
        if (!isalnum((unsigned char)*q) && *q != '#') return 0;
        q++;
    }
    if (q >= e || *q != ';' || q == s) return 0;
    int len = (int)(q - s);
    *used = len + 1;
    if (s[0] == '#') {
        unsigned long v = (len > 1 && (s[1] == 'x' || s[1] == 'X')) ? strtoul(s + 2, NULL, 16) : strtoul(s + 1, NULL, 10);
        return v > 0x10FFFF ? 0 : (unsigned)v;
    }
    for (int i = 0; ents[i].n; i++)
        if ((int)strlen(ents[i].n) == len && !strncmp(ents[i].n, s, len)) return ents[i].cp;
    return 0;
}

static const char *find_str(const char *p, const char *e, const char *s) {
    int n = (int)strlen(s);
    for (; e - p >= n; p++) if (*p == *s && !memcmp(p, s, n)) return p;
    return NULL;
}
static const char *tag_end(const char *q, const char *e) {   /* just past the '>' */
    char quote = 0;
    for (; q < e; q++) {
        if (quote) { if (*q == quote) quote = 0; }
        else if (*q == '"' || *q == '\'') quote = *q;
        else if (*q == '>') return q + 1;
    }
    return e;
}
static const char *skip_to_close(const char *p, const char *e, const char *name) {
    int nl = (int)strlen(name);
    for (; p < e - 1; p++) {
        if (p[0] != '<' || p[1] != '/') continue;
        const char *q = p + 2, *c = q;
        while (c < e && isalnum((unsigned char)*c)) c++;
        if (c < e && *c == ':') q = c + 1;
        if (e - q >= nl && !strncasecmp(q, name, nl) && (q + nl >= e || !isalnum((unsigned char)q[nl]))) return tag_end(q, e);
    }
    return e;
}
static int in_list(const char *n, const char *const *l) {
    for (; *l; l++) if (!strcmp(n, *l)) return 1;
    return 0;
}
static const char *const tag_block[] = { "p", "div", "li", "ul", "ol", "blockquote", "section", "article", "tr",
    "table", "dt", "dd", "dl", "pre", "figure", "figcaption", "header", "footer", "aside", "nav", "body", "main",
    "td", "th", "caption", "address", "center", NULL };
static const char *const tag_skip[] = { "head", "script", "style", "rt", "rp", "math", "title", "desc", NULL };
static const char *const tag_bold[] = { "b", "strong", NULL };
static const char *const tag_ital[] = { "i", "em", "cite", "var", "dfn", NULL };

static int xml_attr(const char *tag, const char *te, const char *name, char *out, int n);
static void path_join(char *out, int n, const char *dir, const char *href);

static void wr_anchor(Wr *w, const char *q, const char *te) {   /* an id="..." the Contents points at */
    char id[96];
    if (!xml_attr(q, te, "id", id, sizeof id) && !xml_attr(q, te, "name", id, sizeof id)) return;
    for (int i = 0; i < w->nfl; i++) {
        TocT *t = &w->tt[w->fl[i]];
        if (t->off >= 0 || strcmp(t->frag, id)) continue;
        if (w->npend < 16) w->pend[w->npend++] = w->fl[i];
        else t->off = w->o->n;
    }
}

static void wr_img(Wr *w, const char *q, const char *te) {   /* <img src> / <image xlink:href>: a paragraph of its own */
    char src[256], full[256], num[16];
    NvBook *b = w->b;
    if (!b || !w->dir) return;
    if (!xml_attr(q, te, "src", src, sizeof src) && !xml_attr(q, te, "xlink:href", src, sizeof src)) return;
    int sl = (int)strlen(src);
    if (!sl || !strncasecmp(src, "data:", 5) || (sl > 4 && !strcasecmp(src + sl - 4, ".svg"))) return;
    path_join(full, sizeof full, w->dir, src);
    if (!full[0]) return;
    int i = 0;
    while (i < b->nimg && strcmp(b->img[i], full)) i++;
    if (i == b->nimg) {
        if (b->nimg >= NV_MAXIMG) return;
        if (b->nimg == b->imgcap) {
            int nc = b->imgcap ? b->imgcap * 2 : 32;
            char (*ni)[256] = realloc(b->img, nc * sizeof *ni);
            if (!ni) return;
            b->img = ni; b->imgcap = nc;
        }
        snprintf(b->img[b->nimg++], sizeof b->img[0], "%s", full);
    }
    int h = w->head, ce = w->center;
    wr_break(w);
    if (w->npend) wr_resolve(w);
    int n = snprintf(num, sizeof num, "%c%d\n", NV_IMG, i);
    buf_put(w->o, num, n);
    w->head = h; w->center = ce;
}

static void html_text(const char *s, int n, Wr *w) {
    const char *p = s, *e = s + n;
    while (p < e) {
        unsigned char c = (unsigned char)*p;
        if (c == '<') {
            if (e - p >= 4 && !memcmp(p, "<!--", 4)) { const char *q = find_str(p + 4, e, "-->"); p = q ? q + 3 : e; continue; }
            const char *q = p + 1;
            int close = 0;
            if (q < e && *q == '/') { close = 1; q++; }
            if (q < e && (*q == '!' || *q == '?')) { p = tag_end(q, e); continue; }
            char name[16];
            int k = 0;
            while (q < e && (isalnum((unsigned char)*q) || *q == ':' || *q == '-' || *q == '_')) {
                if (*q == ':') k = 0;   /* drop a namespace prefix */
                else if (k < 15) name[k++] = (char)tolower((unsigned char)*q);
                q++;
            }
            name[k] = 0;
            const char *te = tag_end(q, e);
            int self = te - p >= 2 && te[-1] == '>' && te[-2] == '/';
            p = te;
            if (!k) continue;
            if (!close && w->nfl) wr_anchor(w, q, te);
            if (!close && (!strcmp(name, "img") || !strcmp(name, "image"))) { wr_img(w, q, te); continue; }
            if (!close && !self && in_list(name, tag_skip)) { p = skip_to_close(p, e, name); continue; }
            if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
                wr_break(w);
                w->capping = 0;
                if (!close && !self) { w->head = 1; if (w->cap && !w->cap[0]) w->capping = 1; }
                continue;
            }
            if (!strcmp(name, "br")) {
                int h = w->head, ce = w->center;
                wr_break(w);
                w->head = h; w->center = ce; w->capsp = 1;
                continue;
            }
            if (!strcmp(name, "hr")) {
                wr_break(w);
                if (!close) { w->center = 1; wr_utf8(w, "* * *", 5); wr_break(w); }
                continue;
            }
            if (in_list(name, tag_block)) { wr_break(w); continue; }
            if (!self && in_list(name, tag_bold)) { w->bn += close ? -1 : 1; if (w->bn < 0) w->bn = 0; continue; }
            if (!self && in_list(name, tag_ital)) { w->in += close ? -1 : 1; if (w->in < 0) w->in = 0; continue; }
            continue;
        }
        if (c == '&') {
            int used;
            unsigned cp = ent_dec(p + 1, e, &used);
            if (!used) { wr_cp(w, '&'); p++; continue; }
            p += 1 + used;
            if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') wr_space(w);
            else if (cp) wr_cp(w, cp);
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { wr_space(w); p++; continue; }
        int l = u8_len((const unsigned char *)p, (const unsigned char *)e);
        if (!l) { wr_cp(w, c); p++; continue; }   /* not UTF-8: use the byte as Latin-1 */
        wr_cp(w, u8_dec((const unsigned char *)p, l));
        p += l;
    }
    wr_break(w);
}

/* ===== ZIP (stored + deflate) ===== */
typedef struct { const char *name; unsigned method, csize, usize, off; } ZEnt;
typedef struct { FILE *f; ZEnt *e; int n; char *names; } Zip;

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

static void zip_close(Zip *z) {
    if (z->f) fclose(z->f);
    free(z->e);
    free(z->names);
    memset(z, 0, sizeof *z);
}

static int zip_open(Zip *z, const char *path) {
    memset(z, 0, sizeof *z);
    z->f = fopen(path, "rb");
    if (!z->f || fseek(z->f, 0, SEEK_END)) return 0;
    long size = ftell(z->f);
    if (size < 22) return 0;
    long tl = size < 65557 ? size : 65557;
    unsigned char *t = malloc(tl);
    if (!t) return 0;
    if (fseek(z->f, size - tl, SEEK_SET) || fread(t, 1, tl, z->f) != (size_t)tl) { free(t); return 0; }
    long eo = -1;
    for (long i = tl - 22; i >= 0; i--) if (rd32(t + i) == 0x06054b50) { eo = i; break; }
    if (eo < 0) { free(t); return 0; }
    unsigned cnt = rd16(t + eo + 10), cds = rd32(t + eo + 12), cdo = rd32(t + eo + 16);
    free(t);
    if (!cnt || !cds || (long)cdo + (long)cds > size) return 0;
    unsigned char *cd = malloc(cds);
    z->e = calloc(cnt, sizeof(ZEnt));
    z->names = malloc(cds);
    if (!cd || !z->e || !z->names) { free(cd); return 0; }
    if (fseek(z->f, (long)cdo, SEEK_SET) || fread(cd, 1, cds, z->f) != cds) { free(cd); return 0; }
    char *np = z->names;
    unsigned pos = 0;
    for (unsigned i = 0; i < cnt && pos + 46 <= cds; i++) {
        const unsigned char *h = cd + pos;
        if (rd32(h) != 0x02014b50) break;
        unsigned nl = rd16(h + 28), xl = rd16(h + 30), cl = rd16(h + 32);
        if (pos + 46 + nl > cds) break;
        ZEnt *e = &z->e[z->n++];
        e->method = rd16(h + 10); e->csize = rd32(h + 20); e->usize = rd32(h + 24); e->off = rd32(h + 42);
        memcpy(np, h + 46, nl);
        np[nl] = 0;
        e->name = np;
        np += nl + 1;
        pos += 46 + nl + xl + cl;
    }
    free(cd);
    return z->n > 0;
}

static ZEnt *zip_find(Zip *z, const char *name) {
    for (int i = 0; i < z->n; i++) if (!strcmp(z->e[i].name, name)) return &z->e[i];
    for (int i = 0; i < z->n; i++) if (!strcasecmp(z->e[i].name, name)) return &z->e[i];
    return NULL;
}

static char *zip_read(Zip *z, const char *name, int *len) {   /* malloc'd, 0-terminated */
    ZEnt *e = zip_find(z, name);
    *len = 0;
    if (!e || e->usize > NV_MAX_FILE || e->csize > NV_MAX_FILE) return NULL;
    unsigned char lh[30];
    if (fseek(z->f, (long)e->off, SEEK_SET) || fread(lh, 1, 30, z->f) != 30 || rd32(lh) != 0x04034b50) return NULL;
    if (fseek(z->f, (long)e->off + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET)) return NULL;
    unsigned outsz = e->usize > e->csize ? e->usize : e->csize;
    unsigned char *c = malloc(e->csize + 1);
    char *out = malloc(outsz + 1);
    if (!c || !out || fread(c, 1, e->csize, z->f) != e->csize) { free(c); free(out); return NULL; }
    int got = -1;
    if (e->method == 0) { memcpy(out, c, e->csize); got = (int)e->csize; }
    else if (e->method == 8) {
        z_stream zs;
        memset(&zs, 0, sizeof zs);
        if (inflateInit2(&zs, -MAX_WBITS) == Z_OK) {
            zs.next_in = c; zs.avail_in = e->csize;
            zs.next_out = (Bytef *)out; zs.avail_out = e->usize;
            int r = inflate(&zs, Z_FINISH);
            if (r == Z_STREAM_END || r == Z_OK || r == Z_BUF_ERROR) got = (int)(e->usize - zs.avail_out);
            inflateEnd(&zs);
        }
    }
    free(c);
    if (got < 0) { free(out); return NULL; }
    out[got] = 0;
    *len = got;
    return out;
}

/* ===== EPUB ===== */
static int xml_attr(const char *tag, const char *te, const char *name, char *out, int n) {
    int nl = (int)strlen(name);
    for (const char *p = tag; p + nl < te; p++) {
        if (strncmp(p, name, nl) || (p > tag && !isspace((unsigned char)p[-1]))) continue;
        const char *q = p + nl;
        while (q < te && isspace((unsigned char)*q)) q++;
        if (q >= te || *q != '=') continue;
        q++;
        while (q < te && isspace((unsigned char)*q)) q++;
        if (q >= te || (*q != '"' && *q != '\'')) continue;
        char qc = *q++;
        int k = 0;
        while (q < te && *q != qc && k < n - 1) out[k++] = *q++;
        out[k] = 0;
        return 1;
    }
    out[0] = 0;
    return 0;
}

static void xml_text(const char *doc, const char *tag, char *out, int n) {   /* text of the first <tag> */
    int tl = (int)strlen(tag), k = 0;
    const char *p = strstr(doc, tag);
    while (p && isalnum((unsigned char)p[tl])) p = strstr(p + 1, tag);
    out[0] = 0;
    if (!p || !(p = strchr(p, '>'))) return;
    p++;
    const char *e = p + strlen(p);
    while (p < e && *p != '<' && k < n - 5) {
        if (*p == '&') {
            int used;
            unsigned cp = ent_dec(p + 1, e, &used);
            if (used) { p += 1 + used; if (cp) k += u8_enc(cp, out + k); continue; }
        }
        char c = *p++;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && (k == 0 || out[k - 1] == ' ')) continue;
        out[k++] = c;
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
}

static void url_dec(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

static void path_join(char *out, int n, const char *dir, const char *href) {
    char tmp[768], *parts[64], *save = NULL;
    int np = 0;
    if (href[0] == '/') snprintf(tmp, sizeof tmp, "%s", href + 1);
    else snprintf(tmp, sizeof tmp, "%s%s%s", dir, dir[0] ? "/" : "", href);
    char *h = strchr(tmp, '#');
    if (h) *h = 0;
    url_dec(tmp);
    for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (np > 0) np--; continue; }
        if (np < 64) parts[np++] = t;
    }
    out[0] = 0;
    for (int i = 0; i < np; i++) {
        if (i) strncat(out, "/", n - strlen(out) - 1);
        strncat(out, parts[i], n - strlen(out) - 1);
    }
}

/* ===== EPUB contents (nav.xhtml or toc.ncx) ===== */
static void dir_of(char *d, int n, const char *path) {
    snprintf(d, n, "%s", path);
    char *s = strrchr(d, '/');
    if (s) *s = 0; else d[0] = 0;
}

static void u8_trim_end(char *s, int *k) {   /* don't end on half a UTF-8 character */
    int j = *k;
    while (j > 0 && ((unsigned char)s[j - 1] & 0xC0) == 0x80) j--;
    if (j > 0) {
        unsigned char c = (unsigned char)s[j - 1];
        int need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (*k - (j - 1) < need) *k = j - 1;
    }
    s[*k] = 0;
}

/* text from p up to the closing </name>, inner tags skipped; returns just past the closing tag */
static const char *inner_text(const char *p, const char *e, const char *name, char *out, int n) {
    int k = 0, nl = (int)strlen(name);
    while (p < e) {
        if (*p == '<') {
            if (p + 1 < e && p[1] == '/') {
                const char *q = p + 2, *c = q;
                while (c < e && isalnum((unsigned char)*c)) c++;
                if (c < e && *c == ':') q = c + 1;
                if (e - q >= nl && !strncasecmp(q, name, nl) && (q + nl >= e || !isalnum((unsigned char)q[nl]))) {
                    p = tag_end(q, e);
                    break;
                }
            }
            p = tag_end(p + 1, e);
            continue;
        }
        if (*p == '&') {
            int used;
            unsigned cp = ent_dec(p + 1, e, &used);
            if (used) {
                p += 1 + used;
                if (cp == 0xA0) cp = ' ';
                if (cp == ' ') { if (k > 0 && out[k - 1] != ' ' && k < n - 5) out[k++] = ' '; }
                else if (cp && k < n - 5) k += u8_enc(cp, out + k);
                continue;
            }
        }
        char c = *p++;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && (k == 0 || out[k - 1] == ' ')) continue;
        if (k < n - 5) out[k++] = c;
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    u8_trim_end(out, &k);
    return p;
}

static int toc_add(TocT *tt, int n, const char *dir, const char *href, const char *label) {
    char h[512];
    if (n >= NV_MAXTOC || !href[0] || href[0] == '#') return n;
    snprintf(h, sizeof h, "%s", href);
    TocT *t = &tt[n];
    char *hash = strchr(h, '#');
    t->frag[0] = 0;
    if (hash) { snprintf(t->frag, sizeof t->frag, "%s", hash + 1); url_dec(t->frag); *hash = 0; }
    path_join(t->file, sizeof t->file, dir, h);
    if (!t->file[0]) return n;
    snprintf(t->title, sizeof t->title, "%s", label);
    t->off = t->fb = -1;
    return n + 1;
}

static int parse_ncx(const char *x, int len, const char *dir, TocT *tt) {   /* EPUB 2 */
    const char *p = x, *e = x + len;
    char label[96] = "", src[256];
    int n = 0;
    while (p < e && (p = memchr(p, '<', e - p))) {
        const char *q = p + 1;
        int close = q < e && *q == '/';
        if (close) q++;
        char name[16];
        int k = 0;
        while (q < e && (isalnum((unsigned char)*q) || *q == ':')) {
            if (*q == ':') k = 0;
            else if (k < 15) name[k++] = (char)tolower((unsigned char)*q);
            q++;
        }
        name[k] = 0;
        const char *te = tag_end(q, e);
        if (!close && !strcmp(name, "text") && te[-1] == '>' && te[-2] != '/') {
            p = inner_text(te, e, "text", label, sizeof label);
            continue;
        }
        if (!close && !strcmp(name, "content") && xml_attr(q, te, "src", src, sizeof src)) {
            n = toc_add(tt, n, dir, src, label);
            label[0] = 0;
        }
        p = te;
    }
    return n;
}

static int parse_nav(const char *x, int len, const char *dir, TocT *tt) {   /* EPUB 3 */
    const char *e = x + len, *start = NULL, *stop, *p;
    char href[256], label[96], ty[64];
    int n = 0;
    for (p = x; (p = find_str(p, e, "<nav")); p += 4) {
        if (p + 4 < e && isalnum((unsigned char)p[4])) continue;
        const char *te = tag_end(p, e);
        if (!start) start = te;   /* the first <nav>, used if none says "toc" */
        if (xml_attr(p, te, "epub:type", ty, sizeof ty) && strstr(ty, "toc")) { start = te; break; }
    }
    if (!start) return 0;
    stop = find_str(start, e, "</nav");
    if (!stop) stop = e;
    for (p = start; p < stop && (p = memchr(p, '<', stop - p)); ) {
        const char *q = p + 1;
        if (q + 1 < stop && (*q == 'a' || *q == 'A') && (isspace((unsigned char)q[1]) || q[1] == '>')) {
            const char *te = tag_end(q, stop);
            if (xml_attr(q, te, "href", href, sizeof href)) {
                p = inner_text(te, stop, "a", label, sizeof label);
                n = toc_add(tt, n, dir, href, label);
                continue;
            }
            p = te;
            continue;
        }
        p = q;
    }
    return n;
}

typedef struct { char id[96], href[256]; int html, img, nav, ncx, cover; } MItem;

static int load_epub(const char *path, NvBook *b, Buf *o) {
    Zip z;
    int len = 0, ok = 0, nmi = 0, cmi = 0, nsp = 0, csp = 0, nord = 0, got = 0, ntt = 0;
    int cand[2] = { -1, -1 };
    char opf_path[512] = "", opf_dir[512] = "", tmp[512], cover_id[96] = "";
    char *cx = NULL, *opf = NULL;
    MItem *mi = NULL;
    char (*sp)[96] = NULL;
    int *ord = NULL, *fl = NULL;
    TocT *tt = NULL;
    const char *oe;

    if (!zip_open(&z, path)) { snprintf(b->err, sizeof b->err, "This EPUB file looks broken"); goto done; }
    cx = zip_read(&z, "META-INF/container.xml", &len);
    if (cx) {
        const char *ce = cx + len;
        for (const char *p = cx; (p = find_str(p, ce, "<rootfile")); p++) {
            char c = p[9];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '/') continue;
            if (xml_attr(p, tag_end(p, ce), "full-path", tmp, sizeof tmp)) {
                url_dec(tmp);
                snprintf(opf_path, sizeof opf_path, "%s", tmp);
                break;
            }
        }
    }
    if (!opf_path[0])
        for (int i = 0; i < z.n; i++) {
            int l = (int)strlen(z.e[i].name);
            if (l > 4 && !strcasecmp(z.e[i].name + l - 4, ".opf")) { snprintf(opf_path, sizeof opf_path, "%s", z.e[i].name); break; }
        }
    if (opf_path[0]) opf = zip_read(&z, opf_path, &len);
    if (!opf) { snprintf(b->err, sizeof b->err, "Couldn't find the book's contents"); goto done; }
    dir_of(opf_dir, sizeof opf_dir, opf_path);
    xml_text(opf, "<dc:title", b->title, sizeof b->title);
    xml_text(opf, "<dc:creator", b->author, sizeof b->author);

    oe = opf + len;
    for (const char *p = opf; p < oe && (p = memchr(p, '<', oe - p)); ) {
        const char *q = p + 1;
        char name[24];
        int k = 0;
        while (q < oe && (isalnum((unsigned char)*q) || *q == ':')) {
            if (*q == ':') k = 0; else if (k < 23) name[k++] = (char)tolower((unsigned char)*q);
            q++;
        }
        name[k] = 0;
        const char *te = tag_end(q, oe);
        if (!strcmp(name, "item")) {
            if (nmi == cmi) {
                int nc = cmi ? cmi * 2 : 64;
                MItem *nm = realloc(mi, nc * sizeof *mi);
                if (!nm) break;
                mi = nm; cmi = nc;
            }
            MItem *m = &mi[nmi++];
            char mt[96], pr[96];
            xml_attr(q, te, "id", m->id, sizeof m->id);
            xml_attr(q, te, "href", m->href, sizeof m->href);
            xml_attr(q, te, "media-type", mt, sizeof mt);
            xml_attr(q, te, "properties", pr, sizeof pr);
            m->html = strstr(mt, "html") != NULL;
            m->img = !strncmp(mt, "image/", 6) && !strstr(mt, "svg");
            m->ncx = strstr(mt, "dtbncx") != NULL;
            m->nav = strstr(pr, "nav") != NULL;
            m->cover = strstr(pr, "cover-image") != NULL;
        } else if (!strcmp(name, "itemref")) {
            if (nsp == csp) {
                int nc = csp ? csp * 2 : 64;
                char (*ns)[96] = realloc(sp, nc * sizeof *sp);
                if (!ns) break;
                sp = ns; csp = nc;
            }
            xml_attr(q, te, "idref", sp[nsp++], 96);
        } else if (!strcmp(name, "meta")) {
            char nm[32];
            if (xml_attr(q, te, "name", nm, sizeof nm) && !strcmp(nm, "cover"))
                xml_attr(q, te, "content", cover_id, sizeof cover_id);
        }
        p = te;
    }

    /* cover picture: properties="cover-image", or <meta name="cover"> */
    for (int i = 0; i < nmi && !b->cover[0]; i++)
        if (mi[i].img && (mi[i].cover || (cover_id[0] && !strcmp(mi[i].id, cover_id))))
            path_join(b->cover, sizeof b->cover, opf_dir, mi[i].href);

    /* Contents: the EPUB 3 nav first, then the EPUB 2 NCX */
    for (int i = 0; i < nmi; i++) {
        if (mi[i].nav && cand[0] < 0) cand[0] = i;
        if (mi[i].ncx && cand[1] < 0) cand[1] = i;
    }
    if (cand[0] >= 0 || cand[1] >= 0) tt = malloc(NV_MAXTOC * sizeof *tt);
    for (int c = 0; tt && c < 2 && ntt == 0; c++) {
        char tp[512], td[512];
        int tl = 0;
        if (cand[c] < 0) continue;
        path_join(tp, sizeof tp, opf_dir, mi[cand[c]].href);
        dir_of(td, sizeof td, tp);
        char *tx = zip_read(&z, tp, &tl);
        if (!tx) continue;
        ntt = c == 0 ? parse_nav(tx, tl, td, tt) : parse_ncx(tx, tl, td, tt);
        free(tx);
    }
    if (ntt > 0 && !(fl = malloc(ntt * sizeof(int)))) ntt = 0;

    ord = malloc(sizeof(int) * (nsp + nmi + 1));
    if (!ord) { snprintf(b->err, sizeof b->err, "Not enough memory for this book"); goto done; }
    for (int s = 0; s < nsp; s++)
        for (int i = 0; i < nmi; i++)
            if (!strcmp(mi[i].id, sp[s])) { if (mi[i].html) ord[nord++] = i; break; }
    if (nord == 0) for (int i = 0; i < nmi; i++) if (mi[i].html) ord[nord++] = i;   /* no spine: manifest order */

    for (int s = 0; s < nord; s++) {
        char full[512], fdir[512], cap[96] = "";
        int xl = 0, nfl = 0;
        path_join(full, sizeof full, opf_dir, mi[ord[s]].href);
        char *x = zip_read(&z, full, &xl);
        if (!x) continue;
        got++;
        sec_begin(b, o);
        int before = o->n;
        for (int t = 0; t < ntt; t++) {   /* Contents entries that point into this file */
            if (strcmp(tt[t].file, full)) continue;
            if (!tt[t].frag[0]) { if (tt[t].off < 0) tt[t].off = before; }
            else { if (tt[t].fb < 0) tt[t].fb = before; fl[nfl++] = t; }
        }
        dir_of(fdir, sizeof fdir, full);
        Wr w;
        memset(&w, 0, sizeof w);
        w.o = o; w.cap = cap; w.capsz = sizeof cap;
        w.b = b; w.dir = fdir; w.tt = tt; w.fl = fl; w.nfl = nfl;
        html_text(x, xl, &w);
        if (w.npend) wr_resolve(&w);
        free(x);
        if (o->n > before && b->nch > 0 && b->ch[b->nch - 1].off == before) {
            int cl = (int)strlen(cap);
            while (cl > 0 && cap[cl - 1] == ' ') cap[--cl] = 0;
            snprintf(b->ch[b->nch - 1].title, sizeof b->ch[0].title, "%s", cap);
        }
        if (o->fail) break;
    }
    if (!got) snprintf(b->err, sizeof b->err, "Couldn't read the book's chapters");
    else ok = 1;

    if (ok && ntt > 0 && (b->toc = malloc(ntt * sizeof(NvToc)))) {
        for (int t = 0; t < ntt; t++) {
            int off = tt[t].off >= 0 ? tt[t].off : tt[t].fb;   /* anchor not found: start of its file */
            if (off < 0) continue;
            b->toc[b->ntoc].off = off;
            snprintf(b->toc[b->ntoc].title, sizeof b->toc[0].title, "%s", tt[t].title);
            b->ntoc++;
        }
    }
done:
    free(fl); free(tt);
    free(ord); free(sp); free(mi); free(opf); free(cx);
    zip_close(&z);
    return ok;
}

/* ===== TXT ===== */
static char *read_file(const char *path, int *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > NV_MAX_FILE) { fclose(f); return NULL; }
    char *d = malloc(sz + 1);
    if (!d) { fclose(f); return NULL; }
    *n = (int)fread(d, 1, sz, f);
    fclose(f);
    d[*n] = 0;
    return d;
}

static const unsigned short cp1252[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };

static char *txt_utf8(const unsigned char *d, int n, int *on) {   /* UTF-8 / UTF-16 / Windows-1252 -> UTF-8 */
    char *o;
    int k = 0;
    if (n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) {
        int be = d[0] == 0xFE;
        o = malloc((size_t)n * 2 + 4);
        if (!o) return NULL;
        for (int i = 2; i + 1 < n; i += 2) {
            unsigned u = be ? (unsigned)(d[i] << 8 | d[i + 1]) : (unsigned)(d[i] | d[i + 1] << 8);
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
                unsigned u2 = be ? (unsigned)(d[i + 2] << 8 | d[i + 3]) : (unsigned)(d[i + 2] | d[i + 3] << 8);
                if (u2 >= 0xDC00 && u2 < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (u2 - 0xDC00); i += 2; }
            }
            if (u) k += u8_enc(u, o + k);
        }
    } else {
        int s = n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF ? 3 : 0, utf8 = 1;
        for (int i = s; i < n; ) { int l = u8_len(d + i, d + n); if (!l) { utf8 = 0; break; } i += l; }
        o = malloc((size_t)n * 3 + 4);
        if (!o) return NULL;
        for (int i = s; i < n; i++) {
            unsigned c = d[i];
            if (utf8 || c < 0x80) { if (c) o[k++] = (char)c; continue; }
            unsigned u = c < 0xA0 ? cp1252[c - 0x80] : c;
            if (u) k += u8_enc(u, o + k);
        }
    }
    o[k] = 0;
    *on = k;
    return o;
}

static int is_chapter_line(const char *s, int n) {
    static const char *const kw[] = { "chapter", "prologue", "epilogue", "interlude", "afterword",
                                      "foreword", "preface", "intermission", "side story", "part", NULL };
    if (n < 4 || n > 70) return 0;
    for (int i = 0; kw[i]; i++) {
        int kl = (int)strlen(kw[i]);
        if (n < kl || strncasecmp(s, kw[i], kl)) continue;
        int part = !strcmp(kw[i], "part");
        if (kl == n) return !part;
        unsigned char c = (unsigned char)s[kl];
        if (part) {   /* "Part 2", "Part II" */
            if (c != ' ' || kl + 1 >= n) return 0;
            if (isdigit((unsigned char)s[kl + 1])) return 1;
            int j = kl + 1;
            while (j < n && s[j] && strchr("IVXLC", s[j])) j++;
            return j > kl + 1 && (j == n || s[j] == ' ' || s[j] == ':' || s[j] == '.');
        }
        return c == ' ' || c == ':' || c == '.' || c == '-' || isdigit(c);
    }
    return 0;
}

static int load_txt(const char *path, NvBook *b, Buf *o) {
    int n = 0, un = 0;
    char *raw = read_file(path, &n);
    if (!raw) { snprintf(b->err, sizeof b->err, "Couldn't read this file"); return 0; }
    char *u = txt_utf8((const unsigned char *)raw, n, &un);
    free(raw);
    if (!u) { snprintf(b->err, sizeof b->err, "Not enough memory for this book"); return 0; }
    const char *bn = strrchr(path, '/');
    snprintf(b->title, sizeof b->title, "%s", bn ? bn + 1 : path);
    char *dot = strrchr(b->title, '.');
    if (dot) *dot = 0;

    int lines = 0, blank = 0;   /* many blank lines = wrapped lines with blank lines between paragraphs */
    for (int i = 0; i < un; ) {
        int j = i, k;
        while (j < un && u[j] != '\n') j++;
        for (k = i; k < j && (u[k] == ' ' || u[k] == '\t' || u[k] == '\r'); k++) ;
        lines++;
        if (k == j) blank++;
        i = j + 1;
    }
    int joined = lines > 20 && blank * 6 >= lines;

    Wr w;
    memset(&w, 0, sizeof w);
    w.o = o;
    sec_begin(b, o);
    for (int i = 0; i < un; ) {
        int j = i;
        while (j < un && u[j] != '\n') j++;
        int a = i, z = j;
        i = j + 1;
        while (a < z && (u[a] == ' ' || u[a] == '\t' || u[a] == '\r')) a++;
        while (z > a && (u[z - 1] == ' ' || u[z - 1] == '\t' || u[z - 1] == '\r')) z--;
        if (!w.para && b->nch > 0 && o->n - b->ch[b->nch - 1].off > NV_TXT_SPLIT) {   /* long section: split it */
            char t[96];
            snprintf(t, sizeof t, "%s", b->ch[b->nch - 1].title);
            sec_begin(b, o);
            if (b->ch[b->nch - 1].off == o->n) snprintf(b->ch[b->nch - 1].title, sizeof t, "%s", t);
        }
        if (a == z) { wr_break(&w); continue; }
        if (!w.para && is_chapter_line(u + a, z - a)) {
            sec_begin(b, o);
            if (b->ch[b->nch - 1].off == o->n) copy_title(b->ch[b->nch - 1].title, sizeof b->ch[0].title, u + a, z - a);
            w.head = 1;
            wr_utf8(&w, u + a, z - a);
            wr_break(&w);
            continue;
        }
        if (joined && w.para) wr_space(&w);
        wr_utf8(&w, u + a, z - a);
        if (!joined) wr_break(&w);
        if (o->fail) break;
    }
    wr_break(&w);
    free(u);
    return 1;
}

/* ===== API ===== */
static int finish_book(NvBook *b, Buf *o) {
    if (o->fail) { snprintf(b->err, sizeof b->err, "Not enough memory for this book"); return 0; }
    while (b->nch > 0 && b->ch[b->nch - 1].off >= o->n) b->nch--;
    if (o->n == 0 || b->nch == 0) { snprintf(b->err, sizeof b->err, "No text found in this book"); return 0; }
    b->ch[0].off = 0;
    for (int i = 0; i < b->nch; i++) b->ch[i].end = i + 1 < b->nch ? b->ch[i + 1].off : o->n;
    b->text = o->p;
    b->len = o->n;
    return 1;
}

int nv_load(const char *path, NvBook *b) {
    Buf o;
    memset(b, 0, sizeof *b);
    memset(&o, 0, sizeof o);
    snprintf(b->path, sizeof b->path, "%s", path);
    const char *dot = strrchr(path, '.');
    int ok = 0;
    if (dot && !strcasecmp(dot, ".epub")) ok = load_epub(path, b, &o);
    else if (dot && !strcasecmp(dot, ".txt")) ok = load_txt(path, b, &o);
    else snprintf(b->err, sizeof b->err, "This file type isn't supported");
    if (ok) ok = finish_book(b, &o);
    if (!ok) {
        free(o.p); free(b->toc); free(b->img);
        b->text = NULL; b->len = 0; b->nch = 0;
        b->toc = NULL; b->ntoc = 0; b->img = NULL; b->nimg = b->imgcap = 0;
    }
    return ok;
}

void nv_free(NvBook *b) {
    free(b->text); free(b->toc); free(b->img);
    memset(b, 0, sizeof *b);
}

unsigned char *nv_zip_get(const char *zip, const char *entry, int *len) {
    Zip z;
    *len = 0;
    if (!zip || !entry || !entry[0]) return NULL;
    char *d = zip_open(&z, zip) ? zip_read(&z, entry, len) : NULL;
    zip_close(&z);
    return (unsigned char *)d;
}

unsigned char *nv_img_data(const NvBook *b, int i, int *len) {
    *len = 0;
    if (!b || i < 0 || i >= b->nimg) return NULL;
    return nv_zip_get(b->path, b->img[i], len);
}
