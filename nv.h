#ifndef NV_H
#define NV_H
/* aniread novel loader (R-2a/R-2b): EPUB and TXT -> one UTF-8 text with style codes */
#define NV_MAXCH  1000
#define NV_MAXIMG 2000
#define NV_MAXTOC 1000
#define NV_B_ON   '\x01'   /* bold on */
#define NV_B_OFF  '\x02'
#define NV_I_ON   '\x03'   /* italic on */
#define NV_I_OFF  '\x04'
#define NV_HEAD   '\x05'   /* first byte of a heading paragraph */
#define NV_CENTER '\x06'   /* first byte of a centred paragraph */
#define NV_IMG    '\x07'   /* a picture paragraph: NV_IMG, the picture number in digits, '\n' */

typedef struct { int off, end; char title[96]; } NvChap;   /* a section: EPUB spine item or TXT chapter */
typedef struct { int off; char title[96]; } NvToc;         /* a Contents entry */
typedef struct {
    char *text; int len;            /* paragraphs end with '\n' */
    NvChap ch[NV_MAXCH]; int nch;
    NvToc *toc; int ntoc;           /* the EPUB's own contents (0 = none) */
    char (*img)[256]; int nimg, imgcap;   /* picture paths inside the zip */
    char path[512], cover[256];     /* the book file, the cover picture inside it */
    char title[200], author[120];
    char err[160];
} NvBook;

int  nv_load(const char *path, NvBook *b);   /* 1 = ok, 0 = failed (reason in b->err) */
void nv_free(NvBook *b);
unsigned char *nv_zip_get(const char *zip, const char *entry, int *len);   /* malloc'd, NULL = failed */
unsigned char *nv_img_data(const NvBook *b, int i, int *len);
#endif
