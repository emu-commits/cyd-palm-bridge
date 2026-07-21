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
    CK(strstr(c.dav_base,"caldav.icloud.com")!=NULL,"default caldav host");
    CK(strstr(c.dav_card_base,"contacts.icloud.com")!=NULL,"default contacts host");

    /* fill + save + reload -> round-trip */
    snprintf(c.wifi_ssid,sizeof c.wifi_ssid,"HomeNet");
    snprintf(c.wifi_pass,sizeof c.wifi_pass,"s3cr3t-pw");
    snprintf(c.dav_user,sizeof c.dav_user,"me@icloud.com");
    snprintf(c.dav_pass,sizeof c.dav_pass,"abcd-efgh-ijkl-mnop");
    snprintf(c.cal_coll,sizeof c.cal_coll,"123/calendars/UUID-CAL");
    snprintf(c.todo_coll,sizeof c.todo_coll,"123/calendars/UUID-TODO");
    snprintf(c.card_coll,sizeof c.card_coll,"123/carddavhome/card");
    snprintf(c.timezone,sizeof c.timezone,"America/New_York");
    snprintf(c.world1,sizeof c.world1,"America/Los_Angeles");
    snprintf(c.world2,sizeof c.world2,"Australia/Sydney");
    c.brightness=55; c.backlight_sec=15; c.clock24=1; c.policy=CFG_POL_BOTH;
    CK(config_save(PATH,&c)==0,"save ok");

    Config d; config_defaults(&d);
    CK(config_load(PATH,&d)==0,"load ok");
    CK(!strcmp(d.wifi_ssid,"HomeNet"),"wifi_ssid round-trips");
    CK(!strcmp(d.wifi_pass,"s3cr3t-pw"),"wifi_pass round-trips");
    CK(!strcmp(d.dav_user,"me@icloud.com"),"dav_user round-trips");
    CK(!strcmp(d.dav_pass,"abcd-efgh-ijkl-mnop"),"dav_pass round-trips");
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
    CK(config_load(PATH,&e)==0,"robust load ok");
    CK(!strcmp(e.wifi_ssid,"Spacey Net"),"trims outer space, keeps inner");
    CK(!strcmp(e.wifi_pass,"CaseKey"),"key match is case-insensitive");
    CK(e.brightness==100,"brightness clamped to 100");
    CK(e.backlight_sec==0,"backlight_sec clamped to 0");
    CK(e.policy==CFG_POL_LOCAL,"policy=local parsed");
    CK(!strcmp(e.timezone,"Europe/London"),"no-space key=value parsed");

    /* missing file -> -1, defaults preserved */
    Config g; config_defaults(&g);
    CK(config_load("state/does_not_exist.ini",&g)==-1,"missing file -> -1");
    CK(g.brightness==80,"defaults intact after failed load");

    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
