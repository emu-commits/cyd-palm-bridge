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

#define NEWS_F_READ 1u      /* the reader has shown this article */

typedef struct {
    char     feed[NEWS_FEED_CAP];
    char     title[NEWS_TITLE_CAP];
    uint32_t when;      /* fetch/publish epoch (0 if unknown) */
    uint32_t len;       /* body length in bytes */
    uint32_t flags;     /* NEWS_F_* */
} NewsMeta;

/* override the default /sdcard paths (for the host gate). */
void news_set_paths(const char *idx_path, const char *dat_path);

/* ---- reader ---- */
int  news_count(void);                          /* articles in the store, 0 if none */
int  news_meta(int i, NewsMeta *m);             /* 1 on success */
int  news_read_text(int i, char *buf, int cap); /* body bytes read (NUL-terminated) */

/* ---- read state ----
 * Marking is a four-byte write into the record's flags word, so the reader can
 * mark an article the moment it puts it on screen. It survives a HotSync: the
 * store is rebuilt every sync, and news_begin() carries read state over by
 * feed+title, otherwise finishing an article and syncing would hand it back as
 * new. first_unread returns -1 when everything has been read. */
int  news_is_read(int i);
int  news_mark_read(int i);
int  news_unread(void);
int  news_first_unread(void);

/* ---- writer (HotSync fetch rebuilds the store) ---- */
int  news_begin(void);                                              /* 1 on success */
int  news_add(const char *feed, const char *title, const char *text, uint32_t when);
int  news_commit(void);                                            /* finalise count */

/* Close the two store files without ending the session, and reopen to append.
 * The device does this across each network fetch: an open file costs a 4 KB
 * FatFs sector cache, and that memory is wanted by the TLS handshake. Between
 * suspend() and resume() news_add() is a no-op, so resume before adding.
 * suspend: 1 if it closed anything. resume: 1 if the store is writable again. */
int  news_suspend(void);
int  news_resume(void);

#endif
