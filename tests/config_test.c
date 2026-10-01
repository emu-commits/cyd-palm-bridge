/* config_test.c -- round-trip + robust-parse the runtime config. No server. */
#include <stdio.h>
#include <string.h>
#include "../bridge/config.h"

static int fails=0;
static void CK(int c,const char*m){ if(!c){ fails++; printf("  FAIL: %s\n",m);} else printf("  ok: %s\n",m); }

int main(void){
    const char *PATH="state/cfg_test.ini";
    printf("== config ==\n");

    /* defaults */
    Config c; config_defaults(&c);
    CK(c.brightness==80,"default brightness 80");
    CK(c.clock24==0,"default clock 12-hour");
    CK(!strcmp(c.world1,"Europe/London"),"default world clock 1");
    CK(!strcmp(c.world2,"Asia/Tokyo"),"default world clock 2");
    CK(c.policy==CFG_POL_SERVER,"default policy server");
    CK(c.cal_back==1 && c.cal_ahead==14,"default calendar window: yesterday to two weeks ahead");
    CK(strstr(c.dav_base,"caldav.icloud.com")!=NULL,"default caldav host");
    CK(strstr(c.dav_card_base,"contacts.icloud.com")!=NULL,"default contacts host");

    /* fill + save + reload -> round-trip */
    snprintf(c.wifi[0].ssid,sizeof c.wifi[0].ssid,"HomeNet");
    ConfigSecrets cs; memset(&cs,0,sizeof cs);
    snprintf(cs.wifi_pass[0],sizeof cs.wifi_pass[0],"s3cr3t-pw");
    snprintf(c.wifi[1].ssid,sizeof c.wifi[1].ssid,"Office 5G");
    snprintf(cs.wifi_pass[1],sizeof cs.wifi_pass[1],"office-pw");
    snprintf(c.wifi[3].ssid,sizeof c.wifi[3].ssid,"Phone Hotspot");
    snprintf(c.dav_user,sizeof c.dav_user,"me@icloud.com");
    snprintf(cs.dav_pass,sizeof cs.dav_pass,"abcd-efgh-ijkl-mnop");
    snprintf(c.cal_coll,sizeof c.cal_coll,"123/calendars/UUID-CAL");
    snprintf(c.todo_coll,sizeof c.todo_coll,"123/calendars/UUID-TODO");
    snprintf(c.card_coll,sizeof c.card_coll,"123/carddavhome/card");
    snprintf(c.timezone,sizeof c.timezone,"America/New_York");
    snprintf(c.world1,sizeof c.world1,"America/Los_Angeles");
    snprintf(c.world2,sizeof c.world2,"Australia/Sydney");
    c.brightness=55; c.backlight_sec=15; c.clock24=1; c.policy=CFG_POL_BOTH;
    CK(config_save(PATH,&c,&cs)==0,"save ok");

    Config d; config_defaults(&d);
    ConfigSecrets ds; memset(&ds,0,sizeof ds);
    CK(config_load(PATH,&d,&ds)==0,"load ok");
    CK(!strcmp(d.wifi[0].ssid,"HomeNet"),"wifi slot 1 ssid round-trips");
    CK(!strcmp(ds.wifi_pass[0],"s3cr3t-pw"),"wifi slot 1 pass round-trips");
    CK(!strcmp(d.wifi[1].ssid,"Office 5G"),"wifi slot 2 ssid round-trips (spaces kept)");
    CK(!strcmp(ds.wifi_pass[1],"office-pw"),"wifi slot 2 pass round-trips");
    CK(d.wifi[2].ssid[0]==0,"an empty slot stays empty");
    CK(!strcmp(d.wifi[3].ssid,"Phone Hotspot"),"wifi slot 4 ssid round-trips");
    CK(ds.wifi_pass[3][0]==0,"a network with no password round-trips as open");
    CK(!strcmp(d.dav_user,"me@icloud.com"),"dav_user round-trips");
    CK(!strcmp(ds.dav_pass,"abcd-efgh-ijkl-mnop"),"dav_pass round-trips");

    /* Without a ConfigSecrets, the passwords are neither read nor written: the
     * file functions have nowhere else to put them. */
    Config d2; config_defaults(&d2);
    CK(config_load(PATH,&d2,NULL)==0,"load with no secrets ok");
    CK(!strcmp(d2.wifi[0].ssid,"HomeNet"),"...and everything else still loads");
    CK(config_save(PATH,&d2,NULL)==0,"save with no secrets ok");
    ConfigSecrets es; memset(&es,0,sizeof es);
    Config d3; config_defaults(&d3);
    CK(config_load(PATH,&d3,&es)==0,"reload it");
    CK(es.wifi_pass[0][0]==0 && es.wifi_pass[1][0]==0 && es.dav_pass[0]==0,
       "a save with no secrets writes every password key empty");
    CK(!strcmp(d3.wifi[1].ssid,"Office 5G"),"...and keeps the networks");
    char wb[8]="abcdefg"; config_wipe(wb,sizeof wb);
    CK(wb[0]==0 && wb[6]==0,"config_wipe zeroes the buffer");
    CK(!strcmp(d.cal_coll,"123/calendars/UUID-CAL"),"cal_coll round-trips");
    CK(!strcmp(d.todo_coll,"123/calendars/UUID-TODO"),"todo_coll round-trips");
    CK(!strcmp(d.card_coll,"123/carddavhome/card"),"card_coll round-trips");
    CK(!strcmp(d.timezone,"America/New_York"),"timezone round-trips");
    CK(d.brightness==55,"brightness round-trips");
    CK(d.backlight_sec==15,"backlight_sec round-trips");
    CK(d.policy==CFG_POL_BOTH,"policy round-trips");

    /* robust parse: comments, whitespace, unknown keys, malformed lines, clamps */
    FILE *f=fopen(PATH,"w");
    fprintf(f,
        "# a hand-edited config\n"
        "\n"
        "  wifi_ssid   =   Spacey Net  \n"     /* surrounding + internal spaces */
        "WIFI_PASS = CaseKey\n"                /* key case-insensitive          */
        "unknown_key = ignore me\n"            /* unknown -> skipped            */
        "no equals sign here\n"                /* malformed -> skipped          */
        "brightness = 999\n"                   /* clamp to 100                  */
        "backlight_sec = -5\n"                 /* clamp to 0                    */
        "policy = local\n"
        "= emptykey\n"                         /* empty key -> skipped          */
        "timezone=Europe/London\n");
    fclose(f);
    Config e; config_defaults(&e);
    ConfigSecrets esec; memset(&esec,0,sizeof esec);
    CK(config_load(PATH,&e,&esec)==0,"robust load ok");
    CK(!strcmp(e.wifi[0].ssid,"Spacey Net"),"trims outer space, keeps inner");
    CK(!strcmp(esec.wifi_pass[0],"CaseKey"),"key match is case-insensitive");
    CK(e.brightness==100,"brightness clamped to 100");
    CK(e.backlight_sec==0,"backlight_sec clamped to 0");
    CK(e.policy==CFG_POL_LOCAL,"policy=local parsed");
    CK(!strcmp(e.timezone,"Europe/London"),"no-space key=value parsed");

    /* INLINE COMMENTS -- the shape config.ini.example actually ships. Before the
     * parser cut these, `timezone` kept the whole trailing comment and silently
     * resolved to UTC, and `dav_pass` silently grew one. */
    f=fopen(PATH,"w");
    fprintf(f,
        "timezone = America/New_York          # empty = floating local time\n"
        "dav_pass = abcd-efgh-ijkl-mnop       # iCloud APP-SPECIFIC password\n"
        "brightness = 80                      # backlight 0..100\n"
        "policy = both                        # server | local | both\n"
        "wifi_pass = P#ssw0rd\n"               /* bare '#' is NOT a comment      */
        "wifi_ssid = Net#5   # trailing note\n"/* bare '#' kept, ' #' cut        */
        "cal_coll =                           # deliberately empty\n");
    fclose(f);
    Config h; config_defaults(&h);
    ConfigSecrets hs; memset(&hs,0,sizeof hs);
    CK(config_load(PATH,&h,&hs)==0,"inline-comment load ok");
    CK(!strcmp(h.timezone,"America/New_York"),"inline comment cut off timezone");
    CK(!strcmp(hs.dav_pass,"abcd-efgh-ijkl-mnop"),"inline comment cut off dav_pass");
    CK(h.brightness==80,"inline comment cut off brightness");
    CK(h.policy==CFG_POL_BOTH,"inline comment cut off policy");
    CK(!strcmp(hs.wifi_pass[0],"P#ssw0rd"),"'#' with no space before it is literal");
    CK(!strcmp(h.wifi[0].ssid,"Net#5"),"literal '#' kept while ' #' comment is cut");
    CK(h.cal_coll[0]==0,"comment-only value is empty, not the comment");

    /* missing file -> -1, defaults preserved */
    Config g; config_defaults(&g);
    CK(config_load("state/does_not_exist.ini",&g,NULL)==-1,"missing file -> -1");
    CK(g.brightness==80,"defaults intact after failed load");

    /* The location's provenance flag, and the DIRECTION OF ITS DEFAULT, which is
     * the part that protects people: a config.ini written before the flag
     * existed has hand-entered coordinates and no `loc_auto` key, and the safe
     * reading of that silence is "a human put these here" -- so silence must
     * mean PINNED, never "help yourself". */
    Config lz; config_defaults(&lz);
    CK(lz.loc_auto == -1, "a fresh config has not been told either way");
    f=fopen(PATH,"w");
    fprintf(f, "latitude = 51.5074\nlongitude = -0.1278\n");   /* a pre-flag card */
    fclose(f);
    Config lo; config_defaults(&lo);
    CK(config_load(PATH,&lo,NULL)==0,"a pre-flag card loads");
    CK(!strcmp(lo.latitude,"51.5074"),"...with its coordinates");
    CK(lo.loc_auto == -1, "...and the question stays open for appcfg to settle");

    Config la; config_defaults(&la);
    la.loc_auto = 1;
    snprintf(la.loc_name,sizeof la.loc_name,"Washington, D.C.");
    snprintf(la.latitude,sizeof la.latitude,"38.9072");
    snprintf(la.longitude,sizeof la.longitude,"-77.0369");
    CK(config_save(PATH,&la,NULL)==0,"save an automatic location");
    Config lb; config_defaults(&lb);
    CK(config_load(PATH,&lb,NULL)==0,"reload it");
    CK(lb.loc_auto == 1, "loc_auto round-trips");

    /* -1 must never reach the file: writing it is what settles the question, and
     * a card that kept saying "I have not been told" would be asked forever. */
    Config lu; config_defaults(&lu);
    snprintf(lu.latitude,sizeof lu.latitude,"1.0");
    CK(lu.loc_auto == -1, "unsettled before save");
    CK(config_save(PATH,&lu,NULL)==0,"save an unsettled config");
    Config lv; config_defaults(&lv);
    CK(config_load(PATH,&lv,NULL)==0,"reload it");
    CK(lv.loc_auto == 0, "an unsettled flag is written as pinned, never as -1");
    CK(!strcmp(lb.loc_name,"Washington, D.C."),"a place name with a comma round-trips");

    /* FOUR networks.
     *
     * The back-compat case is the one that matters on a device that is already
     * in the field: a config.ini written when there was only one network has an
     * unnumbered wifi_ssid/wifi_pass and no other wifi keys at all. It must load
     * into the FIRST slot -- the one that is tried first -- and leave the rest
     * empty rather than shifting anything. */
    f=fopen(PATH,"w");
    fprintf(f, "wifi_ssid = OldCard\nwifi_pass = oldpw\nbrightness = 42\n");
    fclose(f);
    Config o; config_defaults(&o);
    ConfigSecrets os; memset(&os,0,sizeof os);
    CK(config_load(PATH,&o,&os)==0,"a one-network config from before the four slots loads");
    CK(!strcmp(o.wifi[0].ssid,"OldCard"),"a one-network card lands in slot 1");
    CK(!strcmp(os.wifi_pass[0],"oldpw"),"...with its password");
    CK(o.wifi[1].ssid[0]==0 && o.wifi[3].ssid[0]==0,"...and the other slots stay empty");

    /* promote: the order is the try order, so a successful join moves its slot
     * to the front and everything above it slides down by one. */
    Config p; config_defaults(&p);
    snprintf(p.wifi[0].ssid,sizeof p.wifi[0].ssid,"A");
    snprintf(p.wifi[1].ssid,sizeof p.wifi[1].ssid,"B");
    snprintf(p.wifi[2].ssid,sizeof p.wifi[2].ssid,"C");
    p.wifi[2].has_pass = 1;
    snprintf(p.wifi[3].ssid,sizeof p.wifi[3].ssid,"D");
    CK(config_wifi_promote(&p,2)==1,"promoting a later slot reports a change");
    CK(!strcmp(p.wifi[0].ssid,"C"),"the promoted network is now first");
    CK(p.wifi[0].has_pass == 1 && p.wifi[1].has_pass == 0,
       "its has-password flag travels with it");
    CK(!strcmp(p.wifi[1].ssid,"A") && !strcmp(p.wifi[2].ssid,"B"),"the rest keep their order");
    CK(!strcmp(p.wifi[3].ssid,"D"),"slots below the promoted one do not move");
    CK(config_wifi_promote(&p,0)==0,"promoting the first slot is not a change");
    CK(config_wifi_promote(&p,CFG_WIFI_N)==0,"an out-of-range slot is not a change");
    CK(!strcmp(p.wifi[0].ssid,"C"),"...and neither no-op disturbed the order");

    /* the promoted order is what gets written, so the device wakes up trying the
     * network that worked last */
    CK(config_save(PATH,&p,NULL)==0,"save after promote");
    Config q; config_defaults(&q);
    CK(config_load(PATH,&q,NULL)==0,"reload after promote");
    CK(!strcmp(q.wifi[0].ssid,"C") && !strcmp(q.wifi[2].ssid,"B"),"promoted order round-trips");

    /* the calendar window round-trips, and a hand-edited value is clamped */
    { Config w; config_defaults(&w); w.cal_back=7; w.cal_ahead=0;
      CK(config_save(PATH,&w,NULL)==0,"save the window");
      Config x; config_defaults(&x);
      CK(config_load(PATH,&x,NULL)==0 && x.cal_back==7 && x.cal_ahead==0,"the window round-trips (0 = everything)");
      FILE*f=fopen(PATH,"w"); fputs("cal_days_back = -5\ncal_days_ahead = 99999\n",f); fclose(f);
      Config y; config_defaults(&y); config_load(PATH,&y,NULL);
      CK(y.cal_back==0 && y.cal_ahead==730,"out-of-range values are clamped"); }

    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
