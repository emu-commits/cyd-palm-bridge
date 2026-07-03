/* tz.h -- minimal timezone support for the bridge.
 *
 * Palm stores local wall-clock with no zone. The bridge is configured with a
 * single device TZID; on emit we annotate wall-clock with ;TZID= and ship a
 * matching VTIMEZONE (so the wall-clock is preserved literally and round-trips
 * exactly). On parse, UTC (`...Z`) inputs are converted to device-local.
 *
 * DST is computed from standard nth-weekday rules (US and EU); away from the
 * 1-hour transition window this is exact. Unknown zones -> fixed offset.
 */
#ifndef TZ_H
#define TZ_H

enum { DST_NONE, DST_US, DST_EU };

typedef struct {
    const char *id;
    int  stdOff;      /* minutes east of UTC, standard time (US = negative)  */
    int  dstOff;      /* minutes east of UTC, daylight time                  */
    int  rule;        /* DST_NONE / DST_US / DST_EU                          */
    const char *stdName, *dstName;
} Tz;

const Tz *tz_find(const char *id);                 /* NULL if unknown/UTC     */
int  tz_offset_utc(const Tz*,int y,int mo,int d,int h,int mi); /* off at UTC instant */
void tz_utc_to_local(const Tz*,int y,int mo,int d,int h,int mi,
                     int*oy,int*omo,int*od,int*oh,int*omi);
int  tz_vtimezone(const Tz*,char*out,int cap);     /* VTIMEZONE block, or 0   */

#endif
