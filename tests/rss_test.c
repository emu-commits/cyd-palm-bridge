/* rss_test.c -- offline gate for the RSS/Atom parser + HTML-to-text (bridge/rss.c).
 *
 * The live HTTPS fetch is device-only, but the parsing + HTML stripping (the part
 * that turns arbitrary feed bytes into clean reader text) is portable and proven
 * here: RSS 2.0 and Atom, CDATA vs entity-escaped HTML, entity decoding, the body
 * preference order, and the item cap. No network needed.
 */
#include <stdio.h>
#include <string.h>
#include "../bridge/rss.h"

static int failures;
#define CHECK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); failures++; } }while(0)
#define HAS(s,sub)  (strstr((s),(sub))!=NULL)

#define MAXI 16
static struct { char title[256], text[4096]; } g_it[MAXI];
static int g_n;
static void collect(const char *t, const char *x, void *ctx){ (void)ctx;
    if(g_n<MAXI){ snprintf(g_it[g_n].title,sizeof g_it[0].title,"%s",t);
                  snprintf(g_it[g_n].text,sizeof g_it[0].text,"%s",x); g_n++; } }

static const char *RSS =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<rss version=\"2.0\" xmlns:content=\"http://purl.org/rss/1.0/modules/content/\">\n"
"<channel><title>Channel</title>\n"
"<item>\n"
"  <title>First &amp; Best</title>\n"
"  <description>&lt;p&gt;Hello  &lt;b&gt;world&lt;/b&gt;. Caf&#233;.&lt;/p&gt;</description>\n"
"</item>\n"
"<item>\n"
"  <title><![CDATA[Two <3]]></title>\n"
"  <description>short desc</description>\n"
"  <content:encoded><![CDATA[<p>Full &amp; rich body.</p>]]></content:encoded>\n"
"</item>\n"
"</channel></rss>\n";

static const char *ATOM =
"<?xml version=\"1.0\"?>\n"
"<feed xmlns=\"http://www.w3.org/2005/Atom\">\n"
"<title>AtomChannel</title>\n"
"<entry><title>Atom One</title><summary>Just a &amp; summary</summary></entry>\n"
"<entry><title>Atom Two</title>\n"
"  <content type=\"html\">&lt;p&gt;Body two&lt;/p&gt;</content>\n"
"  <summary>ignored</summary></entry>\n"
"</feed>\n";

int main(void){
    printf("RSS/Atom parser gate\n");

    /* --- HTML-to-text units --- */
    char t[256];
    rss_html_to_text(t,sizeof t,"&lt;b&gt;Hi&lt;/b&gt; there");
    CHECK(!strcmp(t,"Hi there"), "escaped tags stripped");
    rss_html_to_text(t,sizeof t,"<p>Raw</p> &amp; more");
    CHECK(HAS(t,"Raw") && HAS(t,"& more") && !HAS(t,"<p>"), "raw tags stripped, &amp; decoded");
    rss_html_to_text(t,sizeof t,"Caf&#233; &#x2014; ok");
    CHECK(HAS(t,"Caf\xC3\xA9") && HAS(t,"\xE2\x80\x94"), "numeric entities -> UTF-8");

    /* --- RSS 2.0 --- */
    g_n=0;
    int n = rss_parse_buf(RSS,(int)strlen(RSS),0,collect,NULL);
    CHECK(n==2, "RSS: 2 items");
    CHECK(!strcmp(g_it[0].title,"First & Best"), "RSS item0 title decoded");
    CHECK(HAS(g_it[0].text,"Hello") && HAS(g_it[0].text,"world") && HAS(g_it[0].text,"Caf\xC3\xA9"),
          "RSS item0 escaped-HTML description -> clean text");
    CHECK(!HAS(g_it[0].text,"<p>") && !HAS(g_it[0].text,"&lt;"), "RSS item0 no tags/entities leak");
    CHECK(HAS(g_it[1].title,"Two"), "RSS item1 CDATA title");
    CHECK(HAS(g_it[1].text,"Full & rich body") && !HAS(g_it[1].text,"short desc"),
          "RSS item1 prefers content:encoded over description");

    /* --- Atom --- */
    g_n=0;
    n = rss_parse_buf(ATOM,(int)strlen(ATOM),0,collect,NULL);
    CHECK(n==2, "Atom: 2 entries");
    CHECK(!strcmp(g_it[0].title,"Atom One") && HAS(g_it[0].text,"Just a & summary"),
          "Atom entry0 title + summary");
    CHECK(HAS(g_it[1].text,"Body two") && !HAS(g_it[1].text,"ignored"),
          "Atom entry1 prefers content over summary");

    /* --- item cap --- */
    g_n=0;
    n = rss_parse_buf(RSS,(int)strlen(RSS),1,collect,NULL);
    CHECK(n==1, "max_items caps emissions");

    /* --- file path variant --- */
    FILE *f=fopen("pdb/_rss.xml","wb");
    if(f){ fwrite(RSS,1,strlen(RSS),f); fclose(f);
        g_n=0; n=rss_parse_file("pdb/_rss.xml",0,collect,NULL);
        CHECK(n==2, "rss_parse_file matches buffer parse");
        remove("pdb/_rss.xml");
    }

    printf(failures ? "\nRSS gate: %d FAIL\n" : "\nRSS gate: OK\n", failures);
    return failures ? 1 : 0;
}
