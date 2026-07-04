/* main.c -- headless bridge CLI.
 *
 *   bridge_cli discover                            -> find collection URLs (iCloud etc.)
 *   bridge_cli push  <cal-pdb> <addr-pdb>          -> full upload (seed)
 *   bridge_cli pull  <cal-out> <addr-out>          -> full download
 *   bridge_cli sync  <cal-pdb> <addr-pdb> [policy] -> incremental two-way (RFC 6578)
 *   bridge_cli synccat cal|todo|card <pdb> [pol]   -> category-routed sync (name match)
 *   bridge_cli synccontacts <pdb> [policy]         -> contact sync (single address book)
 *   bridge_cli demo cal|todo|card <pdb>            -> write a categorized sample PDB
 *   bridge_cli dump  cal|card <pdb>                -> canonical text dump
 *
 * DAV target from env: DAV_BASE, DAV_USER, DAV_PASS, DAV_CAL, DAV_CARD, BRIDGE_TZ.
 * Contacts (iCloud) are on a separate host: DAV_CARD_BASE (default contacts.icloud.com).
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

/* iCloud CardDAV lives on a DIFFERENT host than CalDAV: caldav.icloud.com serves
 * calendars only, contacts are on contacts.icloud.com. Return a copy of `d` whose
 * base points at the contacts host:
 *   - DAV_CARD_BASE if set (explicit override),
 *   - else if DAV_BASE contains "caldav.icloud.com", swap "caldav" -> "contacts",
 *   - else leave base unchanged (self-hosted DAV usually shares one host).       */
static DavCtx cardBase(DavCtx d){
    const char*ov=getenv("DAV_CARD_BASE");
    if(ov&&*ov){ snprintf(d.base,sizeof d.base,"%s",ov); return d; }
    char*cd=strstr(d.base,"caldav.icloud.com");
    if(cd){
        char out[256]; int pre=(int)(cd-d.base);
        if(pre<(int)sizeof out)
            snprintf(out,sizeof out,"%.*s%s%s",pre,d.base,"contacts",cd+strlen("caldav"));
        snprintf(d.base,sizeof d.base,"%s",out);
    }
    return d;
}

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
    if(strstr(href,"/notification")) return;   /* iCloud internal pseudo-collection */
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

static void diagnose(const DavCtx*d){
    fprintf(stderr,"\n-- discovery failed --\nhost tried: %s\nHTTP status: %d\n",d->base,dav_last_status);
    FILE*f=fopen("state/.drep","rb");
    if(f){ char b[900]; int n=(int)fread(b,1,sizeof b-1,f); b[n>0?n:0]=0; fclose(f);
        if(n>0) fprintf(stderr,"server said:\n%.900s\n",b); }
    if(dav_last_status==401)
        fprintf(stderr,"\n401 = auth rejected. For iCloud: DAV_USER must be your full Apple ID email,\n"
                       "and DAV_PASS an APP-SPECIFIC password (appleid.apple.com > Sign-In & Security >\n"
                       "App-Specific Passwords), entered WITH its dashes. The normal password is blocked by 2FA.\n");
    else if(dav_last_status==0)
        fprintf(stderr,"\nstatus 0 = no HTTP response (DNS/TLS/proxy). Check network and that DAV_BASE is https.\n");
    else if(dav_last_status>=300 && dav_last_status<400)
        fprintf(stderr,"\n%d = redirect not resolved. Try DAV_BASE=https://caldav.icloud.com (no trailing path).\n",dav_last_status);
    else if(dav_last_status==207)
        fprintf(stderr,"\n207 = the server answered but no current-user-principal href was found (namespace/shape).\n");
}

static int cmd_discover(DavCtx d){
    Colls cal={0}, card={0};
    DavCtx dc=d; int nc=resolveColls(&dc,'c',&cal);
    if(nc<0){ diagnose(&dc); return 1; }
    printf("host: %s\ncalendars:\n",dc.base);
    for(int i=0;i<cal.n;i++) printf("  %-28s  %s\n",cal.name[i][0]?cal.name[i]:"(unnamed)",cal.path[i]);
    DavCtx da=cardBase(d); resolveColls(&da,'a',&card);   /* contacts on separate iCloud host */
    printf("addressbooks (host: %s):\n",da.base);
    for(int i=0;i<card.n;i++) printf("  %-28s  %s\n",card.name[i][0]?card.name[i]:"(unnamed)",card.path[i]);
    printf("\nSet DAV_BASE to the host and DAV_CAL/DAV_CARD to a path, then `sync`;\n"
           "or use `synccat` to auto-route Palm categories to same-named calendars.\n");
    return 0;
}

/* write a small categorized sample PDB so `synccat` has category labels to
 * route (real Palm DBs carry these; the test samples do not).                */
static int cmd_demo(int kind,const char*path){
    CatTable t; memset(&t,0,sizeof t);
    strcpy(t.name[0],"Unfiled");
    if(kind==KIND_TODO) strcpy(t.name[1],"Reminders");
    else if(kind==KIND_CARD){ strcpy(t.name[1],"Business"); strcpy(t.name[2],"Personal"); }
    else { strcpy(t.name[1],"Work"); strcpy(t.name[2],"Home"); }
    uint8_t ai[APPINFO_SIZE]; int al=appinfo_build(ai,sizeof ai,&t);
    static uint8_t arena[8*PALM_REC_MAX]; PdbRec r[8]; int nr=0, used=0;
    #define REC(uid,cat,LN) do{ r[nr]=(PdbRec){ .attr=(uint8_t)(cat),.uniqueID=(uid),.data=arena+used,.len=(LN) }; used+=(LN); nr++; }while(0)
    if(kind==KIND_TODO){
        struct{ const char*s; int cat; int y,m,d,pr; } td[]={
            {"Buy stamps",1,2026,8,10,2},{"Renew passport",1,0,0,0,1} };
        for(int i=0;i<2;i++){ Todo t2; memset(&t2,0,sizeof t2);
            if(td[i].y){ t2.hasDue=1; t2.dueY=td[i].y; t2.dueM=td[i].m; t2.dueD=td[i].d; }
            t2.priority=td[i].pr; snprintf(t2.description,sizeof t2.description,"%s",td[i].s);
            int l=ToDoPack(arena+used,PALM_REC_MAX,&t2); REC((uint32_t)(i+1),td[i].cat,l); }
        pdb_write_ai(path,"ToDoDB",0x44415441,0x746F646F,ai,al,r,nr);
    } else if(kind==KIND_CARD){
        struct{ const char*last,*first,*co,*phone,*email; int cat; } pc[]={
            {"Appleseed","Johnny","Acme Corp","555-0100","johnny@acme.example",1},
            {"Bramble","Rosa","Widgets Inc","555-0200","rosa@widgets.example",2} };
        for(int i=0;i<2;i++){ Addr a; memset(&a,0,sizeof a);
            a.fields[F_name]=AddrIntern(&a,pc[i].last);
            a.fields[F_firstName]=AddrIntern(&a,pc[i].first);
            a.fields[F_company]=AddrIntern(&a,pc[i].co);
            a.fields[F_phone1]=AddrIntern(&a,pc[i].phone); a.phoneLabel[0]=workLabel;
            a.fields[F_phone2]=AddrIntern(&a,pc[i].email); a.phoneLabel[1]=emailLabel;
            a.displayPhone=0;
            int l=AddrPack(arena+used,PALM_REC_MAX,&a); REC((uint32_t)(i+1),pc[i].cat,l); }
        pdb_write_ai(path,"AddressDB",0x44415441,0x61646472,ai,al,r,nr);
    } else {
        struct{ const char*s; int cat; int d,h; } ev[]={
            {"Standup",1,3,9},{"1:1 with Sam",1,4,14},{"Groceries",2,5,18},{"Dentist",2,6,10} };
        for(int i=0;i<4;i++){ Appt a; memset(&a,0,sizeof a); a.hasTime=1; a.sH=ev[i].h; a.eH=ev[i].h+1;
            a.year=2026;a.month=8;a.day=ev[i].d; snprintf(a.description,sizeof a.description,"%s",ev[i].s);
            int l=ApptPack(arena+used,PALM_REC_MAX,&a); REC((uint32_t)(i+1),ev[i].cat,l); }
        pdb_write_ai(path,"DatebookDB",0x44415441,0x64617465,ai,al,r,nr);
    }
    #undef REC
    printf("wrote %s with %d records (categories: %s)\n",path,nr,
        kind==KIND_TODO?"Reminders":kind==KIND_CARD?"Business, Personal":"Work, Home");
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
    static char defbuf[256];
    if(rt.coll[0]) rt.def=rt.coll[0];             /* Unfiled matched a collection   */
    else if(defEnv){ trimColl(defEnv,defbuf,sizeof defbuf); rt.def=defbuf; }  /* DAV_CAL */
    else rt.def=NULL;                             /* no default: skip unmatched cats */
    if(!rt.def) printf("  (no default collection; records in unmatched categories are left local)\n");
    SyncStats s={0};
    sync_categorized(&d,pdb,pdb,kind,&rt,"state",pol,&s);
    printf("synced: +%d ~%d -%d push | +%d ~%d -%d pull | %d conflict | %d clean\n",
        s.pushNew,s.pushMod,s.pushDel,s.pullNew,s.pullMod,s.pullDel,s.conflicts,s.unchanged);
    return 0;
}

/* contact sync: iCloud exposes ONE address book, on a separate host. Categories
 * are preserved on each record (attr nibble) but NOT routed — so this is a plain
 * single-collection sync_collection with KIND_CARD, not sync_categorized.        */
static int cmd_synccontacts(DavCtx d,const char*pdb,ConflictPolicy pol){
    DavCtx cc=cardBase(d);
    Colls cs={0}; int n=resolveColls(&cc,'a',&cs);
    if(n<=0){ fprintf(stderr,"no address book found on %s\n",cc.base);
              if(n<0) diagnose(&cc);
              return 1; }
    char coll[256]; const char*cardEnv=getenv("DAV_CARD");
    if(cardEnv&&*cardEnv) trimColl(cardEnv,coll,sizeof coll);
    else {                                    /* prefer an href ending in /card/, else first */
        int pick=0;
        for(int j=0;j<cs.n;j++){ int l=(int)strlen(cs.path[j]);
            if(l>=6 && !strcmp(cs.path[j]+l-6,"/card/")){ pick=j; break; } }
        trimColl(cs.path[pick],coll,sizeof coll);
    }
    printf("address book: %s (host: %s)\n",coll,cc.base);
    SyncStats s={0};
    sync_collection(&cc,pdb,pdb,coll,KIND_CARD,"state/contacts.map",pol,&s);
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
    if(!strcmp(argv[1],"demo") && argc>=4){
        int kind = !strcmp(argv[2],"todo")?KIND_TODO : !strcmp(argv[2],"card")?KIND_CARD : KIND_CAL;
        return cmd_demo(kind,argv[3]);
    }
    if(!strcmp(argv[1],"synccontacts") && argc>=3){
        const char*pols = argc>3?argv[3]:"server";
        ConflictPolicy pol = !strcmp(pols,"local")?POL_LOCAL : !strcmp(pols,"both")?POL_BOTH : POL_SERVER;
        return cmd_synccontacts(d,argv[2],pol);
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
