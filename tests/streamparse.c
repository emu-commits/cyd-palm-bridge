/* streamparse.c -- offline gate for the sliding-window DAV response parsers
 * (dav_parse_report_stream / dav_parse_members_stream in dav_xml.c).
 *
 * These are what the device uses to enumerate a collection without buffering the
 * whole etag list in RAM (the fix for the 8 KB truncation that skipped Date Book
 * with 64 events). This test proves the streamed parse is byte-for-byte identical
 * to the in-RAM buffer parse -- including across window boundaries and for the
 * trailing sync-token -- with no server needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/dav_xml.h"

static int failures;
#define CHECK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); failures++; } }while(0)

/* ---- collectors ---- */
#define MAXREC 4096
typedef struct { char name[128]; char etag[160]; int deleted; } Rec;
static Rec  g_buf[MAXREC];  static int g_bufn;   /* from the buffer parser  */
static Rec  g_str[MAXREC];  static int g_strn;   /* from the stream parser  */

static void repBuf(const char*n,const char*e,int del,void*c){ (void)c;
    if(g_bufn<MAXREC){ snprintf(g_buf[g_bufn].name,128,"%s",n); snprintf(g_buf[g_bufn].etag,160,"%s",e); g_buf[g_bufn].deleted=del; g_bufn++; } }
static void repStr(const char*n,const char*e,int del,void*c){ (void)c;
    if(g_strn<MAXREC){ snprintf(g_str[g_strn].name,128,"%s",n); snprintf(g_str[g_strn].etag,160,"%s",e); g_str[g_strn].deleted=del; g_strn++; } }
static void memBuf(const char*n,const char*e,void*c){ repBuf(n,e,0,c); }
static void memStr(const char*n,const char*e,void*c){ repStr(n,e,0,c); }

/* build a multistatus REPORT reply with n present entries + optional trailing
 * token; a few entries are 404-deleted when `withDel`. Returns malloc'd string. */
static char *build_report(int n, int withDel, const char *token, int padetag){
    size_t cap = (size_t)n * 512 + 4096; char *s = malloc(cap); size_t o = 0;
    o += snprintf(s+o,cap-o,"<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\">");
    for(int i=0;i<n;i++){
        int del = withDel && (i%17==0);
        o += snprintf(s+o,cap-o,"<d:response><d:href>/cal/uid-%d.ics</d:href>",i);
        if(del){
            o += snprintf(s+o,cap-o,"<d:status>HTTP/1.1 404 Not Found</d:status>");
        } else {
            o += snprintf(s+o,cap-o,"<d:propstat><d:prop><d:getetag>\"etag%d",i);
            for(int p=0;p<padetag;p++) o += snprintf(s+o,cap-o,"x");   /* widen entries to cross windows */
            o += snprintf(s+o,cap-o,"\"</d:getetag></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>");
        }
        o += snprintf(s+o,cap-o,"</d:response>");
    }
    if(token) o += snprintf(s+o,cap-o,"<d:sync-token>%s</d:sync-token>",token);
    o += snprintf(s+o,cap-o,"</d:multistatus>");
    return s;
}

static void compare(const char *label){
    CHECK(g_bufn==g_strn, label);
    if(g_bufn!=g_strn){ printf("    (buffer=%d stream=%d)\n",g_bufn,g_strn); return; }
    for(int i=0;i<g_bufn;i++){
        if(strcmp(g_buf[i].name,g_str[i].name) || strcmp(g_buf[i].etag,g_str[i].etag) || g_buf[i].deleted!=g_str[i].deleted){
            printf("  FAIL: %s mismatch at %d: buf(%s,%s,d%d) str(%s,%s,d%d)\n",label,i,
                   g_buf[i].name,g_buf[i].etag,g_buf[i].deleted, g_str[i].name,g_str[i].etag,g_str[i].deleted);
            failures++; return;
        }
    }
}

/* run both parsers over `xml`: buffer parser on the string, stream parser on a
 * temp FILE, then compare. */
static void run_report(const char *label, char *xml, const char *wantTok){
    g_bufn=g_strn=0;
    char tokB[512]="", tokS[512]="";
    int rcB = dav_parse_report(xml,207,repBuf,NULL,tokB,sizeof tokB);
    FILE *f=tmpfile(); fwrite(xml,1,strlen(xml),f); rewind(f);
    int rcS = dav_parse_report_stream(f,207,repStr,NULL,tokS,sizeof tokS);
    fclose(f);
    CHECK(rcB==rcS, label);
    compare(label);
    if(wantTok){ CHECK(strcmp(tokS,wantTok)==0, "stream sync-token");
        if(strcmp(tokS,wantTok)) printf("    got token '%s' want '%s'\n",tokS,wantTok); }
    CHECK(strcmp(tokB,tokS)==0, "token buffer==stream");
}

static void run_members(const char *label, char *xml){
    g_bufn=g_strn=0;
    int cB = dav_parse_members(xml,memBuf,NULL);
    FILE *f=tmpfile(); fwrite(xml,1,strlen(xml),f); rewind(f);
    int cS = dav_parse_members_stream(f,memStr,NULL);
    fclose(f);
    CHECK(cB==cS, label);
    compare(label);
}

int main(void){
    printf("== streamparse: report ==\n");
    { char *x=build_report(5,0,"HwoQEgwAAAErNjU=",0);   run_report("small report",x,"HwoQEgwAAAErNjU="); free(x); }
    { char *x=build_report(64,1,"TOKEN-64",0);          run_report("64 entries + deletes",x,"TOKEN-64"); free(x); }
    { char *x=build_report(300,1,"TOKEN-300",0);        run_report("300 entries",x,"TOKEN-300"); free(x); }
    /* pad etags so <response> blocks straddle the 4 KB stream window */
    { char *x=build_report(50,1,"TOKEN-PAD",200);       run_report("wide entries cross window",x,"TOKEN-PAD"); free(x); }
    { char *x=build_report(0,0,"TOKEN-EMPTY",0);        run_report("empty collection",x,"TOKEN-EMPTY"); free(x); }
    { char *x=build_report(10,0,NULL,0);                run_report("no token",x,""); free(x); }

    printf("== streamparse: members (PROPFIND) ==\n");
    { char *x=build_report(5,0,NULL,0);    run_members("small members",x); free(x); }
    { char *x=build_report(128,0,NULL,0);  run_members("128 members",x); free(x); }
    { char *x=build_report(40,0,NULL,150); run_members("wide members cross window",x); free(x); }

    /* token-expired sentinel: a 6578 error body must return 1 (not parse) */
    { const char *exp="<?xml version=\"1.0\"?><d:error xmlns:d=\"DAV:\"><d:valid-sync-token/></d:error>";
      FILE *f=tmpfile(); fwrite(exp,1,strlen(exp),f); rewind(f);
      int rc=dav_parse_report_stream(f,207,repStr,NULL,NULL,0); fclose(f);
      CHECK(rc==1, "valid-sync-token -> rc=1"); }

    if(failures){ printf("\nstreamparse: %d FAILURE(S)\n",failures); return 1; }
    printf("\nstreamparse: ALL PASS\n");
    return 0;
}
