/* vcard.c -- Addr <-> vCard 3.0.
 *
 * emit: proven in the original spike.
 * parse: vCard -> Addr (download direction). Maps TEL/EMAIL back into Palm's
 *   5 phone slots with the right label (the inverse of the email-is-a-phone
 *   quirk), and ADR/N/ORG/TITLE/NOTE into the fixed Palm fields.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "palm.h"
#include "charset.h"

static const char *TELTYPE[8]={"WORK","HOME","FAX","VOICE","","MAIN","PAGER","CELL"};
static const char *S(const char*x){return x?x:"";}

/* CP1252(Palm) -> UTF-8 for output, using a small rotating buffer pool so
 * several fields can be converted within a single snprintf call.           */
static const char *u8(const char*palm){
    static char pool[8][512]; static int r=0;
    char *b=pool[r++&7]; cp1252_to_utf8(b,512,palm?palm:""); return b;
}
/* UTF-8(server) -> CP1252, then intern into the Addr store. */
static const char *internU8(Addr*a,const char*val){
    char cp[512]; utf8_to_cp1252(cp,sizeof cp,val?val:""); return AddrIntern(a,cp);
}

int vcard_emit(char *out,int cap,const Addr *a,uint32_t uid){
    int n=snprintf(out,cap,"BEGIN:VCARD\r\nVERSION:3.0\r\nUID:palm-%u@cyd\r\n",uid);
    n+=snprintf(out+n,cap-n,"N:%s;%s;;%s;\r\n",u8(S(a->fields[F_name])),u8(S(a->fields[F_firstName])),u8(S(a->fields[F_title])));
    if(a->fields[F_firstName]||a->fields[F_name])
        n+=snprintf(out+n,cap-n,"FN:%s%s%s\r\n",u8(S(a->fields[F_firstName])),
                    (a->fields[F_firstName]&&a->fields[F_name])?" ":"",u8(S(a->fields[F_name])));
    else if(a->fields[F_company]) n+=snprintf(out+n,cap-n,"FN:%s\r\n",u8(a->fields[F_company]));
    if(a->fields[F_company]) n+=snprintf(out+n,cap-n,"ORG:%s\r\n",u8(a->fields[F_company]));
    if(a->fields[F_title])   n+=snprintf(out+n,cap-n,"TITLE:%s\r\n",u8(a->fields[F_title]));
    for(int i=0;i<5;i++){
        const char *v=a->fields[F_phone1+i]; if(!v) continue;
        if(a->phoneLabel[i]==emailLabel) n+=snprintf(out+n,cap-n,"EMAIL:%s\r\n",u8(v));
        else n+=snprintf(out+n,cap-n,"TEL;TYPE=%s:%s\r\n",TELTYPE[a->phoneLabel[i]],u8(v));
    }
    if(a->fields[F_address]||a->fields[F_city]||a->fields[F_state]||a->fields[F_zip]||a->fields[F_country])
        n+=snprintf(out+n,cap-n,"ADR;TYPE=WORK:;;%s;%s;%s;%s;%s\r\n",u8(S(a->fields[F_address])),
                    u8(S(a->fields[F_city])),u8(S(a->fields[F_state])),u8(S(a->fields[F_zip])),u8(S(a->fields[F_country])));
    if(a->fields[F_note]) n+=snprintf(out+n,cap-n,"NOTE:%s\r\n",u8(a->fields[F_note]));
    n+=snprintf(out+n,cap-n,"END:VCARD\r\n");
    return n;
}

/* nth ';'-separated component of s into dst (empty -> not set) */
static const char *comp(Addr *a,const char *s,int idx){
    for(int i=0;i<idx && s;i++){ s=strchr(s,';'); if(s)s++; }
    if(!s||!*s) return NULL;
    char tmp[256]; int j=0;
    for(;s[j] && s[j]!=';' && j<(int)sizeof tmp-1;j++) tmp[j]=s[j];
    tmp[j]=0;
    if(!tmp[0]) return NULL;
    return internU8(a,tmp);
}

static int telLabel(const char *params){
    if(strstr(params,"CELL")||strstr(params,"cell")) return mobileLabel;
    if(strstr(params,"HOME")||strstr(params,"home")) return homeLabel;
    if(strstr(params,"FAX")||strstr(params,"fax"))   return faxLabel;
    if(strstr(params,"MAIN")||strstr(params,"main")) return mainLabel;
    if(strstr(params,"PAGER")||strstr(params,"pager"))return pagerLabel;
    if(strstr(params,"WORK")||strstr(params,"work")) return workLabel;
    return otherLabel;
}

int vcard_parse(const char *vcf, Addr *a){
    memset(a,0,sizeof *a);
    int L=(int)strlen(vcf);
    char *buf=malloc(L+1); if(!buf) return -1;
    int w=0;
    for(int i=0;i<L;i++){
        if(vcf[i]=='\r'||vcf[i]=='\n'){
            int j=i; while(j<L && (vcf[j]=='\r'||vcf[j]=='\n')) j++;
            if(j<L && (vcf[j]==' '||vcf[j]=='\t')){ i=j; continue; }
            buf[w++]='\n'; i=j-1;
        } else buf[w++]=vcf[i];
    }
    buf[w]=0;

    int slot=0, have=0;
    char *save=NULL;
    for(char *line=strtok_r(buf,"\n",&save); line; line=strtok_r(NULL,"\n",&save)){
        char *colon=strchr(line,':'); if(!colon) continue;
        *colon=0; char *val=colon+1;
        char *semi=strchr(line,';'); char params[128]="";
        if(semi){ *semi=0; strncpy(params,semi+1,sizeof params-1); }
        char *name=line;

        if(!strcmp(name,"N")){
            a->fields[F_name]      = comp(a,val,0);   /* family */
            a->fields[F_firstName] = comp(a,val,1);   /* given  */
            const char *t = comp(a,val,3);            /* honorific-prefix -> title fallback */
            if(t && !a->fields[F_title]) a->fields[F_title]=t;
            have=1;
        } else if(!strcmp(name,"FN")){ have=1; /* display name; N is authoritative for Palm */
        } else if(!strcmp(name,"ORG")){
            a->fields[F_company]=comp(a,val,0);
        } else if(!strcmp(name,"TITLE")){
            a->fields[F_title]=internU8(a,val);
        } else if(!strcmp(name,"TEL")){
            if(slot<5){ a->fields[F_phone1+slot]=internU8(a,val); a->phoneLabel[slot]=telLabel(params); slot++; }
        } else if(!strcmp(name,"EMAIL")){
            if(slot<5){ a->fields[F_phone1+slot]=internU8(a,val); a->phoneLabel[slot]=emailLabel; slot++; }
        } else if(!strcmp(name,"ADR")){
            a->fields[F_address]=comp(a,val,2);
            a->fields[F_city]   =comp(a,val,3);
            a->fields[F_state]  =comp(a,val,4);
            a->fields[F_zip]    =comp(a,val,5);
            a->fields[F_country]=comp(a,val,6);
        } else if(!strcmp(name,"NOTE")){
            a->fields[F_note]=internU8(a,val);
        }
    }
    free(buf);
    return have ? 0 : -1;
}
