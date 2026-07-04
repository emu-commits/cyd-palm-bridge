/* data.c -- read PDB records from the SD card and hand them to the UI as display
 * strings. Also seeds demo PDBs so the views have content before a HotSync (U7).
 * Uses the shared codec (bridge component) directly; one record in RAM at a time.
 */
#include "data.h"
#include "palm.h"
#include <stdio.h>
#include <string.h>

#define DB_CAL  "/sdcard/DatebookDB.pdb"
#define DB_ADDR "/sdcard/AddressDB.pdb"
#define DB_TODO "/sdcard/ToDoDB.pdb"

static int file_exists(const char *path){
    FILE *f = fopen(path, "rb");
    if(f){ fclose(f); return 1; }
    return 0;
}

/* ------------------------- demo seeding -------------------------
 * Enough rows to overflow the list (scroll test). This overwrites each boot for
 * now; U7 HotSync replaces it with real synced data. */
#define SEEDMAX 32
static void seed_datebook(void){
    static uint8_t arena[SEEDMAX*96]; PdbRec r[SEEDMAX]; int nr=0, used=0;
    struct { const char *s; int mo, d, h; } ev[] = {
        {"Standup",8,3,9},{"1:1 with Sam",8,3,14},{"Groceries",8,4,18},{"Dentist",8,5,10},
        {"Team lunch",8,5,12},{"Code review",8,6,15},{"Gym",8,6,7},{"Call Mom",8,7,19},
        {"Project sync",8,7,11},{"Lunch w/ Ada",8,8,12},{"Haircut",8,9,16},{"Board meeting",8,10,9},
        {"Pay rent",8,11,8},{"Movie night",8,12,20},{"Flight to NYC",8,14,6} };
    int n=(int)(sizeof(ev)/sizeof(ev[0]));
    for(int i=0;i<n && nr<SEEDMAX;i++){
        Appt a; memset(&a,0,sizeof a);
        a.hasTime=1; a.sH=ev[i].h; a.eH=ev[i].h+1;
        a.year=2026; a.month=ev[i].mo; a.day=ev[i].d;
        snprintf(a.description,sizeof a.description,"%s",ev[i].s);
        int l=ApptPack(arena+used,96,&a);
        if(l>0){ r[nr]=(PdbRec){.attr=0,.uniqueID=(uint32_t)(i+1),.data=arena+used,.len=l}; used+=l; nr++; }
    }
    pdb_write_ai(DB_CAL,"DatebookDB",0x44415441,0x64617465,NULL,0,r,nr);
}

static void seed_address(void){
    static uint8_t arena[SEEDMAX*96]; PdbRec r[SEEDMAX]; int nr=0, used=0;
    struct { const char *last,*first,*co,*phone; } pc[] = {
        {"Appleseed","Johnny","Acme Corp","555-0100"},{"Bramble","Rosa","Widgets Inc","555-0200"},
        {"Chen","Mei","","555-0300"},{"Okafor","Ada","Globex","555-0400"},
        {"Diaz","Luis","Initech","555-0500"},{"Novak","Petra","","555-0600"},
        {"Kim","Soo","Umbrella","555-0700"},{"Patel","Raj","Hooli","555-0800"},
        {"Silva","Ana","","555-0900"},{"Weber","Tom","Stark Ind","555-1000"},
        {"Yamada","Emi","Cyberdyne","555-1100"},{"Zola","Kofi","","555-1200"} };
    int n=(int)(sizeof(pc)/sizeof(pc[0]));
    for(int i=0;i<n && nr<SEEDMAX;i++){
        Addr a; memset(&a,0,sizeof a);
        a.fields[F_name]=AddrIntern(&a,pc[i].last);
        a.fields[F_firstName]=AddrIntern(&a,pc[i].first);
        if(pc[i].co[0]) a.fields[F_company]=AddrIntern(&a,pc[i].co);
        a.fields[F_phone1]=AddrIntern(&a,pc[i].phone); a.phoneLabel[0]=workLabel;
        int l=AddrPack(arena+used,96,&a);
        if(l>0){ r[nr]=(PdbRec){.attr=0,.uniqueID=(uint32_t)(i+1),.data=arena+used,.len=l}; used+=l; nr++; }
    }
    pdb_write_ai(DB_ADDR,"AddressDB",0x44415441,0x61646472,NULL,0,r,nr);
}

static void seed_todo(void){
    static uint8_t arena[SEEDMAX*96]; PdbRec r[SEEDMAX]; int nr=0, used=0;
    struct { const char *s; int pr, done; } td[] = {
        {"Buy stamps",2,0},{"Renew passport",1,0},{"Call plumber",3,0},{"File taxes",1,1},
        {"Water plants",4,0},{"Read RFC 6578",2,1},{"Fix bike tire",3,0},{"Book dentist",2,0},
        {"Reply to Ada",1,0},{"Backup laptop",2,1} };
    int n=(int)(sizeof(td)/sizeof(td[0]));
    for(int i=0;i<n && nr<SEEDMAX;i++){
        Todo t; memset(&t,0,sizeof t);
        t.priority=td[i].pr; t.completed=td[i].done;
        snprintf(t.description,sizeof t.description,"%s",td[i].s);
        int l=ToDoPack(arena+used,96,&t);
        if(l>0){ r[nr]=(PdbRec){.attr=0,.uniqueID=(uint32_t)(i+1),.data=arena+used,.len=l}; used+=l; nr++; }
    }
    pdb_write_ai(DB_TODO,"ToDoDB",0x44415441,0x746F646F,NULL,0,r,nr);
}

void data_seed_if_empty(void){
    (void)file_exists;   /* overwrite each boot for now (demo data; U7 replaces) */
    seed_datebook();
    seed_address();
    seed_todo();
}

/* ------------------------- iteration ------------------------- */
typedef struct { data_row_cb cb; void *ctx; } It;

static int cbCal(const PdbRec *r, int i, void *ctx){
    (void)i; It *it=ctx; Appt a;
    if(ApptUnpack(r->data,r->len,&a)) return 0;
    char pri[96];
    if(a.hasTime) snprintf(pri,sizeof pri,"%d/%d %d:%02d  %.72s",a.month,a.day,a.sH,a.sM,a.description);
    else          snprintf(pri,sizeof pri,"%d/%d  %.72s",a.month,a.day,a.description);
    it->cb(pri, NULL, it->ctx);
    return 0;
}
void data_datebook(data_row_cb cb, void *ctx){ It it={cb,ctx}; pdb_read(DB_CAL,cbCal,&it); }

static int cbAddr(const PdbRec *r, int i, void *ctx){
    (void)i; It *it=ctx; Addr a;
    if(AddrUnpack(r->data,r->len,&a)) return 0;
    const char *last=a.fields[F_name], *first=a.fields[F_firstName], *co=a.fields[F_company];
    char pri[96];
    if(last && first) snprintf(pri,sizeof pri,"%.40s, %.40s",last,first);
    else if(last)     snprintf(pri,sizeof pri,"%.80s",last);
    else if(co)       snprintf(pri,sizeof pri,"%.80s",co);
    else              snprintf(pri,sizeof pri,"(no name)");
    it->cb(pri, a.fields[F_phone1], it->ctx);
    return 0;
}
void data_address(data_row_cb cb, void *ctx){ It it={cb,ctx}; pdb_read(DB_ADDR,cbAddr,&it); }

static int cbTodo(const PdbRec *r, int i, void *ctx){
    (void)i; It *it=ctx; Todo t;
    if(ToDoUnpack(r->data,r->len,&t)) return 0;
    char pri[96];
    snprintf(pri,sizeof pri,"%s%.80s",t.completed?"[x] ":"[ ] ",t.description);
    char sec[16]; snprintf(sec,sizeof sec,"pri %d",t.priority);
    it->cb(pri, sec, it->ctx);
    return 0;
}
void data_todo(data_row_cb cb, void *ctx){ It it={cb,ctx}; pdb_read(DB_TODO,cbTodo,&it); }
