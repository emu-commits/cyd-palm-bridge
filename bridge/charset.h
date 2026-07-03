/* charset.h -- Palm text (Windows-1252 / Latin-1 superset) <-> UTF-8.
 * DAV wants UTF-8; Palm PIM stores single-byte CP1252. Round-trips exactly
 * for any byte a Palm field can hold; unmappable UTF-8 -> '?'.
 */
#ifndef CHARSET_H
#define CHARSET_H
int cp1252_to_utf8(char *dst,int cap,const char *src);
int utf8_to_cp1252(char *dst,int cap,const char *src);
#endif
