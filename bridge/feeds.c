/* feeds.c -- on-SD list of RSS/Atom sources for the News reader. See feeds.h.
 *
 * RAM: one fixed table in BSS (FEEDS_MAX entries, ~4 KB); parsing is line-at-a-
 * time (a bounded stack buffer, no whole-file load). Robust against a hand-edited
 * file: blank/`#` lines and malformed rows are skipped, every copy length-bounded.
 */
#include "feeds.h"
#include <stdio.h>
#include <string.h>

static Feed s_feeds[FEEDS_MAX];
static int  s_n;

static void setstr(char *dst, int cap, const char *val){ snprintf(dst, cap, "%s", val); }

/* trim leading/trailing ASCII whitespace (but NOT tabs used as field separators
 * -- callers split on '\t' first, so a field here is already tab-free). */
static char *trim(char *s){
    while(*s==' '||*s=='\r'||*s=='\n') s++;
    char *e=s+strlen(s);
    while(e>s && (e[-1]==' '||e[-1]=='\r'||e[-1]=='\n'||e[-1]=='\t')) *--e=0;
    return s;
}

void feeds_host_label(const char *url, char *out, int cap){
    if(cap<=0) return;
    const char *p = strstr(url ? url : "", "://");
    p = p ? p+3 : (url ? url : "");
    if(!strncmp(p,"www.",4)) p += 4;
    int j=0; for(; p[j] && p[j]!='/' && j<cap-1; j++) out[j]=p[j];
    out[j]=0;
    if(!out[0]) snprintf(out, cap, "News");
}

int feeds_count(void){ return s_n; }
const Feed *feeds_get(int i){ return (i>=0 && i<s_n) ? &s_feeds[i] : NULL; }
int feeds_enabled_count(void){ int c=0; for(int i=0;i<s_n;i++) if(s_feeds[i].enabled) c++; return c; }
void feeds_clear(void){ s_n=0; memset(s_feeds,0,sizeof s_feeds); }

/* fill in name from the URL host when the caller left it blank. */
static void ensure_name(Feed *f){
    if(!f->name[0]) feeds_host_label(f->url, f->name, sizeof f->name);
}

int feeds_add(const char *url, const char *name){
    if(!url || !url[0] || s_n>=FEEDS_MAX) return 0;
    for(int i=0;i<s_n;i++) if(!strcmp(s_feeds[i].url, url)) return 0;   /* no dups */
    Feed *f = &s_feeds[s_n];
    memset(f,0,sizeof *f);
    setstr(f->url,  sizeof f->url,  url);
    setstr(f->name, sizeof f->name, name ? name : "");
    ensure_name(f);
    f->enabled = 1;
    s_n++;
    return 1;
}

int feeds_remove(int i){
    if(i<0 || i>=s_n) return 0;
    for(int k=i; k<s_n-1; k++) s_feeds[k]=s_feeds[k+1];
    s_n--;
    memset(&s_feeds[s_n],0,sizeof s_feeds[s_n]);
    return 1;
}

int feeds_set(int i, const char *url, const char *name){
    if(i<0 || i>=s_n || !url || !url[0]) return 0;
    Feed *f = &s_feeds[i];
    setstr(f->url,  sizeof f->url,  url);
    setstr(f->name, sizeof f->name, name ? name : "");
    ensure_name(f);
    return 1;
}

int feeds_toggle(int i){
    if(i<0 || i>=s_n) return -1;
    s_feeds[i].enabled = !s_feeds[i].enabled;
    return s_feeds[i].enabled;
}

/* ---- the 10 built-in feeds: reputable, English-language, world news, https ---- */
void feeds_seed_defaults(void){
    static const struct { const char *name, *url; int on; } D[] = {
        {"BBC World",         "https://feeds.bbci.co.uk/news/world/rss.xml",           1},
        {"NPR News",          "https://feeds.npr.org/1001/rss.xml",                    1},
        {"Guardian World",    "https://www.theguardian.com/world/rss",                 1},
        {"Al Jazeera",        "https://www.aljazeera.com/xml/rss/all.xml",             1},
        {"NYT World",         "https://rss.nytimes.com/services/xml/rss/nyt/World.xml",0},
        {"Deutsche Welle",    "https://rss.dw.com/rdf/rss-en-world",                   0},
        {"France 24",         "https://www.france24.com/en/rss",                       0},
        {"CBC World",         "https://www.cbc.ca/webfeed/rss/rss-world",              0},
        {"Sky News World",    "https://feeds.skynews.com/feeds/rss/world.xml",         0},
        {"Independent World", "https://www.independent.co.uk/news/world/rss",          0},
    };
    feeds_clear();
    for(unsigned i=0; i<sizeof D/sizeof D[0] && s_n<FEEDS_MAX; i++){
        Feed *f = &s_feeds[s_n++];
        memset(f,0,sizeof *f);
        setstr(f->url,  sizeof f->url,  D[i].url);
        setstr(f->name, sizeof f->name, D[i].name);
        f->enabled = D[i].on;
    }
}

/* ---- persistence: "<on|off>\t name \t url" per line ---- */
static int truthy(const char *s){
    return s && (s[0]=='1' || s[0]=='o' || s[0]=='O' || s[0]=='t' || s[0]=='T' || s[0]=='y' || s[0]=='Y');
}

int feeds_load(const char *path){
    FILE *f = fopen(path,"r");
    if(!f) return -1;
    feeds_clear();
    char line[512];
    while(fgets(line,sizeof line,f) && s_n<FEEDS_MAX){
        if(line[0]=='#') continue;
        /* split into up to three tab-separated fields */
        char *t1 = strchr(line,'\t');
        if(!t1) continue;                          /* need at least on<TAB>url */
        *t1=0;
        char *rest = t1+1;
        char *t2 = strchr(rest,'\t');
        char *name="", *url;
        if(t2){ *t2=0; name=rest; url=t2+1; }
        else  { url=rest; }                        /* two fields: on<TAB>url */
        char *en = trim(line);
        name = trim(name);
        url  = trim(url);
        if(!url[0]) continue;
        Feed *fe = &s_feeds[s_n];
        memset(fe,0,sizeof *fe);
        setstr(fe->url,  sizeof fe->url,  url);
        setstr(fe->name, sizeof fe->name, name);
        ensure_name(fe);
        fe->enabled = truthy(en);
        s_n++;
    }
    fclose(f);
    return 0;
}

int feeds_save(const char *path){
    FILE *f = fopen(path,"w");
    if(!f) return -1;
    fprintf(f, "# CYD News feeds. One per line:  <on|off> <TAB> name <TAB> url\n");
    for(int i=0;i<s_n;i++)
        fprintf(f, "%s\t%s\t%s\n", s_feeds[i].enabled ? "on" : "off",
                s_feeds[i].name, s_feeds[i].url);
    fclose(f);
    return 0;
}

int feeds_load_or_seed(const char *path){
    if(feeds_load(path)==0 && s_n>0) return 0;     /* had an existing list */
    feeds_seed_defaults();
    feeds_save(path);
    return 1;
}
