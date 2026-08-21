/* rss_test.c -- offline gate for the RSS/Atom parser + HTML-to-text (bridge/rss.c).
 *
 * The live HTTPS fetch is device-only, but the parsing + HTML stripping (the part
 * that turns arbitrary feed bytes into clean reader text) is portable and proven
 * here: RSS 2.0 and Atom, CDATA vs entity-escaped HTML, entity decoding, the body
 * preference order, and the item cap. No network needed.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../bridge/rss.h"

static int failures;
#define CHECK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); failures++; } }while(0)
#define HAS(s,sub)  (strstr((s),(sub))!=NULL)

#define MAXI 16
static struct { char title[256], text[4096]; uint32_t when; } g_it[MAXI];
static int g_n;
static void collect(const char *t, const char *x, uint32_t when, void *ctx){ (void)ctx;
    if(g_n<MAXI){ snprintf(g_it[g_n].title,sizeof g_it[0].title,"%s",t);
                  snprintf(g_it[g_n].text,sizeof g_it[0].text,"%s",x);
                  g_it[g_n].when = when; g_n++; } }

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
    CHECK(HAS(t,"Caf\xC3\xA9"), "numeric entity in the font's range stays UTF-8");
    CHECK(HAS(t,"--") && !HAS(t,"\xE2\x80\x94"), "em dash folds to ASCII");

    /* --- folding what lv_font_palm cannot draw (it covers U+0020..U+00FF) ---
     * Every one of these arrived as a hollow box on the device. They come far
     * more often as raw UTF-8 bytes than as entities, so both paths are pinned. */
    rss_html_to_text(t,sizeof t,"\xE2\x80\x9CQuoted\xE2\x80\x9D and \xE2\x80\x98single\xE2\x80\x99");
    CHECK(!strcmp(t,"\"Quoted\" and 'single'"), "raw curly quotes fold to ASCII quotes");
    rss_html_to_text(t,sizeof t,"wait\xE2\x80\xA6 more");
    CHECK(!strcmp(t,"wait... more"), "raw ellipsis folds to three dots");
    rss_html_to_text(t,sizeof t,"a \xE2\x80\x93 b \xE2\x80\x94 c");
    CHECK(!strcmp(t,"a - b -- c"), "en dash is one hyphen, em dash is two");
    rss_html_to_text(t,sizeof t,"&rsquo;&ldquo;&hellip;&mdash;");
    CHECK(!strcmp(t,"'\"...--"), "the same characters fold when they arrive as entities");
    rss_html_to_text(t,sizeof t,"soft\xC2\xADhyphen\xE2\x80\x8Bzero");
    CHECK(!strcmp(t,"softhyphenzero"), "invisible characters are dropped, not boxed");
    rss_html_to_text(t,sizeof t,"nbsp\xC2\xA0gap");
    CHECK(!strcmp(t,"nbsp gap"), "nbsp becomes a space that can wrap");
    rss_html_to_text(t,sizeof t,"caf\xC3\xA9 na\xC3\xAFve \xC2\xA3" "5");
    CHECK(!strcmp(t,"caf\xC3\xA9 na\xC3\xAFve \xC2\xA3" "5"), "Latin-1 the font HAS is left alone");
    rss_html_to_text(t,sizeof t,"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E ok");
    CHECK(!strcmp(t,"? ok"), "an unrenderable run collapses to one mark, not three");
    rss_html_to_text(t,sizeof t,"pre\xE2\x82\xACpost");
    CHECK(!strcmp(t,"preEURpost"), "euro sign spells out");

    /* --- publication dates ------------------------------------------------
     * The fetch keeps today's stories and drops last week's, so a misparsed date
     * silently empties the News app. Every shape our ten shipped feeds actually
     * use is pinned here. 2026-08-20T14:32:00Z is 1787236320. */
    {
        static const char *DATED =
        "<rss version=\"2.0\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><channel>\n"
        "<item><title>Rfc822</title><description>d</description>"
            "<pubDate>Wed, 20 Aug 2026 14:32:00 GMT</pubDate></item>\n"
        "<item><title>Rfc822Off</title><description>d</description>"
            "<pubDate>Wed, 20 Aug 2026 16:32:00 +0200</pubDate></item>\n"
        "<item><title>Iso</title><description>d</description>"
            "<dc:date>2026-08-20T14:32:00Z</dc:date></item>\n"
        "<item><title>IsoOff</title><description>d</description>"
            "<dc:date>2026-08-20T10:32:00-04:00</dc:date></item>\n"
        "<item><title>Undated</title><description>d</description></item>\n"
        "<item><title>Junk</title><description>d</description>"
            "<pubDate>not a date at all</pubDate></item>\n"
        "</channel></rss>\n";
        const uint32_t T = 1787236320u;   /* verified: 2026-08-20T14:32:00Z */
        g_n=0; rss_parse_buf(DATED,(int)strlen(DATED),0,collect,NULL);
        CHECK(g_n==6, "dated feed: 6 items");
        CHECK(g_it[0].when==T, "RFC 822 with GMT");
        CHECK(g_it[1].when==T, "RFC 822 with a +0200 offset resolves to the same instant");
        CHECK(g_it[2].when==T, "ISO 8601 with Z (dc:date, as RDF feeds use)");
        CHECK(g_it[3].when==T, "ISO 8601 with a -04:00 offset");
        CHECK(g_it[4].when==0, "an item with no date reports 0, never a guess");
        CHECK(g_it[5].when==0, "an unparsable date reports 0 too");
    }

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
