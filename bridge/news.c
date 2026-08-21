/* news.c -- on-SD store for the RSS reader. See news.h.
 *
 * news.idx: [magic u32][count u32] then `count` fixed 176-byte records
 *           { feed[32], title[128], off u32, len u32, when u32, flags u32 }.
 * news.dat: the article bodies concatenated (UTF-8). A record's off/len point in.
 * The reader seeks a single record + a single body span, so RAM is O(1) in the
 * number of articles.
 *
 * READ STATE lives in flags, and has to survive a HotSync: every sync REBUILDS
 * the store from scratch, so without carrying it over you finish an article,
 * sync, and are handed it again as new. news_begin() therefore reads the old
 * index before truncating it and remembers a hash of every article already read;
 * news_add() re-marks a matching article as it goes back in. Identity is
 * feed+title rather than the URL, because that is what the store keeps.
 */
#include "news.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 'NEW2': the record grew a flags word. A NEW1 store is simply not recognised,
 * so the reader shows "no news yet" until the next sync rebuilds it -- which is
 * the normal state of this store anyway. */
#define NEWS_MAGIC 0x4E455732u   /* 'NEW2' */
#define HDR   8                  /* magic + count */
#define RECSZ (NEWS_FEED_CAP + NEWS_TITLE_CAP + 4 + 4 + 4 + 4)   /* 176 */
#define F_OFF   (NEWS_FEED_CAP + NEWS_TITLE_CAP)      /* field offsets in a record */
#define F_LEN   (F_OFF + 4)
#define F_WHEN  (F_OFF + 8)
#define F_FLAGS (F_OFF + 12)

static char s_idx[128] = "/sdcard/news.idx";
static char s_dat[128] = "/sdcard/news.dat";

void news_set_paths(const char *idx_path, const char *dat_path){
    if(idx_path) snprintf(s_idx, sizeof s_idx, "%s", idx_path);
    if(dat_path) snprintf(s_dat, sizeof s_dat, "%s", dat_path);
}

/* ---- little-endian u32 helpers (portable regardless of host endianness) ---- */
static void put32(uint8_t *p, uint32_t v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static uint32_t get32(const uint8_t *p){ return p[0]|(p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

/* ---- reader ---- */
int news_count(void){
    FILE *f = fopen(s_idx, "rb"); if(!f) return 0;
    uint8_t h[HDR]; int ok = fread(h,1,HDR,f)==HDR;
    fclose(f);
    if(!ok || get32(h)!=NEWS_MAGIC) return 0;
    return (int)get32(h+4);
}
static int read_rec(int i, uint8_t *rec){
    if(i<0) return 0;
    FILE *f = fopen(s_idx, "rb"); if(!f) return 0;
    if(fseek(f, HDR + (long)i*RECSZ, SEEK_SET)!=0){ fclose(f); return 0; }
    int ok = fread(rec,1,RECSZ,f)==RECSZ;
    fclose(f);
    return ok;
}
int news_meta(int i, NewsMeta *m){
    if(i<0 || i>=news_count() || !m) return 0;
    uint8_t rec[RECSZ]; if(!read_rec(i,rec)) return 0;
    memcpy(m->feed,  rec, NEWS_FEED_CAP);  m->feed[NEWS_FEED_CAP-1]=0;
    memcpy(m->title, rec+NEWS_FEED_CAP, NEWS_TITLE_CAP); m->title[NEWS_TITLE_CAP-1]=0;
    m->len   = get32(rec+F_LEN);
    m->when  = get32(rec+F_WHEN);
    m->flags = get32(rec+F_FLAGS);
    return 1;
}

int news_is_read(int i){
    NewsMeta m;
    return news_meta(i,&m) && (m.flags & NEWS_F_READ);
}

/* One open, one sequential pass over the flags.
 *
 * The obvious implementation -- loop calling news_is_read() -- costs TWO opens
 * of news.idx per article (news_count() then read_rec()). At 200-odd articles
 * that is 400+ FatFs directory lookups, and on SD over SPI it put a full second
 * on every swipe of the reader. Walk the index once instead. */
static int flags_scan(int *first_unread, int *unread){
    if(first_unread) *first_unread = -1;
    if(unread)       *unread = 0;
    FILE *f = fopen(s_idx, "rb"); if(!f) return 0;
    uint8_t h[HDR];
    if(fread(h,1,HDR,f)!=HDR || get32(h)!=NEWS_MAGIC){ fclose(f); return 0; }
    int n = (int)get32(h+4);
    uint8_t rec[RECSZ];
    for(int i=0;i<n;i++){
        if(fread(rec,1,RECSZ,f)!=RECSZ) break;
        if(get32(rec+F_FLAGS) & NEWS_F_READ) continue;
        if(unread) (*unread)++;
        if(first_unread && *first_unread < 0){
            *first_unread = i;
            if(!unread){ fclose(f); return 1; }   /* nothing else to learn */
        }
    }
    fclose(f);
    return 1;
}

/* Set the read bit in place: seek to this record's flags word and rewrite four
 * bytes. No rewrite of the store, so marking an article read as it is displayed
 * costs one small write. */
int news_mark_read(int i){
    if(i<0 || i>=news_count()) return 0;
    FILE *f = fopen(s_idx, "r+b"); if(!f) return 0;
    long at = HDR + (long)i*RECSZ + F_FLAGS;
    uint8_t v[4];
    int ok = fseek(f, at, SEEK_SET)==0 && fread(v,1,4,f)==4;
    if(ok){
        uint32_t fl = get32(v) | NEWS_F_READ;
        put32(v, fl);
        ok = fseek(f, at, SEEK_SET)==0 && fwrite(v,1,4,f)==4;
    }
    fclose(f);
    return ok;
}

int news_unread(void){ int u = 0;  flags_scan(NULL, &u); return u; }
int news_first_unread(void){ int fu = -1; flags_scan(&fu, NULL); return fu; }
int news_read_text(int i, char *buf, int cap){
    if(i<0 || i>=news_count() || cap<=0) return 0;
    uint8_t rec[RECSZ]; if(!read_rec(i,rec)) return 0;
    uint32_t off = get32(rec+F_OFF);
    uint32_t len = get32(rec+F_LEN);
    if((int)len > cap-1) len = cap-1;
    FILE *f = fopen(s_dat, "rb"); if(!f){ buf[0]=0; return 0; }
    int n = 0;
    if(fseek(f, off, SEEK_SET)==0) n = (int)fread(buf,1,len,f);
    fclose(f);
    if(n<0) n=0;
    buf[n]=0;
    return n;
}

/* ---- writer ---- */
static FILE *w_idx, *w_dat;
static int   w_count;
static uint32_t w_off;

/* Read articles carried over the rebuild. Bounded and heap-allocated only for
 * the length of a fetch, so it costs nothing between syncs. */
#define SEEN_MAX 512
static uint32_t *w_seen;
static int       w_seen_n;

static uint32_t id_hash(const char *feed, const char *title){
    uint32_t h = 2166136261u;                       /* FNV-1a over feed \0 title */
    for(const char *p=feed;  *p; p++){ h ^= (unsigned char)*p; h *= 16777619u; }
    h ^= 0; h *= 16777619u;
    for(const char *p=title; *p; p++){ h ^= (unsigned char)*p; h *= 16777619u; }
    return h ? h : 1u;                              /* 0 is the "empty slot" value */
}
static void seen_load(void){
    w_seen_n = 0;
    int n = news_count();
    if(n <= 0) return;
    FILE *f = fopen(s_idx, "rb"); if(!f) return;
    if(fseek(f, HDR, SEEK_SET)!=0){ fclose(f); return; }
    uint8_t rec[RECSZ];
    for(int i=0;i<n && w_seen_n<SEEN_MAX;i++){
        if(fread(rec,1,RECSZ,f)!=RECSZ) break;
        if(!(get32(rec+F_FLAGS) & NEWS_F_READ)) continue;
        if(!w_seen){
            w_seen = (uint32_t*)malloc(sizeof(uint32_t)*SEEN_MAX);
            if(!w_seen) break;
        }
        char feed[NEWS_FEED_CAP], title[NEWS_TITLE_CAP];
        memcpy(feed, rec, NEWS_FEED_CAP);   feed[NEWS_FEED_CAP-1]=0;
        memcpy(title,rec+NEWS_FEED_CAP, NEWS_TITLE_CAP); title[NEWS_TITLE_CAP-1]=0;
        w_seen[w_seen_n++] = id_hash(feed,title);
    }
    fclose(f);
}
static void seen_free(void){ free(w_seen); w_seen=NULL; w_seen_n=0; }
static int seen_has(uint32_t h){
    for(int i=0;i<w_seen_n;i++) if(w_seen[i]==h) return 1;
    return 0;
}

int news_begin(void){
    news_commit();                       /* close a dangling session, if any */
    seen_load();                         /* BEFORE the truncate below wipes it */
    w_idx = fopen(s_idx, "wb");
    w_dat = fopen(s_dat, "wb");
    if(!w_idx || !w_dat){ news_commit(); return 0; }   /* news_commit frees w_seen */
    uint8_t h[HDR]; put32(h, NEWS_MAGIC); put32(h+4, 0);
    fwrite(h,1,HDR,w_idx);
    w_count = 0; w_off = 0;
    return 1;
}
int news_add(const char *feed, const char *title, const char *text, uint32_t when){
    if(!w_idx || !w_dat) return 0;
    if(!feed) feed="";
    if(!title) title="";
    if(!text) text="";
    uint32_t len = (uint32_t)strlen(text);
    if(fwrite(text,1,len,w_dat)!=len) return 0;
    uint8_t rec[RECSZ]; memset(rec,0,sizeof rec);
    snprintf((char*)rec, NEWS_FEED_CAP, "%s", feed);
    snprintf((char*)rec+NEWS_FEED_CAP, NEWS_TITLE_CAP, "%s", title);
    put32(rec+F_OFF,   w_off);
    put32(rec+F_LEN,   len);
    put32(rec+F_WHEN,  when);
    /* An article already read before this sync goes back in marked read, so a
     * HotSync never hands you the same story twice. */
    put32(rec+F_FLAGS, seen_has(id_hash(feed,title)) ? NEWS_F_READ : 0u);
    if(fwrite(rec,1,RECSZ,w_idx)!=RECSZ) return 0;
    w_off += len; w_count++;
    return 1;
}
/* ---- suspend / resume ------------------------------------------------------
 * The writer holds news.idx and news.dat open for the whole fetch, and on the
 * device each open file costs a 4 KB FatFs sector cache (CONFIG_FATFS_SECTOR_4096
 * + PER_FILE_CACHE). Two of those plus the feed spool is 12 KB of the heap that
 * the TLS handshake needs -- and measurably, feed fetches start failing as the
 * largest free block falls. So the store closes itself across the network I/O
 * and reopens to append. Resume reopens in "r+b" and seeks to the end of each
 * file, so the byte offsets already written stay valid. */
int news_suspend(void){
    if(!w_idx && !w_dat) return 0;
    if(w_idx){ fclose(w_idx); w_idx=NULL; }
    if(w_dat){ fclose(w_dat); w_dat=NULL; }
    return 1;
}

int news_resume(void){
    if(w_idx || w_dat) return 1;                  /* never suspended */
    w_idx = fopen(s_idx, "r+b");
    w_dat = fopen(s_dat, "r+b");
    if(!w_idx || !w_dat){
        if(w_idx){ fclose(w_idx); w_idx=NULL; }
        if(w_dat){ fclose(w_dat); w_dat=NULL; }
        return 0;
    }
    /* append position: the counters (w_count, w_off) were never reset, so seeking
     * to the end of both files puts the writer exactly where it left off. */
    if(fseek(w_idx, 0, SEEK_END)!=0 || fseek(w_dat, 0, SEEK_END)!=0){
        fclose(w_idx); w_idx=NULL; fclose(w_dat); w_dat=NULL;
        return 0;
    }
    return 1;
}

int news_commit(void){
    int ok = 1;
    if(w_idx){
        uint8_t c[4]; put32(c, (uint32_t)w_count);
        if(fseek(w_idx, 4, SEEK_SET)!=0 || fwrite(c,1,4,w_idx)!=4) ok=0;   /* patch count */
        fclose(w_idx); w_idx=NULL;
    }
    if(w_dat){ fclose(w_dat); w_dat=NULL; }
    seen_free();
    w_count=0; w_off=0;
    return ok;
}
