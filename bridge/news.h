/* news.h -- on-SD store for the RSS reader.
 *
 * Two files: a fixed-record index (news.idx) and a text blob (news.dat). The
 * writer (HotSync fetch) streams articles in with news_begin/news_add/news_commit;
 * the reader seeks one index record + one text span at a time, so browsing costs
 * O(1) RAM regardless of how many articles are stored. Portable + host-gated
 * (tests/news_test.c); only the paths are device-specific (news_set_paths).
 */
#ifndef NEWS_H
#define NEWS_H
#include <stdint.h>

#define NEWS_FEED_CAP  32
#define NEWS_TITLE_CAP 128

typedef struct {
    char     feed[NEWS_FEED_CAP];
    char     title[NEWS_TITLE_CAP];
    uint32_t when;      /* fetch/publish epoch (0 if unknown) */
    uint32_t len;       /* body length in bytes */
} NewsMeta;

/* override the default /sdcard paths (for the host gate). */
void news_set_paths(const char *idx_path, const char *dat_path);

/* ---- reader ---- */
int  news_count(void);                          /* articles in the store, 0 if none */
int  news_meta(int i, NewsMeta *m);             /* 1 on success */
int  news_read_text(int i, char *buf, int cap); /* body bytes read (NUL-terminated) */

/* ---- writer (HotSync fetch rebuilds the store) ---- */
int  news_begin(void);                                              /* 1 on success */
int  news_add(const char *feed, const char *title, const char *text, uint32_t when);
int  news_commit(void);                                            /* finalise count */

#endif
