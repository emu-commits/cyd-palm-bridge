/* rss.h -- streaming RSS 2.0 / Atom feed parser + HTML-to-text.
 *
 * On the device the feed body is spooled to SD during HotSync and parsed here
 * one item at a time (bounded RAM, like the DAV sliding-window enumeration), then
 * each item's title + plain text is written into the news store the reader app
 * browses offline. All logic is portable and host-gate-tested (tests/rss_test.c);
 * only the network GET that fills the spool file is device-specific.
 */
#ifndef RSS_H
#define RSS_H

/* one parsed item: UTF-8 title + plain-text body (HTML tags stripped, entities
 * decoded, whitespace collapsed). Both are NUL-terminated, caller-owned for the
 * duration of the callback only. */
typedef void (*rss_item_cb)(const char *title, const char *text, void *ctx);

/* Parse an RSS 2.0 or Atom feed. Recognises <item>/<entry>, picks <title> and the
 * richest body among content:encoded / description / content / summary, and emits
 * each via cb. max_items caps emissions (0 = no cap). Bounded RAM in feed size.
 * Returns items emitted, or -1 on open error. */
int rss_parse_file(const char *path, int max_items, rss_item_cb cb, void *ctx);
int rss_parse_buf (const char *xml, int len, int max_items, rss_item_cb cb, void *ctx);

/* Strip HTML tags and decode entities from `html` into plain UTF-8 text in
 * dst[cap] (collapsing whitespace runs to single spaces). Returns dst length. */
int rss_html_to_text(char *dst, int cap, const char *html);

#endif
