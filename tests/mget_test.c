/* mget_test.c -- the multiget reply parser (dav_parse_multiget_stream).
 *
 * A batched fetch's reply carries many objects in one multistatus, each one's
 * calendar or address data inside XML: entity-escaped (Radicale) or in a CDATA
 * section (iCloud), with whatever namespace prefixes the server likes. The
 * parser streams it a character at a time into a body file, so an object's
 * size costs no RAM. This checks that every object comes out byte for byte,
 * that a member with no data (a 404) is skipped, and that hostile input --
 * cut anywhere, unterminated, enormous -- can't make it misbehave. Offline;
 * in `make test`, and under ASan/UBSan in `make ftest`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/dav_xml.h"

static int fails = 0;
static void CK(int c, const char *m){ if(!c){ fails++; printf("  FAIL: %s\n", m); } }

#define IN   "state/.mg_in"
#define BODY "state/.mg_body"

typedef struct { char name[8][64], etag[8][64]; long off[8], len[8]; int n; } Got;
static void cb(const char *name, const char *etag, long off, long len, void *ctx){
    Got *g = ctx; if(g->n >= 8) return;
    snprintf(g->name[g->n], 64, "%s", name); snprintf(g->etag[g->n], 64, "%s", etag);
    g->off[g->n] = off; g->len[g->n] = len; g->n++;
}
static int run(const char *xml, size_t xl, Got *g){
    FILE *f = fopen(IN, "wb"); fwrite(xml, 1, xl, f); fclose(f);
    FILE *in = fopen(IN, "rb"), *b = fopen(BODY, "w+b");
    memset(g, 0, sizeof *g);
    int n = dav_parse_multiget_stream(in, b, cb, g);
    fclose(in); fclose(b);
    return n;
}
/* the body of member i, as the parser wrote it */
static char *bodyOf(const Got *g, int i){
    static char buf[70000];
    FILE *b = fopen(BODY, "rb"); fseek(b, g->off[i], SEEK_SET);
    size_t n = fread(buf, 1, (size_t)g->len[i], b); buf[n] = 0; fclose(b);
    return buf;
}

int main(void){
    Got g;
    printf("== entity-escaped data, two members and a 404 ==\n");
    const char *x1 =
        "<?xml version=\"1.0\"?>\n<multistatus xmlns=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
        "<response><href>/palm/cal/a.ics</href><propstat><prop><getetag>\"e1\"</getetag>"
        "<C:calendar-data>BEGIN:VCALENDAR&#13;\nSUMMARY:Tom &amp; Jerry &lt;3&gt;\nEND:VCALENDAR</C:calendar-data>"
        "</prop><status>HTTP/1.1 200 OK</status></propstat></response>"
        "<response><href>/palm/cal/gone.ics</href><status>HTTP/1.1 404 Not Found</status></response>"
        "<response><href>/palm/cal/b%20c.ics</href><propstat><prop><getetag>W/\"e2\"</getetag>"
        "<C:calendar-data>X:caf&#233; &#x2603;</C:calendar-data></prop></propstat></response>"
        "</multistatus>";
    CK(run(x1, strlen(x1), &g) == 2, "two members with data (the 404 is skipped)");
    CK(!strcmp(g.name[0], "a.ics") && !strcmp(g.etag[0], "e1"), "first: name and etag");
    CK(!strcmp(bodyOf(&g, 0), "BEGIN:VCALENDAR\r\nSUMMARY:Tom & Jerry <3>\nEND:VCALENDAR"), "first: entities decoded exactly");
    CK(!strcmp(g.name[1], "b%20c.ics") && !strcmp(g.etag[1], "e2"), "second: name as written, weak etag stripped");
    CK(!strcmp(bodyOf(&g, 1), "X:caf\xC3\xA9 \xE2\x98\x83"), "second: numeric entities become UTF-8");

    printf("== CDATA, other prefixes, and a ']' inside ==\n");
    const char *x2 =
        "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>\n  /1/calendars/home/x.ics\n</d:href>"
        "<d:propstat><d:prop><d:getetag>\"9\"</d:getetag>"
        "<cal:calendar-data xmlns:cal=\"urn:ietf:params:xml:ns:caldav\"><![CDATA[BEGIN:V\r\nNOTE:a]b]]c <tag> &amp;\r\nEND:V\r\n]]></cal:calendar-data>"
        "</d:prop></d:propstat></d:response></d:multistatus>";
    CK(run(x2, strlen(x2), &g) == 1, "one member");
    CK(!strcmp(g.name[0], "x.ics"), "href trimmed to its name");
    CK(!strcmp(bodyOf(&g, 0), "BEGIN:V\r\nNOTE:a]b]]c <tag> &amp;\r\nEND:V\r\n"), "CDATA copied raw, ']' and ']]' kept");

    printf("== an object far bigger than any buffer ==\n");
    { size_t big = 60000; char *x = malloc(big + 1000), *want = malloc(big + 1);
      int n = sprintf(x, "<multistatus xmlns=\"DAV:\"><response><href>/c/big.vcf</href><getetag>e</getetag><address-data>");
      for(size_t i = 0; i < big; i++){ want[i] = (char)('A' + i % 26); x[n++] = want[i]; }
      want[big] = 0;
      n += sprintf(x + n, "</address-data></response></multistatus>");
      CK(run(x, (size_t)n, &g) == 1 && g.len[0] == (long)big, "all 60000 bytes");
      CK(!strcmp(bodyOf(&g, 0), want), "byte for byte");
      free(x); free(want); }

    printf("== not a multistatus ==\n");
    const char *x4 = "<html><body>502 Bad Gateway</body></html>";
    CK(run(x4, strlen(x4), &g) == -1, "refused");

    printf("== cut at every byte: never crashes, never invents a member ==\n");
    { size_t l = strlen(x1); int bad = 0;
      for(size_t cut = 0; cut < l; cut++){
          int n = run(x1, cut, &g);
          if(n > 2) bad++;
          for(int i = 0; i < g.n; i++) if(g.len[i] < 0) bad++;
      }
      CK(!bad, "every truncation handled"); }

    printf("== hostile: unterminated CDATA, comment and tag, endless entity ==\n");
    const char *h[] = {
        "<multistatus><response><href>/a</href><calendar-data><![CDATA[never ends",
        "<multistatus><!-- never ends <response>",
        "<multistatus><response><href>/a</href><calendar-data>&aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "<multistatus><response><href>/a</href><calendar-data>x</calendar-data></response",
        "<<<<<<<>>>>>>&&&&;;;;<![CDATA[]]>]]>",
    };
    for(int i = 0; i < 5; i++){ run(h[i], strlen(h[i]), &g); CK(g.n <= 1, "hostile input parsed without harm"); }

    remove(IN); remove(BODY);
    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
