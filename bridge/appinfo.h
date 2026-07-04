/* appinfo.h -- Palm standard AppInfo block (category table).
 *
 * Categorized Palm apps store a 276-byte AppInfoType at the front of the .pdb
 * (pointed to by the header's appInfoID). It holds up to 16 category labels;
 * a record's category is the low nibble of its index-entry attribute byte.
 *
 *   UInt16 renamedCategories
 *   Char   categoryLabels[16][16]   (null-terminated names; [0] == "Unfiled")
 *   UInt8  categoryUniqIDs[16]
 *   UInt8  lastUniqID
 *   UInt8  padding
 */
#ifndef APPINFO_H
#define APPINFO_H
#include <stdint.h>

#define CAT_COUNT 16
#define APPINFO_SIZE 276

typedef struct { char name[CAT_COUNT][16]; } CatTable;

/* parse category labels out of an AppInfo block. 0 on success. */
int appinfo_parse(const uint8_t *ai, int len, CatTable *t);
/* build a 276-byte AppInfo block from a category table. returns length. */
int appinfo_build(uint8_t *out, int cap, const CatTable *t);

#endif
