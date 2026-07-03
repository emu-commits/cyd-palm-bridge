/* addr_prototype.c -- CardDAV half of the spike: AddressDB.pdb -> vCard 3.0.
 *
 * Same clean seam as the DateBook spike: own PDB reader, own unpack, no
 * PumpkinOS DataMgr/MemHandle, one record in RAM at a time.
 *
 * Unpack logic ported from PumpkinOS src/AddressBook/AddressDB.c
 * (PrvAddrDBUnpack, orig Palm SDK, Roger Flores 1/11/95), made
 * endianness-correct (BE reads instead of struct casts).
 *
 * The one non-obvious Palm quirk this proves we handle:
 *   - There is NO email field. Email is a phone slot whose 4-bit label in
 *     options.phoneBits == emailLabel. So TEL-vs-EMAIL is decided by label,
 *     not by field index. Getting this wrong silently drops every email.
 *
 * Build: cc -Wall -O2 -o addr_prototype addr_prototype.c   ;  ./addr_prototype
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* field indices (AddressDB.h) */
enum { F_name, F_firstName, F_company, F_phone1, F_phone2, F_phone3, F_phone4,
       F_phone5, F_address, F_city, F_state, F_zip, F_country, F_title,
       F_custom1, F_custom2, F_custom3, F_custom4, F_note, F_COUNT };

/* phone-label enum (AddressDB.h): index into these per phone slot */
enum { workLabel, homeLabel, faxLabel, otherLabel, emailLabel, mainLabel,
       pagerLabel, mobileLabel };
static const char *TELTYPE[8] = {"WORK","HOME","FAX","VOICE","","MAIN","PAGER","CELL"};

/* bitPos[]: field index -> bit in the 32-bit flags word (AddressDB.c) */
static const int bitPos[32] = {
  24,25,26,27,28,29,30,31, 16,17,18,19,20,21,22,23, 8,9,10,11,12,13,14,15, 0,1,2,3,4,5,6,7 };

static uint16_t be16(const uint8_t*p){return (p[0]<<8)|p[1];}
static uint32_t be32(const uint8_t*p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|(p[2]<<8)|p[3];}

typedef struct {
    const char *fields[F_COUNT];   /* into the record buffer, or NULL */
    int phoneLabel[5];             /* label index per phone slot      */
} Addr;

/* ---- the port: raw packed AddressDB record bytes -> Addr ---- */
static void AddrUnpack(const uint8_t *r, Addr *a){
    memset(a,0,sizeof *a);
    uint32_t phoneBits = be32(r+0);      /* options: 5 nibbles from MSB + display */
    uint32_t flags     = be32(r+4);      /* which fields present            */
    /* r[8] = companyFieldOffset (sort optimization) -- unused for export */
    for(int i=0;i<5;i++) a->phoneLabel[i] = (phoneBits >> (28 - i*4)) & 0xF;

    const char *p = (const char*)(r + 9); /* firstField */
    for(int i=0;i<F_COUNT;i++){
        if(flags & (1u << bitPos[i])){ a->fields[i]=p; p += strlen(p)+1; }
        else a->fields[i]=NULL;
    }
}

/* ---- vCard 3.0 emitter ---- */
static const char *S(const char*x){return x?x:"";}
static void emitVCARD(const Addr*a){
    printf("BEGIN:VCARD\r\nVERSION:3.0\r\n");
    printf("N:%s;%s;;%s;\r\n", S(a->fields[F_name]), S(a->fields[F_firstName]), S(a->fields[F_title]));
    if(a->fields[F_firstName]||a->fields[F_name])
        printf("FN:%s%s%s\r\n", S(a->fields[F_firstName]),
               (a->fields[F_firstName]&&a->fields[F_name])?" ":"", S(a->fields[F_name]));
    else if(a->fields[F_company]) printf("FN:%s\r\n", a->fields[F_company]);
    if(a->fields[F_company]) printf("ORG:%s\r\n", a->fields[F_company]);
    if(a->fields[F_title])   printf("TITLE:%s\r\n", a->fields[F_title]);

    for(int i=0;i<5;i++){
        const char *v = a->fields[F_phone1+i];
        if(!v) continue;
        if(a->phoneLabel[i]==emailLabel) printf("EMAIL:%s\r\n", v);       /* <- the quirk */
        else printf("TEL;TYPE=%s:%s\r\n", TELTYPE[a->phoneLabel[i]], v);
    }
    if(a->fields[F_address]||a->fields[F_city]||a->fields[F_state]||a->fields[F_zip]||a->fields[F_country])
        printf("ADR;TYPE=WORK:;;%s;%s;%s;%s;%s\r\n", S(a->fields[F_address]),
               S(a->fields[F_city]), S(a->fields[F_state]), S(a->fields[F_zip]), S(a->fields[F_country]));
    if(a->fields[F_note]) printf("NOTE:%s\r\n", a->fields[F_note]);
    printf("END:VCARD\r\n");
}

/* ================= sample-data generator (packs a real AddressDB.pdb) ===== */
static void put32(uint8_t*p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static void put16(uint8_t*p,uint16_t v){p[0]=v>>8;p[1]=v&0xFF;}

/* pack one record given field strings[F_COUNT] (NULL=absent) + phone labels */
static int packAddr(uint8_t*buf,const char*fields[F_COUNT],const int phoneLabel[5]){
    uint32_t phoneBits=0; for(int i=0;i<5;i++) phoneBits |= ((uint32_t)(phoneLabel[i]&0xF))<<(28-i*4);
    uint32_t flags=0; for(int i=0;i<F_COUNT;i++) if(fields[i]) flags |= (1u<<bitPos[i]);
    put32(buf+0,phoneBits); put32(buf+4,flags);
    int n=9, companyOff=0;
    for(int i=0;i<F_COUNT;i++) if(fields[i]){
        if(i==F_company) companyOff=n-9;
        int l=strlen(fields[i])+1; memcpy(buf+n,fields[i],l); n+=l;
    }
    buf[8]=(uint8_t)companyOff;
    return n;
}

static void writeSamplePDB(const char*path){
    uint8_t rec[2][512]; int rlen[2];
    /* contact 1: person with work phone, mobile, and an EMAIL-labelled slot */
    const char* c1[F_COUNT]={0};
    c1[F_name]="Smith"; c1[F_firstName]="John"; c1[F_company]="Acme Corp";
    c1[F_phone1]="555-1000"; c1[F_phone2]="555-2000"; c1[F_phone3]="john@acme.com";
    c1[F_address]="1 Main St"; c1[F_city]="Portland"; c1[F_state]="OR"; c1[F_zip]="97201";
    c1[F_title]="Engineer"; c1[F_note]="met at conference";
    int l1[5]={workLabel, mobileLabel, emailLabel, 0, 0};
    rlen[0]=packAddr(rec[0],c1,l1);
    /* contact 2: company-only card (no name fields) -> tests flag/absence logic */
    const char* c2[F_COUNT]={0};
    c2[F_company]="Widgets Inc"; c2[F_phone1]="800-555-9999";
    int l2[5]={mainLabel,0,0,0,0};
    rlen[1]=packAddr(rec[1],c2,l2);

    int nrec=2, hdr=78, idx=8*nrec+2;
    uint8_t H[78]; memset(H,0,sizeof H);
    memcpy(H,"AddressDB",9);
    put32(H+0x3C,0x44415441); /* 'DATA' */ put32(H+0x40,0x61646472); /* 'addr' creator */
    put16(H+0x4C,nrec);
    FILE*f=fopen(path,"wb"); fwrite(H,1,78,f);
    int off=hdr+idx; uint8_t e[8];
    for(int i=0;i<nrec;i++){put32(e,off);e[4]=0;e[5]=e[6]=0;e[7]=i+1;fwrite(e,1,8,f);off+=rlen[i];}
    uint8_t pad[2]={0,0}; fwrite(pad,1,2,f);
    for(int i=0;i<nrec;i++) fwrite(rec[i],1,rlen[i],f);
    fclose(f);
}

static void streamPDB(const char*path){
    FILE*f=fopen(path,"rb"); if(!f){perror(path);exit(1);}
    uint8_t H[78]; if(fread(H,1,78,f)!=78){fprintf(stderr,"short\n");exit(1);}
    int nrec=be16(H+0x4C);
    uint32_t *off=malloc(sizeof(uint32_t)*(nrec+1));
    for(int i=0;i<nrec;i++){uint8_t e[8]; if(fread(e,1,8,f)!=8)exit(1); off[i]=be32(e);}
    fseek(f,0,SEEK_END); off[nrec]=ftell(f);
    uint8_t buf[4096];                       /* ONE record at a time */
    for(int i=0;i<nrec;i++){
        int len=off[i+1]-off[i]; if(len>(int)sizeof buf)len=sizeof buf;
        fseek(f,off[i],SEEK_SET); if(fread(buf,1,len,f)!=(size_t)len)exit(1);
        buf[len]=0; buf[len+1]=0;
        Addr a; AddrUnpack(buf,&a); emitVCARD(&a);
    }
    free(off); fclose(f);
    fprintf(stderr,"[peak record buffer: %zu bytes for %d contacts]\n",sizeof buf,nrec);
}

int main(void){ const char*p="AddressDB.pdb"; writeSamplePDB(p); streamPDB(p); return 0; }
