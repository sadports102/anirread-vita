#ifndef MD_H
#define MD_H

#define MD_PAGE   24     /* results per browse page */
#define MD_CH_MAX 1500   /* max chapters per title */
#define MD_Q_MAX  64     /* download queue size */

enum { MD_LATEST, MD_POPULAR, MD_SEARCH };
enum { MD_IDLE, MD_BUSY, MD_READY, MD_ERR };
enum { DQ_WAIT, DQ_ACTIVE, DQ_DONE, DQ_FAIL };

typedef struct {
    char id[40], title[160], author[80], status[16], rating[16];
    char cover[96];            /* cover file name on uploads.mangadex.org */
    char genres[256];
    char desc[1200];
    int  year, webtoon;
    char thumb[96];            /* local jpg path when thumb_ok == 1 */
    volatile int thumb_ok;     /* 0 loading, 1 ready, -1 failed */
} MdManga;

typedef struct {
    char id[40], num[16], vol[8], title[96], group[64], date[12];
    int  pages;
    volatile int have;         /* already in the library */
} MdChapter;

typedef struct {
    MdManga   m;
    MdChapter c;
    volatile int state;
} MdJob;

extern MdManga   md_res[MD_PAGE];
extern volatile int md_nres, md_total, md_res_state;
extern MdManga   md_cur;
extern MdChapter md_ch[MD_CH_MAX];
extern volatile int md_nch, md_ch_state;
extern MdJob     md_q[MD_Q_MAX];
extern volatile int md_nq;
extern volatile int md_dl_on, md_dl_page, md_dl_pages, md_lib_changed;
extern char md_dl_name[200];
extern char md_err[128];
extern int  md_saver;

int  md_init(void);
void md_shutdown(void);
void md_browse(int mode, const char *query, int offset);
void md_open(int idx);
int  md_queue(int ch_idx);
int  md_queue_range(int a, int b);
void md_selftest(const char *query);
void md_log(const char *fmt, ...);

extern volatile int md_meta_state;   /* 0 busy/none, 1 saved, 2 info only (cover failed), 3 failed */
void md_fetch_meta(int idx, const char *dir, int novel);

#endif


