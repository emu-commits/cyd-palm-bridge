/* find.c -- global streaming search across the Palm PIM databases. See find.h.
 *
 * Records are streamed one at a time via pdb_read, so memory stays bounded no
 * matter how big the database is (the no-PSRAM device constraint). Matching is
 * ASCII case-insensitive substring; that covers Find's job without dragging in
 * locale/charset folding (Palm text is CP1252, but Find queries are ASCII).
 *
 * RAM: no scratch buffers. Text fields are matched in place -- codec strings are
 * already null-terminated in the (stack) unpacked struct, and memo bodies are
 * scanned directly in the reader's record buffer via a length-bounded matcher,
 * so nothing is copied.
 */
#include <string.h>
#include <ctype.h>
#include "palm.h"
#include "find.h"

/* case-insensitive ASCII substring over [hay, hay+haylen); returns the match
 * offset within hay, or -1. Does not require hay to be null-terminated. */
static int ci_find_n(const char *hay, int haylen, const char *needle){
    if(!hay || !needle || !*needle) return -1;
    int nl=(int)strlen(needle);
    for(int i=0; i+nl<=haylen; i++){
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

/* match a null-terminated field; on hit fill the snippet and return 1. */
static int hit_field(FindHit *h, const char *field, const char *query){
    if(!field) return 0;
    int len=(int)strlen(field);
    int off=ci_find_n(field,len,query);
    if(off<0) return 0;
    snippet_at(h->snippet,(int)sizeof h->snippet,field,len,off);
    return 1;
}

typedef struct { int app; const char *query; find_hit_cb cb; void *ctx; int n; } FindCtx;

static int recCb(const PdbRec *r, int i, void *ctx){
    (void)i; FindCtx *f=ctx;
    FindHit h; memset(&h,0,sizeof h); h.app=f->app; h.uid=r->uniqueID;
    int matched=0;

    if(f->app==FIND_CAL){
        Appt a; if(ApptUnpack(r->data,r->len,&a)) return 0;
        matched = hit_field(&h,a.description,f->query) || hit_field(&h,a.note,f->query);
    } else if(f->app==FIND_TODO){
        Todo t; if(ToDoUnpack(r->data,r->len,&t)) return 0;
        matched = hit_field(&h,t.description,f->query) || hit_field(&h,t.note,f->query);
    } else if(f->app==FIND_ADDR){
        Addr a; if(AddrUnpack(r->data,r->len,&a)) return 0;
        for(int fi=0; fi<F_COUNT && !matched; fi++)
            matched = hit_field(&h,a.fields[fi],f->query);
    } else { /* FIND_MEMO: record bytes are plain text -- scan them in place */
        int off=ci_find_n((const char*)r->data, r->len, f->query);
        if(off>=0){ snippet_at(h.snippet,(int)sizeof h.snippet,(const char*)r->data,r->len,off); matched=1; }
    }

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
