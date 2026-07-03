/* address.c -- PalmOS AddressDB record <-> Addr, both directions.
 *
 * On-disk record (big-endian):
 *   [0..3] options: 5 phone-label nibbles (from MSB) + displayPhone nibble
 *   [4..7] flags:   bit set per present field (bitPos[] maps field->bit)
 *   [8]    companyFieldOffset (sort optimization; recomputed on pack)
 *   [9..]  packed null-terminated field strings, in field-index order.
 *
 * Quirk this handles: there is NO email field. Email is a phone slot whose
 * 4-bit label == emailLabel. TEL vs EMAIL is decided by label, not index.
 * Clean-room from the documented layout.
 */
#include <string.h>
#include "palm.h"

/* field index -> bit position in the 32-bit flags word (exactly F_COUNT=19) */
static const int bitPos[F_COUNT] = {
  24,25,26,27,28,29,30,31, 16,17,18,19,20,21,22,23, 8,9,10 };

const char *AddrIntern(Addr *a, const char *s){
    if(!s) return NULL;
    int l=(int)strlen(s)+1;
    if(a->used + l > (int)sizeof a->store) return NULL;
    char *dst = a->store + a->used;
    memcpy(dst,s,l);
    a->used += l;
    return dst;
}

int AddrUnpack(const uint8_t *r, int len, Addr *a){
    if(len < 9) return -1;
    memset(a,0,sizeof *a);
    uint32_t phoneBits = be32(r+0);
    uint32_t flags     = be32(r+4);
    for(int i=0;i<5;i++) a->phoneLabel[i] = (phoneBits >> (28 - i*4)) & 0xF;
    a->displayPhone = (phoneBits >> 8) & 0xF;   /* nibble below the 5 labels */

    const char *p   = (const char*)(r + 9);
    const char *end = (const char*)(r + len);
    for(int i=0;i<F_COUNT;i++){
        if(flags & (1u << bitPos[i])){
            if(p >= end) return -1;
            a->fields[i] = AddrIntern(a, p);
            p += strlen(p)+1;
        } else a->fields[i]=NULL;
    }
    return 0;
}

int AddrPack(uint8_t *buf, int cap, const Addr *a){
    if(cap < 9) return -1;
    uint32_t phoneBits=0;
    for(int i=0;i<5;i++) phoneBits |= ((uint32_t)(a->phoneLabel[i]&0xF)) << (28 - i*4);
    phoneBits |= ((uint32_t)(a->displayPhone & 0xF)) << 8;
    uint32_t flags=0;
    for(int i=0;i<F_COUNT;i++) if(a->fields[i] && a->fields[i][0]) flags |= (1u<<bitPos[i]);
    put32(buf+0, phoneBits);
    put32(buf+4, flags);

    int n=9, companyOff=0;
    for(int i=0;i<F_COUNT;i++){
        if(a->fields[i] && a->fields[i][0]){
            if(i==F_company) companyOff = n-9;
            int l=(int)strlen(a->fields[i])+1;
            if(n+l>cap) return -1;
            memcpy(buf+n,a->fields[i],l); n+=l;
        }
    }
    buf[8]=(uint8_t)companyOff;
    return n;
}
