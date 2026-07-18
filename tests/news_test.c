/* news_test.c -- offline gate for the RSS reader's SD store (bridge/news.c).
 * Writes a few articles, reopens the store, and reads back metadata + bodies by
 * index -- the exact path the reader app uses. No device needed. */
#include <stdio.h>
#include <string.h>
#include "../bridge/news.h"

static int failures;
#define CHECK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); failures++; } }while(0)

int main(void){
    printf("News store gate\n");
    news_set_paths("pdb/_news.idx", "pdb/_news.dat");

    CHECK(news_count()==0 || news_count()>=0, "count on missing store is safe");

    /* write three articles */
    CHECK(news_begin()==1, "begin");
    CHECK(news_add("Tech","First post","Body of the first article.",1000)==1, "add 0");
    CHECK(news_add("World","Second","A somewhat longer second body here.",2000)==1, "add 1");
    CHECK(news_add("Tech","Third \xC3\xA9","short",3000)==1, "add 2 (utf8 title)");
    CHECK(news_commit()==1, "commit");

    /* reopen (fresh reads, no in-RAM carryover) */
    CHECK(news_count()==3, "count == 3 after reopen");

    NewsMeta m;
    CHECK(news_meta(0,&m) && !strcmp(m.feed,"Tech") && !strcmp(m.title,"First post") && m.when==1000,
          "meta 0");
    CHECK(news_meta(1,&m) && !strcmp(m.feed,"World") && !strcmp(m.title,"Second"), "meta 1");
    CHECK(news_meta(2,&m) && !strcmp(m.title,"Third \xC3\xA9"), "meta 2 utf8 title");
    CHECK(!news_meta(3,&m) && !news_meta(-1,&m), "out-of-range meta rejected");

    char buf[256];
    int n = news_read_text(0,buf,sizeof buf);
    CHECK(n==(int)strlen("Body of the first article.") && !strcmp(buf,"Body of the first article."),
          "body 0 exact");
    news_read_text(1,buf,sizeof buf);
    CHECK(!strcmp(buf,"A somewhat longer second body here."), "body 1 exact (offset seek)");
    news_read_text(2,buf,sizeof buf);
    CHECK(!strcmp(buf,"short"), "body 2 exact");

    /* truncation is safe */
    char small[4];
    n = news_read_text(0,small,sizeof small);
    CHECK(n==3 && small[3]==0, "small buffer truncates + NUL-terminates");

    /* rewriting the store replaces it */
    CHECK(news_begin() && news_add("Only","one","just one",5)==1 && news_commit(), "rewrite");
    CHECK(news_count()==1, "count == 1 after rewrite");

    remove("pdb/_news.idx"); remove("pdb/_news.dat");
    printf(failures ? "\nNews gate: %d FAIL\n" : "\nNews gate: OK\n", failures);
    return failures ? 1 : 0;
}
