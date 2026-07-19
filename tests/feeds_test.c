/* feeds_test.c -- offline gate for the News feed list (bridge/feeds.c).
 * Exercises seeding, add/remove/toggle, dedup, name auto-derivation, and a
 * save -> reload round-trip. No device needed. */
#include <stdio.h>
#include <string.h>
#include "../bridge/feeds.h"

static int failures;
#define CHECK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); failures++; } }while(0)

#define PATH "pdb/_feeds.txt"

int main(void){
    printf("News feeds gate\n");
    remove(PATH);

    /* load_or_seed on a missing file seeds the built-in list */
    CHECK(feeds_load_or_seed(PATH)==1, "missing file -> seeds defaults");
    CHECK(feeds_count()==10, "10 default feeds");
    CHECK(feeds_enabled_count()>=1 && feeds_enabled_count()<=10, "some feeds enabled by default");
    const Feed *f0 = feeds_get(0);
    CHECK(f0 && !strcmp(f0->name,"BBC World") && strstr(f0->url,"bbci.co.uk"), "first default is BBC World");
    CHECK(f0 && f0->enabled, "BBC enabled by default");

    /* toggle persistence: flip one, save, reload */
    int was = feeds_get(4)->enabled;
    CHECK(feeds_toggle(4)==!was, "toggle flips enabled");
    CHECK(feeds_save(PATH)==0, "save");
    CHECK(feeds_load(PATH)==0, "reload");
    CHECK(feeds_count()==10, "count survives round-trip");
    CHECK(feeds_get(4)->enabled==!was, "toggle survived the round-trip");

    /* add a feed with no name -> name derived from host; dedup on url */
    CHECK(feeds_add("https://www.example.com/feed.xml", "")==1, "add with empty name");
    CHECK(feeds_count()==11, "count == 11");
    CHECK(!strcmp(feeds_get(10)->name,"example.com"), "name auto-derived from host (www stripped)");
    CHECK(feeds_get(10)->enabled, "added feed enabled by default");
    CHECK(feeds_add("https://www.example.com/feed.xml","dup")==0, "duplicate url rejected");
    CHECK(feeds_count()==11, "count unchanged after dup");

    /* edit + remove */
    CHECK(feeds_set(10,"https://news.site.org/rss","My Site")==1, "set fields");
    CHECK(!strcmp(feeds_get(10)->name,"My Site"), "name updated");
    CHECK(feeds_remove(10)==1, "remove");
    CHECK(feeds_count()==10, "count back to 10");
    CHECK(feeds_remove(99)==0, "out-of-range remove rejected");

    /* host label helper */
    char lbl[FEED_NAME_CAP];
    feeds_host_label("https://feeds.bbci.co.uk/news/world/rss.xml", lbl, sizeof lbl);
    CHECK(!strcmp(lbl,"feeds.bbci.co.uk"), "host label strips scheme");
    feeds_host_label("", lbl, sizeof lbl);
    CHECK(!strcmp(lbl,"News"), "empty url -> 'News' fallback");

    /* fill to capacity: adding past FEEDS_MAX fails cleanly */
    feeds_clear();
    int added=0;
    char url[64];
    for(int i=0;i<FEEDS_MAX+5;i++){ snprintf(url,sizeof url,"https://f%d.example/rss",i); added += feeds_add(url,""); }
    CHECK(feeds_count()==FEEDS_MAX, "capped at FEEDS_MAX");
    CHECK(added==FEEDS_MAX, "adds past capacity rejected");

    remove(PATH);
    printf(failures ? "\nFeeds gate: %d FAIL\n" : "\nFeeds gate: OK\n", failures);
    return failures ? 1 : 0;
}
