/* md.c - MangaDex source for Aniread (English only) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <strings.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <curl/curl.h>
#define JSMN_STATIC
#include "jsmn.h"
#include "md.h"

#define MD_ROOT     "ux0:data/aniread"
#define MD_API      "https://api.mangadex.org"
#define MD_UP       "https://uploads.mangadex.org"
#define MD_UA       "Aniread/1.0 (PS Vita homebrew reader)"
#define MD_CA_SRC   "app0:cacert.pem"
#define MD_CA       MD_ROOT "/ca.pem"
#define MD_LOGF     MD_ROOT "/md_log.txt"
#define MD_JSON_MAX (6 * 1024 * 1024)
#define MD_API_GAP  250000ULL    /* 4 req/s, under the 5/s limit */
#define MD_HOME_GAP 1600000ULL   /* at-home: under 40/min */
#define MD_UP_GAP   150000ULL

MdManga   md_res[MD_PAGE];
volatile int md_nres, md_total, md_res_state;
MdManga   md_cur;
MdChapter md_ch[MD_CH_MAX];
volatile int md_nch, md_ch_state;
MdJob     md_q[MD_Q_MAX];
volatile int md_nq;
volatile int md_dl_on, md_dl_page, md_dl_pages, md_lib_changed;
char md_dl_name[200];
char md_err[128];
int  md_saver = 1;

static volatile int md_quit, md_up, md_bgen, md_fgen;
static SceKernelLwMutexWork md_mx, lg_mx;
static SceUID th_api = -1, th_dl = -1;
static CURL *api_h, *dl_h;
static SceUInt64 last_api, last_home, last_up;

#define LOCK()   sceKernelLockLwMutex(&md_mx, 1, NULL)
#define UNLOCK() sceKernelUnlockLwMutex(&md_mx, 1)

/* ---------- log / time / files ---------- */
void md_log(const char *fmt, ...)
{
    sceKernelLockLwMutex(&lg_mx, 1, NULL);
    FILE *f = fopen(MD_LOGF, "a");
    if (f) {
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f); fclose(f);
    }
    sceKernelUnlockLwMutex(&lg_mx, 1);
}

static SceUInt64 now_us(void) { return sceKernelGetProcessTimeWide(); }

static void nap_ms(int ms)
{
    while (ms > 0 && !md_quit) {
        int k = ms > 100 ? 100 : ms;
        sceKernelDelayThread(k * 1000);
        ms -= k;
    }
}

static void gap_wait(SceUInt64 *last, SceUInt64 gap)
{
    for (;;) {
        LOCK();
        SceUInt64 t = now_us();
        if (t - *last >= gap) { *last = t; UNLOCK(); return; }
        SceUInt64 left = gap - (t - *last);
        UNLOCK();
        sceKernelDelayThread(left > 50000 ? 50000 : (SceUInt)left);
        if (md_quit) return;
    }
}

static int f_exists(const char *p) { SceIoStat st; return sceIoGetstat(p, &st) >= 0; }

static long f_size(const char *p)
{
    SceIoStat st;
    if (sceIoGetstat(p, &st) < 0) return -1;
    return (long)st.st_size;
}

static void mkdirs(const char *path)
{
    char b[512];
    snprintf(b, sizeof b, "%s", path);
    for (char *p = b + 5; *p; p++)
        if (*p == '/') { *p = 0; sceIoMkdir(b, 0777); *p = '/'; }
    sceIoMkdir(b, 0777);
}

static int f_replace(const char *tmp, const char *dst)
{
    sceIoRemove(dst);
    return sceIoRename(tmp, dst);
}

static int move_dir(const char *src, const char *dst)
{
    if (sceIoRename(src, dst) >= 0) return 0;
    mkdirs(dst);
    SceUID d = sceIoDopen(src);
    if (d < 0) return -1;
    SceIoDirent e; int bad = 0;
    memset(&e, 0, sizeof e);
    while (sceIoDread(d, &e) > 0) {
        if (e.d_name[0] != '.') {
            char a[400], b[400];
            snprintf(a, sizeof a, "%s/%s", src, e.d_name);
            snprintf(b, sizeof b, "%s/%s", dst, e.d_name);
            if (sceIoRename(a, b) < 0) bad = 1;
        }
        memset(&e, 0, sizeof e);
    }
    sceIoDclose(d);
    if (!bad) sceIoRmdir(src);
    return bad ? -1 : 0;
}

/* folder-safe title: ASCII only, no \/:*?"<>| */
static void md_clean(const char *in, char *out, int cap)
{
    int o = 0;
    for (; *in && o < cap - 1; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 32 || c >= 128 || strchr("\\/:*?\"<>|", c)) continue;
        if (c == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
        out[o++] = (char)c;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;
    out[o] = 0;
    if (!o) snprintf(out, cap, "Untitled");
}

/* "12.5" -> chapter012.5, "" -> chapter000 (sorts right as text and as numbers) */
static void ch_name(const char *num, char *o, int cap)
{
    char frac[8] = "";
    const char *dot = strchr(num, '.');
    if (dot) {
        int k = 0; frac[k++] = '.';
        for (dot++; *dot >= '0' && *dot <= '9' && k < 6; dot++) frac[k++] = *dot;
        frac[k] = 0;
        if (k == 1) frac[0] = 0;
    }
    snprintf(o, cap, "chapter%03d%s", atoi(num), frac);
}

static void ch_dir(const MdManga *m, const MdChapter *c, char *o, int cap)
{
    char t[96], nm[24];
    md_clean(m->title, t, sizeof t);
    ch_name(c->num, nm, sizeof nm);
    snprintf(o, cap, MD_ROOT "/manga/%s/chapters/%s", t, nm);
}

/* ---------- curl ---------- */
typedef struct { char *p; size_t n, cap; } Buf;

static size_t wr_mem(void *d, size_t s, size_t n, void *u)
{
    Buf *b = (Buf *)u; size_t k = s * n;
    if (b->n + k + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 16384;
        while (nc < b->n + k + 1) nc *= 2;
        if (nc > MD_JSON_MAX) return 0;
        char *np = realloc(b->p, nc);
        if (!np) return 0;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, d, k);
    b->n += k; b->p[b->n] = 0;
    return k;
}

static size_t wr_file(void *d, size_t s, size_t n, void *u)
{
    return fwrite(d, 1, s * n, (FILE *)u);
}

static size_t hd_cb(char *d, size_t s, size_t n, void *u)
{
    size_t k = s * n; int *hit = (int *)u;
    if (k > 8 && !strncasecmp(d, "X-Cache:", 8)) {
        const char *v = d + 8;
        while (*v == ' ') v++;
        *hit = !strncasecmp(v, "HIT", 3);
    }
    return k;
}

static int xfer_cb(void *u, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
    (void)u; (void)a; (void)b; (void)c; (void)d;
    return md_quit;
}

static CURL *easy(CURL **h)
{
    if (!*h) *h = curl_easy_init(); else curl_easy_reset(*h);
    CURL *c = *h;
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_USERAGENT, MD_UA);
    curl_easy_setopt(c, CURLOPT_CAINFO, MD_CA);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 256L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    return c;
}

static int get_mem(CURL **h, const char *url, Buf *b, long *code)
{
    CURL *c = easy(h);
    *code = 0;
    if (!c) return -1;
    b->n = 0; if (b->p) b->p[0] = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr_mem);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
    int r = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, code);
    return r;
}

static int get_file(CURL **h, const char *url, const char *path, long *code, long *bytes, int *hit)
{
    *code = 0; *bytes = 0; *hit = 0;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
    CURL *c = easy(h);
    if (!c) { fclose(f); return -1; }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, hd_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, hit);
    int r = curl_easy_perform(c);
    fclose(f);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, code);
    *bytes = f_size(path);
    return r;
}

/* GET with rate limit + retries. home = at-home server call */
static int api_get(CURL **h, const char *url, Buf *b, int home)
{
    for (int tries = 0; tries < 4 && !md_quit; tries++) {
        gap_wait(home ? &last_home : &last_api, home ? MD_HOME_GAP : MD_API_GAP);
        long code = 0;
        int r = get_mem(h, url, b, &code);
        if (r == 0 && code == 200 && b->n) return 0;
        md_log("GET %s -> curl %d http %ld", url, r, code);
        if (r == CURLE_SSL_CACERT || r == 60 || r == 77) {
            snprintf(md_err, sizeof md_err, "Certificate error (curl %d)", r);
            return -1;
        }
        if (code == 429) { nap_ms(home ? 60000 : 15000); continue; }
        if (code >= 400 && code < 500) {
            snprintf(md_err, sizeof md_err, "MangaDex error %ld", code);
            return -1;
        }
        nap_ms(3000);
    }
    snprintf(md_err, sizeof md_err, "Network error");
    return -1;
}

/* MangaDex@Home asks clients to report image loads from its nodes */
static void report(const char *url, int ok, long bytes, long ms, int hit)
{
    if (strstr(url, "mangadex.org")) return;
    char body[700];
    snprintf(body, sizeof body,
             "{\"url\":\"%s\",\"success\":%s,\"bytes\":%ld,\"duration\":%ld,\"cached\":%s}",
             url, ok ? "true" : "false", bytes, ms, hit ? "true" : "false");
    CURL *c = easy(&dl_h);
    if (!c) return;
    struct curl_slist *hd = curl_slist_append(NULL, "Content-Type: application/json");
    Buf b = {0};
    curl_easy_setopt(c, CURLOPT_URL, "https://api.mangadex.network/report");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hd);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr_mem);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_perform(c);
    curl_slist_free_all(hd);
    free(b.p);
}

/* ---------- CA list (trimmed, the Vita runs out of locks with all of them) ---------- */
static const char *ca_wanted[] = {
    "ISRG Root", "GTS Root", "GlobalSign", "DigiCert Global Root",
    "DigiCert High Assurance", "USERTrust", "COMODO", "Comodo",
    "AAA Certificate", "Amazon Root", "Starfield", "Go Daddy", "Baltimore", NULL
};

static int ca_build(void)
{
    FILE *f = fopen(MD_CA_SRC, "rb");
    if (!f) { md_log("missing %s", MD_CA_SRC); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    if (!s) { fclose(f); return -1; }
    fread(s, 1, n, f); s[n] = 0; fclose(f);
    FILE *o = fopen(MD_CA, "wb");
    if (!o) { free(s); return -1; }
    char prev[160] = "";
    int want = 0, copy = 0, kept = 0;
    char *p = s;
    while (*p) {
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char *nx = e ? e + 1 : p + l;
        size_t ll = l;
        if (ll && p[ll - 1] == '\r') ll--;
        if (copy) {
            fwrite(p, 1, ll, o); fputc('\n', o);
            if (!strncmp(p, "-----END", 8)) { copy = 0; kept++; }
        } else if (!strncmp(p, "-----BEGIN", 10)) {
            if (want) { copy = 1; fwrite(p, 1, ll, o); fputc('\n', o); }
        } else if (ll >= 3 && !strncmp(p, "===", 3)) {
            want = 0;
            for (int i = 0; ca_wanted[i]; i++) if (strstr(prev, ca_wanted[i])) want = 1;
        } else {
            size_t k = ll < sizeof prev - 1 ? ll : sizeof prev - 1;
            memcpy(prev, p, k); prev[k] = 0;
        }
        p = nx;
    }
    fclose(o); free(s);
    md_log("ca.pem: kept %d roots", kept);
    return kept > 0 ? 0 : -1;
}

/* ---------- JSON (jsmn) ---------- */
typedef struct { const char *s; jsmntok_t *t; int n; } Js;

static int js_parse(Js *j, const char *s, size_t len)
{
    jsmn_parser p;
    j->t = NULL; j->n = 0; j->s = s;
    jsmn_init(&p);
    int n = jsmn_parse(&p, s, len, NULL, 0);
    if (n <= 0) return -1;
    j->t = malloc(sizeof(jsmntok_t) * n);
    if (!j->t) return -1;
    jsmn_init(&p);
    j->n = jsmn_parse(&p, s, len, j->t, n);
    if (j->n <= 0 || j->t[0].type != JSMN_OBJECT) { free(j->t); j->t = NULL; return -1; }
    return 0;
}

static int js_skip(Js *j, int i)
{
    int e = j->t[i].end; i++;
    while (i < j->n && j->t[i].start < e) i++;
    return i;
}

static int js_eq(Js *j, int i, const char *k)
{
    if (i < 0 || i >= j->n) return 0;
    jsmntok_t *t = &j->t[i];
    int l = t->end - t->start;
    return t->type == JSMN_STRING && (int)strlen(k) == l && !strncmp(j->s + t->start, k, l);
}

static int js_get(Js *j, int o, const char *k)
{
    if (o < 0 || o >= j->n || j->t[o].type != JSMN_OBJECT) return -1;
    int i = o + 1;
    for (int c = 0; c < j->t[o].size && i + 1 < j->n; c++) {
        int v = i + 1;
        if (js_eq(j, i, k)) return v;
        i = js_skip(j, v);
    }
    return -1;
}

static int js_path(Js *j, int o, const char *path)
{
    char k[48];
    while (*path && o >= 0) {
        int l = 0;
        while (*path && *path != '.' && l < 47) k[l++] = *path++;
        k[l] = 0;
        if (*path == '.') path++;
        o = js_get(j, o, k);
    }
    return o;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static int js_str(Js *j, int i, char *o, int cap)
{
    int n = 0;
    if (cap <= 0) return 0;
    o[0] = 0;
    if (i < 0 || i >= j->n) return 0;
    jsmntok_t *t = &j->t[i];
    if (t->type == JSMN_PRIMITIVE && j->s[t->start] == 'n') return 0;
    if (t->type != JSMN_STRING && t->type != JSMN_PRIMITIVE) return 0;
    const char *s = j->s + t->start, *e = j->s + t->end;
    while (s < e && n < cap - 1) {
        unsigned c = (unsigned char)*s++;
        if (c == '\\' && s < e) {
            char x = *s++;
            if (x == 'n') c = '\n';
            else if (x == 'r') c = '\r';
            else if (x == 't' || x == 'b' || x == 'f') c = ' ';
            else if (x == 'u' && e - s >= 4) {
                c = 0;
                for (int k = 0; k < 4; k++) c = c * 16 + hexv(s[k]);
                s += 4;
                if (c >= 0xD800 && c < 0xDC00 && e - s >= 6 && s[0] == '\\' && s[1] == 'u') {
                    unsigned d = 0;
                    for (int k = 0; k < 4; k++) d = d * 16 + hexv(s[2 + k]);
                    if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); s += 6; }
                }
                char u[4]; int k = 0;
                if (c < 0x80) u[k++] = (char)c;
                else if (c < 0x800) { u[k++] = 0xC0 | (c >> 6); u[k++] = 0x80 | (c & 63); }
                else if (c < 0x10000) { u[k++] = 0xE0 | (c >> 12); u[k++] = 0x80 | ((c >> 6) & 63); u[k++] = 0x80 | (c & 63); }
                else { u[k++] = 0xF0 | (c >> 18); u[k++] = 0x80 | ((c >> 12) & 63); u[k++] = 0x80 | ((c >> 6) & 63); u[k++] = 0x80 | (c & 63); }
                if (n + k > cap - 1) break;
                memcpy(o + n, u, k); n += k;
                continue;
            } else c = (unsigned char)x;
        }
        o[n++] = (char)c;
    }
    if (s < e) {   /* cut short: don't leave half a UTF-8 letter */
        while (n > 0 && (o[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0 && (o[n - 1] & 0xC0) == 0xC0) n--;
    }
    o[n] = 0;
    return n;
}

static int js_int(Js *j, int i, int def)
{
    if (i < 0 || i >= j->n || j->t[i].type != JSMN_PRIMITIVE) return def;
    if (j->s[j->t[i].start] == 'n') return def;
    return atoi(j->s + j->t[i].start);
}

/* ---------- parsing ---------- */
static void clean_desc(char *d)
{
    char *cut = strstr(d, "---");
    if (cut) *cut = 0;
    int o = 0;
    for (int i = 0; d[i]; i++) {
        char c = d[i];
        if (c == '*') continue;
        if (c == '\n' || c == '\r') c = ' ';
        if (c == ' ' && o > 0 && d[o - 1] == ' ') continue;
        d[o++] = c;
    }
    while (o > 0 && d[o - 1] == ' ') o--;
    d[o] = 0;
}

static int parse_manga(Js *j, int o, MdManga *m)
{
    memset((void *)m, 0, sizeof *m);
    js_str(j, js_get(j, o, "id"), m->id, sizeof m->id);
    if (!m->id[0]) return -1;
    int a = js_get(j, o, "attributes");
    int t = js_get(j, a, "title");
    int en = js_get(j, t, "en");
    if (en >= 0) js_str(j, en, m->title, sizeof m->title);
    if (!m->title[0]) {
        int alt = js_get(j, a, "altTitles");
        if (alt >= 0 && j->t[alt].type == JSMN_ARRAY) {
            int i = alt + 1;
            for (int k = 0; k < j->t[alt].size; k++, i = js_skip(j, i)) {
                int e = js_get(j, i, "en");
                if (e >= 0) { js_str(j, e, m->title, sizeof m->title); break; }
            }
        }
    }
    if (!m->title[0] && t >= 0 && j->t[t].size > 0) js_str(j, t + 2, m->title, sizeof m->title);
    js_str(j, js_path(j, a, "description.en"), m->desc, sizeof m->desc);
    clean_desc(m->desc);
    js_str(j, js_get(j, a, "status"), m->status, sizeof m->status);
    js_str(j, js_get(j, a, "contentRating"), m->rating, sizeof m->rating);
    m->year = js_int(j, js_get(j, a, "year"), 0);

    int tg = js_get(j, a, "tags");
    if (tg >= 0 && j->t[tg].type == JSMN_ARRAY) {
        int i = tg + 1;
        for (int k = 0; k < j->t[tg].size; k++, i = js_skip(j, i)) {
            char nm[48], grp[16];
            int ta = js_get(j, i, "attributes");
            js_str(j, js_path(j, ta, "name.en"), nm, sizeof nm);
            js_str(j, js_get(j, ta, "group"), grp, sizeof grp);
            if (!strcmp(grp, "format") && !strcmp(nm, "Long Strip")) m->webtoon = 1;
            if (!strcmp(grp, "genre") && nm[0] &&
                strlen(m->genres) + strlen(nm) + 3 < sizeof m->genres) {
                if (m->genres[0]) strcat(m->genres, ", ");
                strcat(m->genres, nm);
            }
        }
    }

    int rel = js_get(j, o, "relationships");
    if (rel >= 0 && j->t[rel].type == JSMN_ARRAY) {
        int i = rel + 1;
        for (int k = 0; k < j->t[rel].size; k++, i = js_skip(j, i)) {
            char ty[24];
            js_str(j, js_get(j, i, "type"), ty, sizeof ty);
            int ra = js_get(j, i, "attributes");
            if (!strcmp(ty, "cover_art") && !m->cover[0])
                js_str(j, js_get(j, ra, "fileName"), m->cover, sizeof m->cover);
            else if (!strcmp(ty, "author") && !m->author[0])
                js_str(j, js_get(j, ra, "name"), m->author, sizeof m->author);
        }
    }
    return 0;
}

/* ---------- API worker (browse, chapter list, thumbnails) ---------- */
typedef struct { int type, mode, offset, gen; char q[128]; char dir[512]; MdManga m; } Req;
enum { RQ_NONE, RQ_BROWSE, RQ_FEED, RQ_META };
volatile int md_meta_state;
static Req req;
static volatile int req_on;
static MdManga tmp_res[MD_PAGE];

static void url_enc(const char *s, char *o, int cap)
{
    static const char hx[] = "0123456789ABCDEF";
    int n = 0;
    for (; *s && n < cap - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o[n++] = (char)c;
        else { o[n++] = '%'; o[n++] = hx[c >> 4]; o[n++] = hx[c & 15]; }
    }
    o[n] = 0;
}

static void browse_fail(int gen)
{
    LOCK();
    if (gen == md_bgen) md_res_state = MD_ERR;
    UNLOCK();
}

static void do_browse(Req *r)
{
    char url[900], q[400];
    int n = snprintf(url, sizeof url, MD_API "/manga?limit=%d&offset=%d"
        "&availableTranslatedLanguage%%5B%%5D=en"
        "&contentRating%%5B%%5D=safe&contentRating%%5B%%5D=suggestive"
        "&includes%%5B%%5D=cover_art&includes%%5B%%5D=author", MD_PAGE, r->offset);
    if (r->mode == MD_SEARCH) {
        url_enc(r->q, q, sizeof q);
        snprintf(url + n, sizeof url - n, "&title=%s%s", q, "&order%5Brelevance%5D=desc");
    } else if (r->mode == MD_LATEST) {
        snprintf(url + n, sizeof url - n, "%s", "&order%5BlatestUploadedChapter%5D=desc");
    } else {
        snprintf(url + n, sizeof url - n, "%s", "&order%5BfollowedCount%5D=desc");
    }

    Buf b = {0};
    if (api_get(&api_h, url, &b, 0) != 0) { free(b.p); browse_fail(r->gen); return; }
    Js j;
    if (js_parse(&j, b.p, b.n) != 0) {
        snprintf(md_err, sizeof md_err, "Bad reply from MangaDex");
        free(b.p); browse_fail(r->gen); return;
    }
    int data = js_get(&j, 0, "data");
    int total = js_int(&j, js_get(&j, 0, "total"), 0);
    int cnt = 0;
    if (data >= 0 && j.t[data].type == JSMN_ARRAY) {
        int i = data + 1;
        for (int k = 0; k < j.t[data].size && cnt < MD_PAGE; k++, i = js_skip(&j, i))
            if (parse_manga(&j, i, &tmp_res[cnt]) == 0) cnt++;
    }
    free(j.t); free(b.p);
    LOCK();
    if (r->gen == md_bgen) {
        memcpy((void *)md_res, tmp_res, sizeof(MdManga) * cnt);
        md_nres = cnt; md_total = total; md_res_state = MD_READY;
    }
    UNLOCK();
}

static void do_thumb(int k, int gen)
{
    MdManga *m = &md_res[k];
    if (!m->cover[0]) { m->thumb_ok = -1; return; }
    char url[300], path[128], part[140];
    snprintf(url, sizeof url, MD_UP "/covers/%s/%s.256.jpg", m->id, m->cover);
    snprintf(path, sizeof path, MD_ROOT "/tmp/md%d.jpg", k);
    snprintf(part, sizeof part, "%s.dl", path);
    gap_wait(&last_up, MD_UP_GAP);
    long code, bytes; int hit;
    int r = get_file(&api_h, url, part, &code, &bytes, &hit);
    if (gen != md_bgen) { sceIoRemove(part); return; }
    if (r == 0 && code == 200 && bytes > 0 && f_replace(part, path) >= 0) {
        snprintf(m->thumb, sizeof m->thumb, "%s", path);
        m->thumb_ok = 1;
    } else {
        sceIoRemove(part);
        m->thumb_ok = -1;
    }
}

static void do_feed(Req *r)
{
    int n = 0, off = 0, total = 1;
    while (off < total && n < MD_CH_MAX && !md_quit) {
        if (r->gen != md_fgen) return;
        char url[600];
        snprintf(url, sizeof url, MD_API "/manga/%s/feed?limit=100&offset=%d"
            "&translatedLanguage%%5B%%5D=en&order%%5Bchapter%%5D=asc"
            "&includes%%5B%%5D=scanlation_group"
            "&contentRating%%5B%%5D=safe&contentRating%%5B%%5D=suggestive"
            "&contentRating%%5B%%5D=erotica", r->m.id, off);
        Buf b = {0};
        Js j;
        if (api_get(&api_h, url, &b, 0) != 0 || js_parse(&j, b.p, b.n) != 0) {
            free(b.p);
            LOCK(); if (r->gen == md_fgen) md_ch_state = MD_ERR; UNLOCK();
            return;
        }
        total = js_int(&j, js_get(&j, 0, "total"), 0);
        int data = js_get(&j, 0, "data");
        if (data >= 0 && j.t[data].type == JSMN_ARRAY) {
            int i = data + 1;
            for (int k = 0; k < j.t[data].size && n < MD_CH_MAX; k++, i = js_skip(&j, i)) {
                MdChapter ch;
                memset((void *)&ch, 0, sizeof ch);
                int a = js_get(&j, i, "attributes");
                int ext = js_get(&j, a, "externalUrl");
                if (ext >= 0 && j.t[ext].type == JSMN_STRING) continue;  /* MANGA Plus link etc. */
                ch.pages = js_int(&j, js_get(&j, a, "pages"), 0);
                if (ch.pages <= 0) continue;
                js_str(&j, js_get(&j, i, "id"), ch.id, sizeof ch.id);
                js_str(&j, js_get(&j, a, "chapter"), ch.num, sizeof ch.num);
                js_str(&j, js_get(&j, a, "volume"), ch.vol, sizeof ch.vol);
                js_str(&j, js_get(&j, a, "title"), ch.title, sizeof ch.title);
                js_str(&j, js_get(&j, a, "publishAt"), ch.date, 11);
                int rel = js_get(&j, i, "relationships");
                if (rel >= 0 && j.t[rel].type == JSMN_ARRAY) {
                    int ri = rel + 1;
                    for (int q = 0; q < j.t[rel].size; q++, ri = js_skip(&j, ri)) {
                        if (js_eq(&j, js_get(&j, ri, "type") , "scanlation_group")) {
                            js_str(&j, js_path(&j, ri, "attributes.name"), ch.group, sizeof ch.group);
                            break;
                        }
                    }
                }
                int dup = 0;   /* same chapter from two groups: keep the first */
                for (int q = n - 1; q >= 0 && q >= n - 8; q--)
                    if (ch.num[0] && !strcmp(md_ch[q].num, ch.num)) { dup = 1; break; }
                if (dup) continue;
                char dir[300];
                ch_dir(&r->m, &ch, dir, sizeof dir);
                ch.have = f_exists(dir);
                md_ch[n++] = ch;
            }
        }
        free(j.t); free(b.p);
        off += 100;
    }
    LOCK();
    if (r->gen == md_fgen) { md_nch = n; md_ch_state = MD_READY; }
    UNLOCK();
}

/* Fetch info: save a MangaDex title's cover + info into a local title's meta folder */
static void do_meta(Req *r)
{
    char p[600], tmp[620];
    int cover_ok = 1, info_ok = 0;
    snprintf(p, sizeof p, "%s/meta", r->dir);
    mkdirs(p);
    if (r->m.cover[0]) {
        char url[300]; long code = 0, bytes = 0; int hit = 0;
        snprintf(url, sizeof url, MD_UP "/covers/%s/%s.512.jpg", r->m.id, r->m.cover);
        snprintf(p, sizeof p, "%s/meta/cover.jpg", r->dir);
        snprintf(tmp, sizeof tmp, "%s.dl", p);
        gap_wait(&last_up, MD_UP_GAP);
        if (get_file(&api_h, url, tmp, &code, &bytes, &hit) == 0 && code == 200 && bytes > 0 && f_replace(tmp, p) >= 0) { }
        else { sceIoRemove(tmp); cover_ok = 0; md_log("fetch cover failed (http %ld)", code); }
    } else cover_ok = 0;

    snprintf(p, sizeof p, "%s/meta/info.txt", r->dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (f) {
        char st[32];
        snprintf(st, sizeof st, "%s", r->m.status);
        if (st[0]) st[0] = (char)toupper((unsigned char)st[0]);
        fprintf(f, "title=%s\n", r->m.title);
        if (r->m.author[0]) fprintf(f, "author=%s\n", r->m.author);
        if (r->m.year)      fprintf(f, "year=%d\n", r->m.year);
        if (r->m.genres[0]) fprintf(f, "genres=%s\n", r->m.genres);
        if (st[0])          fprintf(f, "status=%s\n", st);
        fprintf(f, "type=%s\n", r->mode ? "NOVEL" : r->m.webtoon ? "WEBTOON" : "MANGA");
        if (r->m.desc[0])   fprintf(f, "synopsis=%s\n", r->m.desc);
        fprintf(f, "source=mangadex\nmdid=%s\n", r->m.id);
        if (fclose(f) == 0 && f_replace(tmp, p) >= 0) info_ok = 1;
    }
    md_log("fetch info %s: info %s, cover %s", r->m.title, info_ok ? "OK" : "FAILED", cover_ok ? "OK" : "FAILED");
    md_meta_state = !info_ok ? 3 : cover_ok ? 1 : 2;
}

static int api_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int thumb_next = 0, thumb_gen = -1;
    while (!md_quit) {
        Req r; int have = 0;
        LOCK();
        if (req_on) { r = req; req_on = 0; have = 1; }
        UNLOCK();
        if (have) {
            if (r.type == RQ_BROWSE) { do_browse(&r); thumb_next = 0; thumb_gen = r.gen; }
            else if (r.type == RQ_META) do_meta(&r);
            else do_feed(&r);
            continue;
        }
        if (md_res_state == MD_READY && thumb_gen == md_bgen && thumb_next < md_nres) {
            do_thumb(thumb_next++, thumb_gen);
            continue;
        }
        sceKernelDelayThread(20000);
    }
    sceKernelExitThread(0);
    return 0;
}

void md_browse(int mode, const char *q, int offset)
{
    LOCK();
    memset(&req, 0, sizeof req);
    req.type = RQ_BROWSE; req.mode = mode; req.offset = offset;
    snprintf(req.q, sizeof req.q, "%s", q ? q : "");
    req.gen = ++md_bgen;
    req_on = 1;
    md_nres = 0; md_res_state = MD_BUSY; md_err[0] = 0;
    UNLOCK();
}

void md_open(int idx)
{
    if (idx < 0 || idx >= md_nres) return;
    LOCK();
    memset(&req, 0, sizeof req);
    req.type = RQ_FEED;
    req.m = md_res[idx];
    md_cur = md_res[idx];
    req.gen = ++md_fgen;
    req_on = 1;
    md_nch = 0; md_ch_state = MD_BUSY; md_err[0] = 0;
    UNLOCK();
}

void md_fetch_meta(int idx, const char *dir, int novel)
{
    if (idx < 0 || idx >= md_nres) return;
    LOCK();
    memset(&req, 0, sizeof req);
    req.type = RQ_META; req.mode = novel;
    req.m = md_res[idx];
    snprintf(req.dir, sizeof req.dir, "%s", dir);
    md_meta_state = 0;
    req_on = 1;
    UNLOCK();
}

/* ---------- downloads ---------- */
static void dl_meta(const MdManga *m)
{
    char t[96], base[200], p[300];
    md_clean(m->title, t, sizeof t);
    snprintf(base, sizeof base, MD_ROOT "/manga/%s", t);
    snprintf(p, sizeof p, "%s/meta", base); mkdirs(p);
    snprintf(p, sizeof p, "%s/chapters", base); mkdirs(p);

    snprintf(p, sizeof p, "%s/meta/cover.jpg", base);
    if (!f_exists(p) && m->cover[0]) {
        char url[300], tmp[320]; long code, bytes; int hit;
        snprintf(url, sizeof url, MD_UP "/covers/%s/%s.512.jpg", m->id, m->cover);
        snprintf(tmp, sizeof tmp, "%s.dl", p);
        gap_wait(&last_up, MD_UP_GAP);
        if (get_file(&dl_h, url, tmp, &code, &bytes, &hit) == 0 && code == 200 && bytes > 0)
            f_replace(tmp, p);
        else { sceIoRemove(tmp); md_log("cover failed (http %ld)", code); }
    }

    snprintf(p, sizeof p, "%s/info.txt", base);
    if (!f_exists(p)) {
        FILE *f = fopen(p, "w");
        if (f) {
            fprintf(f, "title=%s\n", m->title);
            if (m->author[0]) fprintf(f, "author=%s\n", m->author);
            if (m->year)      fprintf(f, "year=%d\n", m->year);
            if (m->genres[0]) fprintf(f, "genres=%s\n", m->genres);
            if (m->status[0]) fprintf(f, "status=%s\n", m->status);
            fprintf(f, "type=%s\n", m->webtoon ? "WEBTOON" : "MANGA");
            if (m->desc[0])   fprintf(f, "synopsis=%s\n", m->desc);
            fprintf(f, "source=mangadex\nmdid=%s\n", m->id);
            fclose(f);
        }
    }
}

static int dl_chapter(const MdManga *m, const MdChapter *c)
{
    char fin[300], tmpd[200], url[600];
    ch_dir(m, c, fin, sizeof fin);
    if (f_exists(fin)) return 0;
    dl_meta(m);
    snprintf(tmpd, sizeof tmpd, MD_ROOT "/tmp/dl/%s", c->id);
    mkdirs(tmpd);

    for (int attempt = 0; attempt < 2 && !md_quit; attempt++) {
        Buf b = {0};
        Js j;
        snprintf(url, sizeof url, MD_API "/at-home/server/%s", c->id);
        if (api_get(&dl_h, url, &b, 1) != 0) { free(b.p); return -1; }
        if (js_parse(&j, b.p, b.n) != 0) { free(b.p); return -1; }
        char baseurl[200], hash[80];
        js_str(&j, js_get(&j, 0, "baseUrl"), baseurl, sizeof baseurl);
        int chn = js_get(&j, 0, "chapter");
        js_str(&j, js_get(&j, chn, "hash"), hash, sizeof hash);
        int arr = js_get(&j, chn, md_saver ? "dataSaver" : "data");
        const char *kind = md_saver ? "data-saver" : "data";
        if (arr < 0 || j.t[arr].type != JSMN_ARRAY || !baseurl[0] || !hash[0]) {
            md_log("at-home reply missing data");
            free(j.t); free(b.p); return -1;
        }
        int n = j.t[arr].size, bad = 0;
        md_dl_pages = n;
        int i = arr + 1;
        for (int k = 0; k < n && !md_quit; k++, i = js_skip(&j, i)) {
            char fn[128], out[300], part[310];
            js_str(&j, i, fn, sizeof fn);
            const char *ext = strrchr(fn, '.');
            if (!ext) ext = ".jpg";
            snprintf(out, sizeof out, "%s/%d%s", tmpd, k + 1, ext);
            md_dl_page = k;
            if (f_size(out) > 0) continue;              /* resumed download */
            snprintf(part, sizeof part, "%s.dl", out);
            snprintf(url, sizeof url, "%s/%s/%s/%s", baseurl, kind, hash, fn);
            int ok = 0;
            for (int tr = 0; tr < 3 && !ok && !md_quit; tr++) {
                long code = 0, bytes = 0; int hit = 0;
                SceUInt64 t0 = now_us();
                int r = get_file(&dl_h, url, part, &code, &bytes, &hit);
                long ms = (long)((now_us() - t0) / 1000);
                ok = (r == 0 && code == 200 && bytes > 0);
                report(url, ok, bytes, ms, hit);
                if (!ok) {
                    md_log("page %d failed: curl %d http %ld", k + 1, r, code);
                    nap_ms(code == 429 ? 30000 : 1500);
                }
            }
            if (ok) f_replace(part, out);
            else { sceIoRemove(part); bad = 1; break; }
        }
        free(j.t); free(b.p);
        if (md_quit) return -1;
        if (!bad) {
            md_dl_page = n;
            if (move_dir(tmpd, fin) != 0) { md_log("could not move %s", tmpd); return -1; }
            return 0;
        }
        md_log("retrying chapter with a new server");
    }
    return -1;
}

static int dl_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!md_quit) {
        MdJob job; int idx = -1;
        LOCK();
        for (int i = 0; i < md_nq; i++)
            if (md_q[i].state == DQ_WAIT) { md_q[i].state = DQ_ACTIVE; job = md_q[i]; idx = i; break; }
        UNLOCK();
        if (idx < 0) { sceKernelDelayThread(100000); continue; }

        snprintf(md_dl_name, sizeof md_dl_name, "%s  Ch. %s",
                 job.m.title, job.c.num[0] ? job.c.num : "?");
        md_dl_page = 0; md_dl_pages = job.c.pages; md_dl_on = 1;
        int r = dl_chapter(&job.m, &job.c);
        md_dl_on = 0;

        LOCK();
        for (int i = 0; i < md_nq; i++)
            if (md_q[i].state == DQ_ACTIVE && !strcmp(md_q[i].c.id, job.c.id)) {
                md_q[i].state = r == 0 ? DQ_DONE : DQ_FAIL; break;
            }
        if (r == 0) {
            if (!strcmp(md_cur.id, job.m.id))
                for (int i = 0; i < md_nch; i++)
                    if (!strcmp(md_ch[i].id, job.c.id)) md_ch[i].have = 1;
            md_lib_changed = 1;
        }
        UNLOCK();
        md_log("download %s: %s", md_dl_name, r == 0 ? "OK" : "FAILED");
    }
    sceKernelExitThread(0);
    return 0;
}

static void q_compact(void)   /* lock held: drop finished jobs */
{
    int o = 0;
    for (int i = 0; i < md_nq; i++)
        if (md_q[i].state != DQ_DONE) { if (o != i) md_q[o] = md_q[i]; o++; }
    md_nq = o;
}

int md_queue(int ci)
{
    if (ci < 0 || ci >= md_nch) return -1;
    MdChapter *c = &md_ch[ci];
    if (c->have) return 0;
    LOCK();
    for (int i = 0; i < md_nq; i++)
        if (!strcmp(md_q[i].c.id, c->id)) {
            if (md_q[i].state == DQ_FAIL) md_q[i].state = DQ_WAIT;   /* retry */
            UNLOCK(); return 0;
        }
    if (md_nq >= MD_Q_MAX) q_compact();
    if (md_nq >= MD_Q_MAX) { UNLOCK(); return -1; }
    MdJob *q = &md_q[md_nq];
    memset((void *)q, 0, sizeof *q);
    q->m = md_cur; q->c = *c; q->state = DQ_WAIT;
    md_nq++;
    UNLOCK();
    return 1;
}

int md_queue_range(int a, int b)
{
    int n = 0;
    if (a > b) { int t = a; a = b; b = t; }
    for (int i = a; i <= b; i++) if (md_queue(i) > 0) n++;
    return n;
}

/* ---------- init / test / shutdown ---------- */
int md_init(void)
{
    if (md_up) return 0;
    sceKernelCreateLwMutex(&md_mx, "md_mx", 0, 0, NULL);
    sceKernelCreateLwMutex(&lg_mx, "md_lg", 0, 0, NULL);
    mkdirs(MD_ROOT "/tmp/dl");
    mkdirs(MD_ROOT "/manga");
    sceIoRemove(MD_LOGF);
    md_log("Aniread MangaDex log");

    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    static char netmem[1024 * 1024];
    SceNetInitParam np;
    np.memory = netmem; np.size = sizeof netmem; np.flags = 0;
    int r = sceNetInit(&np);
    md_log("sceNetInit 0x%08X", r);
    r = sceNetCtlInit();
    md_log("sceNetCtlInit 0x%08X", r);
    ca_build();
    curl_global_init(CURL_GLOBAL_ALL);

    th_api = sceKernelCreateThread("md_api", api_thread, 0x10000100, 0x100000, 0, 0, NULL);
    th_dl  = sceKernelCreateThread("md_dl",  dl_thread,  0x10000100, 0x100000, 0, 0, NULL);
    if (th_api >= 0) sceKernelStartThread(th_api, 0, NULL);
    if (th_dl  >= 0) sceKernelStartThread(th_dl,  0, NULL);
    md_up = 1;
    return (th_api >= 0 && th_dl >= 0) ? 0 : -1;
}

static char st_q[128];

static void st_wait(volatile int *state)
{
    while (*state == MD_BUSY && !md_quit) nap_ms(100);
}

static int st_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    md_log("SELFTEST: %s \"%s\"", st_q[0] ? "search" : "popular", st_q);
    md_browse(st_q[0] ? MD_SEARCH : MD_POPULAR, st_q, 0);
    st_wait(&md_res_state);
    if (md_res_state != MD_READY) { md_log("SELFTEST FAIL: browse (%s)", md_err); goto out; }
    md_log("SELFTEST: %d results (total %d)", md_nres, md_total);
    for (int k = 0; k < md_nres && k < 5; k++)
        md_log("  %s | %s | %s | %s", md_res[k].title, md_res[k].author,
               md_res[k].webtoon ? "WEBTOON" : "MANGA", md_res[k].genres);

    for (int k = 0; k < md_nres && k < 5; k++) {
        md_open(k);
        st_wait(&md_ch_state);
        if (md_ch_state == MD_READY && md_nch > 0) break;
        md_log("  %s: no downloadable English chapters", md_res[k].title);
    }
    if (md_nch <= 0) { md_log("SELFTEST FAIL: no chapters"); goto out; }
    md_log("SELFTEST: %s -> %d chapters, first Ch. %s, %d pages, group %s",
           md_cur.title, md_nch, md_ch[0].num, md_ch[0].pages, md_ch[0].group);

    if (md_ch[0].have) { md_log("SELFTEST: Ch. %s already downloaded", md_ch[0].num); goto out; }
    md_queue(0);
    nap_ms(300);
    for (;;) {
        int busy = 0;
        LOCK();
        for (int i = 0; i < md_nq; i++) if (md_q[i].state <= DQ_ACTIVE) busy = 1;
        UNLOCK();
        if (!busy || md_quit) break;
        nap_ms(500);
    }
    md_log("SELFTEST: done (check the lines above for OK/FAILED)");
out:
    sceKernelExitDeleteThread(0);
    return 0;
}

void md_selftest(const char *query)
{
    snprintf(st_q, sizeof st_q, "%s", query ? query : "");
    SceUID t = sceKernelCreateThread("md_test", st_thread, 0x10000100, 0x10000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

void md_shutdown(void)
{
    if (!md_up) return;
    md_quit = 1;
    SceUInt tmo = 3000000;
    if (th_api >= 0) { sceKernelWaitThreadEnd(th_api, NULL, &tmo); sceKernelDeleteThread(th_api); }
    tmo = 3000000;
    if (th_dl >= 0)  { sceKernelWaitThreadEnd(th_dl, NULL, &tmo);  sceKernelDeleteThread(th_dl); }
    if (api_h) curl_easy_cleanup(api_h);
    if (dl_h)  curl_easy_cleanup(dl_h);
    curl_global_cleanup();
    md_up = 0;
}
