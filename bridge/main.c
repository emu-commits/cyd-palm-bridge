/* main.c -- headless bridge CLI.
 *
 *   bridge_cli discover                            -> find collection URLs (iCloud etc.)
 *   bridge_cli push  <cal-pdb> <addr-pdb>          -> full upload (seed)
 *   bridge_cli pull  <cal-out> <addr-out>          -> full download
 *   bridge_cli sync  <cal-pdb> <addr-pdb> [policy] -> incremental two-way (RFC 6578)
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
#include "palm.h"
#include "dav.h"
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

typedef struct { const char*label; } DiscCtx;
static void discCb(const char*href,int kind,const char*dn,void*ctx){
    (void)ctx;
    const char*k = kind=='c'?"CALENDAR   ":kind=='a'?"ADDRESSBOOK":"(other)    ";
    if(kind) printf("  %s  %-28s  %s\n",k,dn[0]?dn:"(unnamed)",href);
}

static int cmd_discover(DavCtx d){
    /* follow any redirect to the real host (iCloud: caldav.icloud.com -> pNN-...) */
    char host[256]; if(dav_effective_host(&d,"/",host,sizeof host)==0 && host[0]){
        snprintf(d.base,sizeof d.base,"%s",host);
        printf("host: %s\n",d.base);
    }
    char principal[512]="";
    if(dav_prop_href(&d,"/","<d:current-user-principal/>","",principal,sizeof principal)!=0 || !principal[0]){
        fprintf(stderr,"could not resolve current-user-principal (check credentials)\n"); return 1;
    }
    char*ppath=absPath(principal,&d);
    printf("principal: %s%s\n",d.base,ppath);

    char calHome[512]="", cardHome[512]="";
    dav_prop_href(&d,ppath,"<c:calendar-home-set/>",
                  "xmlns:c=\"urn:ietf:params:xml:ns:caldav\"",calHome,sizeof calHome);
    dav_prop_href(&d,ppath,"<e:addressbook-home-set/>",
                  "xmlns:e=\"urn:ietf:params:xml:ns:carddav\"",cardHome,sizeof cardHome);

    if(calHome[0]){ char*cp=absPath(calHome,&d); printf("\ncalendars under %s%s:\n",d.base,cp);
        dav_list_collections(&d,cp,discCb,NULL); }
    if(cardHome[0]){ char*cp=absPath(cardHome,&d); printf("\naddressbooks under %s%s:\n",d.base,cp);
        dav_list_collections(&d,cp,discCb,NULL); }

    printf("\nTo sync a collection, set DAV_BASE to the host above and DAV_CAL/DAV_CARD\n"
           "to the collection path (minus the host), then run: bridge_cli sync ...\n");
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
    if(!strcmp(argv[1],"dump") && argc>=4){
        int isCal=!strcmp(argv[2],"cal");
        pdb_read(argv[3], isCal?dumpCal:dumpCard, NULL);
        return 0;
    }
    fprintf(stderr,"unknown command '%s'\n",argv[1]);
    return 2;
}
