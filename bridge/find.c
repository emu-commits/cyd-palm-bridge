/* find.c -- streaming search of the Memos database. See find.h.
 *
 * Records are streamed one at a time via pdb_read, so memory stays bounded no
 * matter how big the database is (the no-PSRAM device constraint). Matching is
 * ASCII case-insensitive substring; that covers Find's job without dragging in
 * locale/charset folding (Palm text is CP1252, but Find queries are ASCII).
 * A match must start a word, as Palm's did ("a prefix-only case-insensitive
 * search on each word": "the" finds "Then", not "Lathe"), so a short query
 * finds what it names rather than every word that happens to contain it.
 *
 * RAM: no scratch buffers. Memo bodies are scanned directly in the reader's
 * record buffer via a length-bounded matcher, so nothing is copied.
 */
#include <string.h>
#include <ctype.h>
#include "palm.h"
#include "find.h"

/* A word's letters and digits. Bytes past ASCII are letters of the CP1252
 * text (é, ß), so "ller" doesn't start a word inside "Müller". */
static int word_byte(unsigned char c){ return isalnum(c) || c >= 0x80; }

/* case-insensitive ASCII match at the start of a word in [hay, hay+haylen);
 * returns the match offset within hay, or -1. Does not require hay to be
 * null-terminated. */
static int ci_find_n(const char *hay, int haylen, const char *needle){
    if(!hay || !needle || !*needle) return -1;
    int nl=(int)strlen(needle);
    for(int i=0; i+nl<=haylen; i++){
        if(i > 0 && word_byte((unsigned char)hay[i-1])) continue;   /* mid-word */
        int j=0;
        while(j<nl && tolower((unsigned char)hay[i+j])==tolower((unsigned char)needle[j])) j++;
        if(j==nl) return i;
    }
    return -1;
}

/* build a one-line snippet from [base, base+len) centred a little before off. */
static void snippet_at(char *out, int cap, const char *base, int len, int off){
    int start = off>8 ? off-8 : 0;
    int n=0;
    for(int i=start; i<len && n<cap-1; i++){
        char c=base[i];
        if(c==0) break;
        out[n++] = (c=='\n'||c=='\r'||c=='\t') ? ' ' : c;   /* keep it one line */
    }
    out[n]=0;
}

typedef struct { int app; const char *query; find_hit_cb cb; void *ctx; int n; } FindCtx;

static int recCb(const PdbRec *r, int i, void *ctx){
    (void)i; FindCtx *f=ctx;
    if(r->attr & REC_ATTR_DELETE) return 0;   /* marked deleted by an older build */
    FindHit h; memset(&h,0,sizeof h); h.app=f->app; h.uid=r->uniqueID;
    int matched=0;
    /* a memo is plain text: scan it in place */
    int off=ci_find_n((const char*)r->data, r->len, f->query);
    if(off>=0){ snippet_at(h.snippet,(int)sizeof h.snippet,(const char*)r->data,r->len,off); matched=1; }

    if(matched){ f->n++; if(f->cb) f->cb(&h,f->ctx); }
    return 0;
}

int find_in_pdb(const char *path, int app, const char *query,
                find_hit_cb cb, void *ctx){
    if(!query || !*query) return 0;
    FindCtx f={ .app=app, .query=query, .cb=cb, .ctx=ctx, .n=0 };
    if(pdb_read(path,recCb,&f) < 0) return -1;
    return f.n;
}
