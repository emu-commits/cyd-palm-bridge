/* objfile.c -- read and merge a server object a line at a time (objfile.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "objfile.h"
#include "sync.h"          /* KIND_* */

/* ---- reading: physical lines from a range of a file ----------------------- */
typedef struct { FILE *f; long left; int peek; } Rd;   /* peek: -2 = empty */
#define NOPEEK (-2)

static int rd_open(Rd *r, FILE *f, long off, long len){
    r->f = f; r->left = len < 0 ? -1 : len; r->peek = NOPEEK;
    return fseek(f, off, SEEK_SET);
}
static int rd_get(Rd *r){
    if(r->peek != NOPEEK){ int c = r->peek; r->peek = NOPEEK; return c; }
    if(r->left == 0) return EOF;
    int c = fgetc(r->f);
    if(c == EOF){ r->left = 0; return EOF; }
    if(r->left > 0) r->left--;
    return c;
}
static int rd_peekc(Rd *r){ if(r->peek == NOPEEK) r->peek = rd_get(r); return r->peek; }

/* The start of the next physical line into b (at most cap-1 bytes, no line
 * end). 1, or 0 at the end. *more: the line goes on past b (rd_rest). */
static int rd_line(Rd *r, char *b, int cap, int *more){
    int n = 0, c = rd_get(r);
    *more = 0;
    if(c == EOF) return 0;
    while(c != EOF && c != '\n'){
        if(c != '\r'){
            if(n >= cap-1){ r->peek = c; *more = 1; break; }
            b[n++] = (char)c;
        }
        c = rd_get(r);
    }
    b[n] = 0;
    return 1;
}
/* the rest of a physical line, to out (or dropped) */
static void rd_rest(Rd *r, FILE *out, char *ob, int obcap, int *on){
    int c;
    while((c = rd_get(r)) != EOF && c != '\n'){
        if(c == '\r') continue;
        if(out) fputc(c, out);
        if(ob && *on < obcap-1) ob[(*on)++] = (char)c;
        else if(ob) *on = obcap;                      /* overflowed */
    }
}
static int is_cont(int c){ return c == ' ' || c == '\t'; }

/* a property's name (before ';' or ':'), upper-cased, without a group prefix:
 * Apple writes "item1.EMAIL" and "item2.ADR" to tie a label to a value */
static void pname(const char *l, char *nm, int cap){
    int i = 0;
    for(const char *p = l; *p && *p != ';' && *p != ':'; p++){
        if(*p == '.'){ i = 0; continue; }            /* "item1." -- the group, dropped */
        if(i < cap-1) nm[i++] = (char)toupper((unsigned char)*p);
    }
    nm[i] = 0;
}
/* BEGIN:X / END:X -> X, upper-cased and trimmed */
static void bval(const char *l, char *v, int cap){
    const char *c = strchr(l, ':'); int i = 0;
    if(c) for(c++; *c && *c != ' ' && *c != '\r' && *c != '\n' && i < cap-1; c++) v[i++] = (char)toupper((unsigned char)*c);
    v[i] = 0;
}
static const char *kcomp(int kind){
    return kind == KIND_CAL ? "VEVENT" : kind == KIND_TODO ? "VTODO" : "VCARD";
}

#define LB 512                       /* first-chunk buffer: holds any property name */
static void pass_line(Rd *r, const char *ln, int more, FILE *out);

/* ---- obj_scan ------------------------------------------------------------- */
int obj_scan(int kind, FILE *f, long off, long len, ObjScan *sc){
    memset(sc, 0, sizeof *sc);
    Rd r; if(rd_open(&r, f, off, len)) return -1;
    const char *ct = kcomp(kind);
    char ln[LB], nm[32], v[32]; int more;
    int depth = 0, ord = -1, cdepth = 0, rid = 0, first = -1, adr = 0, tel = 0, inTz = 0;
    while(rd_line(&r, ln, sizeof ln, &more)){
        pname(ln, nm, sizeof nm);
        if(!strcmp(nm, "BEGIN")){
            depth++; bval(ln, v, sizeof v);
            if(!cdepth && !strcmp(v, ct)){ ord++; cdepth = depth; rid = 0; adr = 0; tel = 0; }
            if(!strcmp(v, "VTIMEZONE")) inTz = depth;
        } else if(!strcmp(nm, "END")){
            bval(ln, v, sizeof v);
            if(cdepth && depth == cdepth && !strcmp(v, ct)){
                if(!rid && first < 0){ first = ord; sc->nMasterAdr = adr; sc->nMasterTel = tel; }
                cdepth = 0;
            }
            if(inTz && depth == inTz) inTz = 0;
            if(depth) depth--;
        } else if(cdepth && depth == cdepth){
            if(!strcmp(nm, "RECURRENCE-ID")) rid = 1;
            else if(!strcmp(nm, "ADR")) adr++;
            else if(!strcmp(nm, "TEL") || !strcmp(nm, "EMAIL")) tel++;
        } else if(inTz && depth == inTz && !strcmp(nm, "TZID") && sc->ntz < 4){
            const char *c = strchr(ln, ':');
            if(c) snprintf(sc->tz[sc->ntz++], sizeof sc->tz[0], "%s", c+1);
        }
        pass_line(&r, "", more, NULL);                 /* drop the rest, continuations too */
    }
    if(ord < 0) return 0;
    sc->ok = 1;
    sc->ncomp = ord + 1;
    sc->master = first >= 0 ? first : 0;
    return 0;
}

/* ---- what the Palm reads, and what it owns -------------------------------- */
static int inlist(const char *nm, const char *const *l){
    for(; *l; l++) if(!strcmp(nm, *l)) return 1;
    return 0;
}
static const char *const RD_CAL[]  = {"UID","DTSTART","DTEND","DURATION","SUMMARY","DESCRIPTION",
                                      "EXDATE","RRULE",NULL};
static const char *const RD_TODO[] = {"UID","SUMMARY","DESCRIPTION","DUE","PRIORITY","STATUS",
                                      "PERCENT-COMPLETE",NULL};
static const char *const RD_CARD[] = {"UID","VERSION","N","FN","ORG","TITLE","TEL","EMAIL",
                                      "ADR","NOTE",NULL};

enum { R_ALL, R_FIRST5, R_LAST, R_ALARM };
typedef struct { const char *names[4]; int rule; } Grp;
static const Grp G_CAL[] = {
    {{"DTSTART","DTEND","DURATION",NULL}, R_ALL}, {{"SUMMARY",NULL}, R_ALL},
    {{"DESCRIPTION",NULL}, R_ALL}, {{"RRULE",NULL}, R_ALL}, {{"EXDATE",NULL}, R_ALL},
    {{"VALARM",NULL}, R_ALARM}, {{NULL}, 0} };
static const Grp G_TODO[] = {
    {{"SUMMARY",NULL}, R_ALL}, {{"DESCRIPTION",NULL}, R_ALL}, {{"DUE",NULL}, R_ALL},
    {{"PRIORITY",NULL}, R_ALL}, {{"STATUS","PERCENT-COMPLETE","COMPLETED",NULL}, R_ALL},
    {{NULL}, 0} };
/* The Palm has five phone/e-mail slots, filled by the first five TEL or EMAIL
 * lines in order, and one address, the last ADR (vcard_parse). Only those
 * lines are the Palm's to replace; the sixth phone and the home address stay. */
static const Grp G_CARD[] = {
    {{"N",NULL}, R_ALL}, {{"FN",NULL}, R_ALL}, {{"ORG",NULL}, R_ALL}, {{"TITLE",NULL}, R_ALL},
    {{"TEL","EMAIL",NULL}, R_FIRST5}, {{"ADR",NULL}, R_LAST}, {{"NOTE",NULL}, R_ALL},
    {{NULL}, 0} };
static const Grp *groups(int kind){ return kind == KIND_CAL ? G_CAL : kind == KIND_TODO ? G_TODO : G_CARD; }
static int ngroups(const Grp *g){ int n = 0; while(g[n].names[0]) n++; return n; }
static int grpOf(const Grp *g, const char *nm){
    for(int i = 0; g[i].names[0]; i++) if(g[i].rule != R_ALARM && inlist(nm, g[i].names)) return i;
    return -1;
}

/* ---- obj_filter ----------------------------------------------------------- */
typedef struct { char *b; int cap, n; } Ob;
static void ob_put(Ob *o, const char *s){ for(; *s; s++){ if(o->n < o->cap-1) o->b[o->n++] = *s; else o->n = o->cap; } }

int obj_filter(int kind, FILE *f, long off, long len, const ObjScan *sc, char *out, int cap){
    if(!sc->ok || cap < 2) return -1;
    Rd r; if(rd_open(&r, f, off, len)) return -1;
    const char *ct = kcomp(kind);
    const char *const *rdl = kind == KIND_CAL ? RD_CAL : kind == KIND_TODO ? RD_TODO : RD_CARD;
    Ob o = { out, cap, 0 };
    if(kind != KIND_CARD) ob_put(&o, "BEGIN:VCALENDAR\r\n");
    char ln[LB], nm[32], v[32]; int more;
    int depth = 0, ord = -1, mdepth = 0, alarm = 0;
    while(rd_line(&r, ln, sizeof ln, &more)){
        pname(ln, nm, sizeof nm);
        int keep = 0, isBegin = !strcmp(nm, "BEGIN"), isEnd = !strcmp(nm, "END");
        if(isBegin){
            depth++; bval(ln, v, sizeof v);
            if(!mdepth && depth >= 1 && !strcmp(v, ct)){ ord++; if(ord == sc->master){ mdepth = depth; keep = 1; } }
            else if(mdepth && depth == mdepth+1 && kind == KIND_CAL && !strcmp(v, "VALARM")){ alarm = 1; keep = 1; }
        } else if(isEnd){
            bval(ln, v, sizeof v);
            if(mdepth && depth == mdepth && !strcmp(v, ct)){ keep = 1; mdepth = -mdepth; }
            else if(mdepth > 0 && alarm && depth == mdepth+1){ keep = 1; alarm = 0; }
            if(depth) depth--;
        } else if(mdepth > 0 && depth == mdepth) keep = inlist(nm, rdl);
        else if(mdepth > 0 && alarm && depth == mdepth+1) keep = !strcmp(nm, "TRIGGER");

        if(keep){
            ob_put(&o, ln);
            if(more) rd_rest(&r, NULL, o.b, o.cap, &o.n);
        } else if(more) rd_rest(&r, NULL, NULL, 0, NULL);
        while(is_cont(rd_peekc(&r))){                 /* continuation: unfold */
            rd_get(&r);                               /* the leading space */
            if(keep){ char c2[LB]; int m2; rd_line(&r, c2, sizeof c2, &m2); ob_put(&o, c2);
                      if(m2) rd_rest(&r, NULL, o.b, o.cap, &o.n); }
            else { char c2[8]; int m2; rd_line(&r, c2, sizeof c2, &m2); if(m2) rd_rest(&r, NULL, NULL, 0, NULL); }
        }
        if(keep) ob_put(&o, "\r\n");
        if(mdepth < 0) break;                         /* the master is done */
    }
    if(kind != KIND_CARD) ob_put(&o, "END:VCALENDAR\r\n");
    if(o.n >= cap){ out[cap-1] = 0; return cap-1; }
    out[o.n] = 0;
    return o.n;
}

/* ---- obj_merge ------------------------------------------------------------ */
/* the lines of group g in an emitted component, CRLF-ended, into a new string */
static char *grp_text(const char *e, const Grp *g){
    size_t cap = strlen(e) + 1; char *o = malloc(cap); if(!o) return NULL;
    size_t n = 0; int alarm = 0;
    const char *p = e;
    while(*p){
        const char *eol = strchr(p, '\n'); size_t l = eol ? (size_t)(eol - p + 1) : strlen(p);
        char nm[32]; pname(p, nm, sizeof nm);
        char v[32] = ""; if(!strcmp(nm, "BEGIN") || !strcmp(nm, "END")) bval(p, v, sizeof v);
        int take;
        if(g->rule == R_ALARM){
            if(!strcmp(nm, "BEGIN") && !strcmp(v, "VALARM")) alarm = 1;
            take = alarm;
            if(!strcmp(nm, "END") && !strcmp(v, "VALARM")) alarm = 0;
        } else {
            if(!strcmp(nm, "BEGIN") && !strcmp(v, "VALARM")) alarm = 1;
            take = !alarm && strcmp(nm, "BEGIN") && strcmp(nm, "END") && inlist(nm, g->names);
            if(!strcmp(nm, "END") && !strcmp(v, "VALARM")) alarm = 0;
        }
        if(take){ memcpy(o + n, p, l); n += l; }
        p += l;
    }
    o[n] = 0;
    return o;
}

/* copy (out != NULL) or drop the rest of the current logical line, whose first
 * chunk `ln` has been read */
static void pass_line(Rd *r, const char *ln, int more, FILE *out){
    if(out) fputs(ln, out);
    if(more) rd_rest(r, out, NULL, 0, NULL);
    if(out) fputs("\r\n", out);
    while(is_cont(rd_peekc(r))){
        char c2[LB]; int m2; rd_line(r, c2, sizeof c2, &m2);
        if(out) fputs(c2, out);
        if(m2) rd_rest(r, out, NULL, 0, NULL);
        if(out) fputs("\r\n", out);
    }
}

/* split "A\r\nB\r\n" in place into lines (each kept with its "\r\n") */
static int split5(char *t, char **ln){
    int n = 0;
    for(char *p = t; *p && n < 5; ){
        char *e = strstr(p, "\r\n"); if(!e) break;
        ln[n++] = p; p = e + 2;
    }
    return n;
}
static int lneq(const char *a, const char *b){
    const char *ea = strstr(a, "\r\n"), *eb = strstr(b, "\r\n");
    size_t la = ea ? (size_t)(ea - a) : strlen(a), lb = eb ? (size_t)(eb - b) : strlen(b);
    return la == lb && !memcmp(a, b, la);
}
static void putln(const char *l, FILE *out){
    const char *e = strstr(l, "\r\n"); size_t n = e ? (size_t)(e - l) : strlen(l);
    fwrite(l, 1, n, out); fputs("\r\n", out);
}

/* a group's lines from the device; a to-do marked done also gets when */
static void putgrp(const Grp *g, const char *text, int kind, const char *stamp, FILE *out){
    fputs(text, out);
    if(kind == KIND_TODO && !strcmp(g->names[0], "STATUS") && stamp && stamp[0] &&
       strstr(text, "STATUS:COMPLETED"))
        fprintf(out, "COMPLETED:%s\r\n", stamp);
}

int obj_merge(int kind, FILE *f, long off, long len, const ObjScan *sc,
              const char *eBase, const char *eSrv, const char *eLoc,
              const char *vtz, const char *tzid, const char *stamp, FILE *out){
    if(!sc->ok) return -1;
    const Grp *G = groups(kind); int ng = ngroups(G);
    char *loc[8] = {0}, *bas[8] = {0}, *srv[8] = {0};
    int chg[8] = {0}, ins[8] = {0}, nchg = 0, rc = -1;
    /* WHAT THE DEVICE CHANGED is its record against the base -- the record as
     * it was at its last sync -- never against the server's copy: a field the
     * device never touched can differ from the server's parse for reasons that
     * aren't edits (an older parser pulled it), and must not be written. */
    for(int i = 0; i < ng; i++){
        bas[i] = grp_text(eBase, &G[i]); loc[i] = grp_text(eLoc, &G[i]); srv[i] = grp_text(eSrv, &G[i]);
        if(!bas[i] || !loc[i] || !srv[i]) goto done;
        chg[i] = strcmp(bas[i], loc[i]) != 0; nchg += chg[i];
    }
    /* A series with moved occurrences: each is tied to the series by its
     * original start (RECURRENCE-ID). Moving or re-repeating the series from
     * the device would cut them loose, so that edit is refused, not sent. */
    if(kind == KIND_CAL && sc->ncomp > 1 && (chg[0] || chg[3])) goto done;
    /* The Palm's address is the object's last ADR. If the base's address isn't
     * that one any more, which line the device edited is unknown: refused. */
    int ga = -1;
    for(int i = 0; i < ng; i++) if(G[i].rule == R_LAST) ga = i;
    if(ga >= 0 && chg[ga] && strcmp(bas[ga], srv[ga])) goto done;
    /* a changed time in a zone the object has no VTIMEZONE for gets ours */
    int needTz = 0;
    if(kind == KIND_CAL && chg[0] && tzid && tzid[0] && vtz && vtz[0]){
        char want[80]; snprintf(want, sizeof want, "TZID=%s:", tzid);
        if(strstr(loc[0], want)){
            needTz = 1;
            for(int i = 0; i < sc->ntz; i++) if(!strcmp(sc->tz[i], tzid)) needTz = 0;
        }
    }
    /* THE PHONE SLOTS, as a set: a number the device removed or changed is
     * found among the server's lines by its value and only that line goes (or
     * takes the new value, keeping its place); a new number is added after the
     * last one. Untouched numbers keep their lines exactly (TYPE, PREF, the
     * item label), and a shift of the Palm's slots rewrites nothing. */
    int gs = -1;
    for(int i = 0; i < ng; i++) if(G[i].rule == R_FIRST5) gs = i;
    char *B[5], *L[5], *S[5], *A[5]; int nb = 0, nl = 0, nsv = 0, na = 0, nextA = 0;
    int act[5]; for(int j = 0; j < 5; j++) act[j] = -1;      /* -1 keep, -2 drop, >=0 replace by A[act] */
    if(gs >= 0 && chg[gs]){
        nb = split5(bas[gs], B); nl = split5(loc[gs], L); nsv = split5(srv[gs], S);
        int mb[5] = {0}, ml[5] = {0};
        for(int i = 0; i < nb; i++) for(int k = 0; k < nl; k++)
            if(!ml[k] && lneq(B[i], L[k])){ mb[i] = ml[k] = 1; break; }
        char *R[5]; int nr = 0, usedR[5] = {0};
        for(int i = 0; i < nb; i++) if(!mb[i]) R[nr++] = B[i];
        for(int k = 0; k < nl; k++) if(!ml[k]) A[na++] = L[k];
        for(int j = 0; j < nsv; j++)
            for(int r = 0; r < nr; r++) if(!usedR[r] && lneq(S[j], R[r])){
                usedR[r] = 1; act[j] = nextA < na ? nextA++ : -2; break;
            }
    }
    int nOwned = sc->nMasterTel < 5 ? sc->nMasterTel : 5;
    Rd r; if(rd_open(&r, f, off, len)) goto done;
    const char *ct = kcomp(kind);
    char ln[LB], nm[32], v[32]; int more;
    int depth = 0, ord = -1, mdepth = 0, skip = 0, tel = 0, adr = 0;
    while(rd_line(&r, ln, sizeof ln, &more)){
        if(!ln[0] && !more) continue;                       /* a blank line */
        pname(ln, nm, sizeof nm);
        int isBegin = !strcmp(nm, "BEGIN"), isEnd = !strcmp(nm, "END");
        if(isBegin || isEnd) bval(ln, v, sizeof v);
        if(skip){                                           /* inside a dropped VALARM */
            if(isBegin) depth++;
            if(isEnd){ depth--; if(depth < skip) skip = 0; }
            pass_line(&r, ln, more, NULL);
            continue;
        }
        if(isBegin){
            if(!mdepth && !strcmp(v, ct) && ++ord == sc->master){
                if(needTz){ fputs(vtz, out); needTz = 0; }
                mdepth = depth + 1;
            } else if(mdepth && depth == mdepth && kind == KIND_CAL && !strcmp(v, "VALARM") && chg[5]){
                if(!ins[5]){ fputs(loc[5], out); ins[5] = 1; }
                depth++; skip = depth;
                pass_line(&r, ln, more, NULL);
                continue;
            }
            depth++;
            pass_line(&r, ln, more, out);
            continue;
        }
        if(isEnd){
            if(mdepth && depth == mdepth && !strcmp(v, ct)){
                for(int i = 0; i < ng; i++) if(chg[i] && !ins[i]){
                    if(i == gs) while(nextA < na) putln(A[nextA++], out);
                    else putgrp(&G[i], loc[i], kind, stamp, out);
                    ins[i] = 1;
                }
                mdepth = 0; ord = 1 << 20;                  /* no second master */
            }
            if(depth) depth--;
            pass_line(&r, ln, more, out);
            continue;
        }
        if(mdepth && depth == mdepth){
            int g = grpOf(G, nm), owned = 0;
            if(g >= 0 && g == gs && chg[g]){
                int k = tel++;
                if(k < 5){
                    if(act[k] == -1) pass_line(&r, ln, more, out);           /* untouched */
                    else {
                        if(act[k] >= 0) putln(A[act[k]], out);                 /* changed, in place */
                        pass_line(&r, ln, more, NULL);
                    }
                    if(k == nOwned - 1){                                      /* new ones after the last */
                        while(nextA < na) putln(A[nextA++], out);
                        ins[g] = 1;
                    }
                    continue;
                }
            }
            if(g >= 0){
                if(G[g].rule == R_FIRST5) owned = tel++ < 5;
                else if(G[g].rule == R_LAST) owned = adr++ == sc->nMasterAdr - 1;
                else owned = 1;
            }
            if(g >= 0 && owned && chg[g]){
                if(!ins[g]){ putgrp(&G[g], loc[g], kind, stamp, out); ins[g] = 1; }
                pass_line(&r, ln, more, NULL);
                continue;
            }
        }
        pass_line(&r, ln, more, out);
    }
    rc = nchg;
done:
    for(int i = 0; i < ng; i++){ free(loc[i]); free(bas[i]); free(srv[i]); }
    return ferror(out) ? -1 : rc;
}
