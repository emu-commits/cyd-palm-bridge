/* news.c -- on-SD store for the RSS reader. See news.h.
 *
 * news.idx: [magic u32][count u32] then `count` fixed 172-byte records
 *           { feed[32], title[128], off u32, len u32, when u32 }.
 * news.dat: the article bodies concatenated (UTF-8). A record's off/len point in.
 * The reader seeks a single record + a single body span, so RAM is O(1) in the
 * number of articles.
 */
#include "news.h"
#include <stdio.h>
#include <string.h>

#define NEWS_MAGIC 0x4E455731u   /* 'NEW1' */
#define HDR   8                  /* magic + count */
#define RECSZ (NEWS_FEED_CAP + NEWS_TITLE_CAP + 4 + 4 + 4)   /* 172 */

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
    m->len  = get32(rec+NEWS_FEED_CAP+NEWS_TITLE_CAP+4);
    m->when = get32(rec+NEWS_FEED_CAP+NEWS_TITLE_CAP+8);
    return 1;
}
int news_read_text(int i, char *buf, int cap){
    if(i<0 || i>=news_count() || cap<=0) return 0;
    uint8_t rec[RECSZ]; if(!read_rec(i,rec)) return 0;
    uint32_t off = get32(rec+NEWS_FEED_CAP+NEWS_TITLE_CAP);
    uint32_t len = get32(rec+NEWS_FEED_CAP+NEWS_TITLE_CAP+4);
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

int news_begin(void){
    news_commit();                       /* close a dangling session, if any */
    w_idx = fopen(s_idx, "wb");
    w_dat = fopen(s_dat, "wb");
    if(!w_idx || !w_dat){ news_commit(); return 0; }
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
    put32(rec+NEWS_FEED_CAP+NEWS_TITLE_CAP,   w_off);
    put32(rec+NEWS_FEED_CAP+NEWS_TITLE_CAP+4, len);
    put32(rec+NEWS_FEED_CAP+NEWS_TITLE_CAP+8, when);
    if(fwrite(rec,1,RECSZ,w_idx)!=RECSZ) return 0;
    w_off += len; w_count++;
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
    w_count=0; w_off=0;
    return ok;
}
