/* main.c -- headless bridge CLI.
 *
 *   bridge_cli discover                            -> find collection URLs (iCloud etc.)
 *   bridge_cli push  <cal-pdb> <addr-pdb>          -> full upload (seed)
 *   bridge_cli pull  <cal-out> <addr-out>          -> full download
 *   bridge_cli sync  <cal-pdb> <addr-pdb> [policy] -> incremental two-way (RFC 6578)
 *   bridge_cli synccat cal|todo|card <pdb> [pol]   -> category-routed sync (name match)
 *   bridge_cli dump  cal|card <pdb>                -> canonical text dump
 *
 * DAV target from env: DAV_BASE, DAV_USER, DAV_PASS, DAV_CAL, DAV_CARD, BRIDGE_TZ.
 * policy: server | local | both  (conflict resolution; default server)
 * iCloud: DAV_BASE=https://caldav.icloud.com, DAV_USER=<appleid>,
 *         DAV_PASS=<app-specific-password>  (run `discover` to get paths)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "palm.h"
#include "dav.h"
#include "appinfo.h"
#include "sync.h"

/* dump: read a PDB and print each record's canonical ICS/vCard to stdout,
 * so two PDBs can be compared with a plain text diff.                      */
static int dumpCal(const PdbRec*r,int i,void*ctx){ (void)ctx;
    Appt a; if(ApptUnpack(r->data,r->len,&a)) return 0; (void)i;
    char b[4096]; ical_emit(b,sizeof b,&a,r->uniqueID); fputs(b,stdout); return 0; }
static int dumpCard(const PdbRec*r,int i,void*ctx){ (void)ctx;(void)i;
    Addr a; if(AddrUnpack(r->data,r->len,&a)) return 0;
    char b[2048]; vcard_emit(b,sizeof b,&a,r->uniqueID); fputs(b,stdout); return 0; }

static const char*env(const char*k,const char*def){ const char*v=getenv(k); return v&&*v?v:def; }

/* If href is an absolute URL, point d->base at its origin and return the path;
 * otherwise return href unchanged (already a path on the current base).        */
static char* absPath(char*href,DavCtx*d){
    if(strncmp(href,"http",4)==0){
        char*p=strstr(href,"://");
        if(p){ char*slash=strchr(p+3,'/');
            if(slash){ int n=(int)(slash-href); if(n<(int)sizeof d->base){ char b[256];
                memcpy(b,href,n); b[n]=0; snprintf(d->base,sizeof d->base,"%s",b); } return slash; } }
    }
    return href;
}

/* collect (displayname, path) collections of a kind ('c' cal / 'a' card) */
typedef struct { char name[16][128]; char path[16][256]; int n; int want; } Colls;
static void collCb(const char*href,int kind,const char*dn,void*ctx){
    Colls*c=ctx; if(kind!=c->want || c->n>=16) return;
    snprintf(c->name[c->n],128,"%s",dn); snprintf(c->path[c->n],256,"%s",href); c->n++;
}

/* Resolve principal -> home-set -> collections of `want` kind. Updates d->base
 * to the resolved host. Returns count; paths in cs->path are host-relative.    */
static int resolveColls(DavCtx*d,int want,Colls*cs){
    memset(cs,0,sizeof *cs); cs->want=want;
    char host[256]; if(dav_effective_host(d,"/",host,sizeof host)==0 && host[0]) snprintf(d->base,sizeof d->base,"%s",host);
    char principal[512]="";
    if(dav_prop_href(d,"/","<d:current-user-principal/>","",principal,sizeof principal)!=0 || !principal[0]) return -1;
    char*ppath=absPath(principal,d);
    char home[512]="";
    if(want=='a') dav_prop_href(d,ppath,"<e:addressbook-home-set/>","xmlns:e=\"urn:ietf:params:xml:ns:carddav\"",home,sizeof home);
    else          dav_prop_href(d,ppath,"<c:calendar-home-set/>","xmlns:c=\"urn:ietf:params:xml:ns:caldav\"",home,sizeof home);
    if(!home[0]) return -1;
    char*hp=absPath(home,d);
    dav_list_collections(d,hp,collCb,cs);
    return cs->n;
}

static int cmd_discover(DavCtx d){
    Colls cal={0}, card={0};
    DavCtx dc=d; int nc=resolveColls(&dc,'c',&cal);
    if(nc<0){ fprintf(stderr,"could not resolve principal (check credentials)\n"); return 1; }
    printf("host: %s\ncalendars:\n",dc.base);
    for(int i=0;i<cal.n;i++) printf("  %-28s  %s\n",cal.name[i][0]?cal.name[i]:"(unnamed)",cal.path[i]);
    DavCtx da=d; resolveColls(&da,'a',&card);
    printf("addressbooks:\n");
    for(int i=0;i<card.n;i++) printf("  %-28s  %s\n",card.name[i][0]?card.name[i]:"(unnamed)",card.path[i]);
    printf("\nSet DAV_BASE to the host and DAV_CAL/DAV_CARD to a path, then `sync`;\n"
           "or use `synccat` to auto-route Palm categories to same-named calendars.\n");
    return 0;
}

/* strip leading/trailing slashes so a discovered href works as a `coll` arg */
static void trimColl(const char*in,char*out,int cap){
    while(*in=='/') in++;
    int l=(int)strlen(in); while(l>0 && in[l-1]=='/') l--;
    if(l>cap-1) l=cap-1;
    memcpy(out,in,l); out[l]=0;
}

/* category-routed sync: match Palm category labels to same-named collections */
static int cmd_synccat(DavCtx d,int kind,const char*pdb,ConflictPolicy pol){
    uint8_t ai[512]; int al=pdb_read_appinfo(pdb,ai,sizeof ai);
    CatTable ct; if(al<=0 || appinfo_parse(ai,al,&ct)!=0){ fprintf(stderr,"no category table in %s\n",pdb); return 1; }
    Colls cs={0}; int n=resolveColls(&d,'c',&cs);   /* cal + todo both live under calendar-home */
    if(n<0){ fprintf(stderr,"discovery failed (check credentials)\n"); return 1; }

    static char paths[CAT_COUNT][256];
    CatRoute rt; memset(&rt,0,sizeof rt);
    const char*defEnv=getenv("DAV_CAL");
    printf("category routing:\n");
    for(int c=0;c<CAT_COUNT;c++){
        if(!ct.name[c][0]) continue;
        for(int j=0;j<cs.n;j++) if(!strcasecmp(ct.name[c],cs.name[j])){
            trimColl(cs.path[j],paths[c],sizeof paths[c]); rt.coll[c]=paths[c];
            printf("  [%2d] %-16s -> %s\n",c,ct.name[c],paths[c]); break; }
        if(!rt.coll[c]) printf("  [%2d] %-16s -> (default)\n",c,ct.name[c]);
    }
    rt.def = rt.coll[0] ? rt.coll[0] : defEnv;   /* Unfiled's collection, else DAV_CAL */
    if(!rt.def){ fprintf(stderr,"no default collection: name a calendar 'Unfiled' or set DAV_CAL\n"); return 1; }
    SyncStats s={0};
    sync_categorized(&d,pdb,pdb,kind,&rt,"state",pol,&s);
    printf("synced: +%d ~%d -%d push | +%d ~%d -%d pull | %d conflict | %d clean\n",
        s.pushNew,s.pushMod,s.pushDel,s.pullNew,s.pullMod,s.pullDel,s.conflicts,s.unchanged);
    return 0;
}

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: %s push|pull ...\n",argv[0]); return 2; }
    DavCtx d; memset(&d,0,sizeof d);
    snprintf(d.base,sizeof d.base,"%s",env("DAV_BASE","http://localhost:5232"));
    snprintf(d.user,sizeof d.user,"%s",env("DAV_USER","palm"));
    snprintf(d.pass,sizeof d.pass,"%s",env("DAV_PASS","palm"));
    const char*cal =env("DAV_CAL","palm/cal");
    const char*card=env("DAV_CARD","palm/card");
    ical_set_tz(getenv("BRIDGE_TZ"));   /* NULL/unset => floating local time */

    if(!strcmp(argv[1],"push")){
        const char*calpdb =argc>2?argv[2]:"pdb/DatebookDB.pdb";
        const char*addrpdb=argc>3?argv[3]:"pdb/AddressDB.pdb";
        int a=sync_push(&d,calpdb,cal,1);
        int b=sync_push(&d,addrpdb,card,0);
        printf("pushed %d events, %d contacts\n",a,b);
        return 0;
    }
    if(!strcmp(argv[1],"pull")){
        const char*calout =argc>2?argv[2]:"pdb/DatebookDB.down.pdb";
        const char*addrout=argc>3?argv[3]:"pdb/AddressDB.down.pdb";
        int a=sync_pull(&d,cal,calout,1);
        int b=sync_pull(&d,card,addrout,0);
        printf("pulled %d events -> %s, %d contacts -> %s\n",a,calout,b,addrout);
        return 0;
    }
    if(!strcmp(argv[1],"sync")){
        const char*calpdb =argc>2?argv[2]:"pdb/DatebookDB.pdb";
        const char*addrpdb=argc>3?argv[3]:"pdb/AddressDB.pdb";
        const char*pols   =argc>4?argv[4]:"server";
        ConflictPolicy pol = !strcmp(pols,"local")?POL_LOCAL : !strcmp(pols,"both")?POL_BOTH : POL_SERVER;
        SyncStats sa={0}, sb={0};
        sync_collection(&d,calpdb,calpdb,cal,1,"state/cal.map",pol,&sa);
        sync_collection(&d,addrpdb,addrpdb,card,0,"state/card.map",pol,&sb);
        printf("cal : +%d ~%d -%d push | +%d ~%d -%d pull | %d conflict | %d clean\n",
               sa.pushNew,sa.pushMod,sa.pushDel,sa.pullNew,sa.pullMod,sa.pullDel,sa.conflicts,sa.unchanged);
        printf("card: +%d ~%d -%d push | +%d ~%d -%d pull | %d conflict | %d clean\n",
               sb.pushNew,sb.pushMod,sb.pushDel,sb.pullNew,sb.pullMod,sb.pullDel,sb.conflicts,sb.unchanged);
        return 0;
    }
    if(!strcmp(argv[1],"discover")){
        return cmd_discover(d);
    }
    if(!strcmp(argv[1],"synccat") && argc>=4){
        int kind = !strcmp(argv[2],"card")?KIND_CARD : !strcmp(argv[2],"todo")?KIND_TODO : KIND_CAL;
        const char*pols = argc>4?argv[4]:"server";
        ConflictPolicy pol = !strcmp(pols,"local")?POL_LOCAL : !strcmp(pols,"both")?POL_BOTH : POL_SERVER;
        return cmd_synccat(d,kind,argv[3],pol);
    }
    if(!strcmp(argv[1],"dump") && argc>=4){
        int isCal=!strcmp(argv[2],"cal");
        pdb_read(argv[3], isCal?dumpCal:dumpCard, NULL);
        return 0;
    }
    fprintf(stderr,"unknown command '%s'\n",argv[1]);
    return 2;
}
