/* appinfo.c -- Palm AppInfo category table parse/build. */
#include <string.h>
#include "appinfo.h"
#include "palm.h"   /* put16 */

int appinfo_parse(const uint8_t *ai, int len, CatTable *t){
    memset(t,0,sizeof *t);
    if(len < 2 + CAT_COUNT*16) return -1;
    const uint8_t *labels = ai + 2;             /* skip renamedCategories */
    for(int i=0;i<CAT_COUNT;i++){
        char *dst = t->name[i];
        memcpy(dst, labels + i*16, 15); dst[15]=0;
        dst[strnlen(dst,15)] = 0;               /* ensure terminated */
    }
    return 0;
}

int appinfo_build(uint8_t *out, int cap, const CatTable *t){
    if(cap < APPINFO_SIZE) return -1;
    memset(out,0,APPINFO_SIZE);
    put16(out,0);                               /* renamedCategories = 0 */
    uint8_t *labels = out + 2;
    for(int i=0;i<CAT_COUNT;i++){
        strncpy((char*)labels + i*16, t->name[i], 15);
        out[2 + CAT_COUNT*16 + i] = (uint8_t)i;  /* categoryUniqIDs[i] = i */
    }
    out[2 + CAT_COUNT*16 + CAT_COUNT] = CAT_COUNT-1;  /* lastUniqID */
    return APPINFO_SIZE;
}
