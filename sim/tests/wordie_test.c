/* wordie_test.c -- host gate for wordie.c (pure word-game logic).
 * Asserts deterministic daily/seeded answers, letter entry + delete bounds, the
 * two-pass scoring (including correct duplicate handling), keyboard-state folding,
 * and win/lose. Build+run via `make -C sim wordie`. Non-zero exit on any failure. */
#include "wordie.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d  %s\n",__FILE__,__LINE__,#c); fails++; } }while(0)

/* play a full known guess into g (assumes 5 letters, game in play). */
static void play(WdGame *g, const char *w){
    for(int i=0;i<5;i++) wd_addch(g, w[i]);
    wd_enter(g);
}

int main(void){
    /* --- determinism: same day => same answer; wraps modulo the bank --- */
    {
        WdGame a,b; wd_daily(&a, 100); wd_daily(&b, 100);
        CHECK(strcmp(a.answer,b.answer)==0);
        CHECK(strlen(a.answer)==5);
        WdGame c; wd_daily(&c, 100 + wd_nbank());     /* one full wrap -> same word */
        CHECK(strcmp(a.answer,c.answer)==0);
        WdGame d; wd_daily(&d, -1);                   /* negative day is well-defined */
        CHECK(strlen(d.answer)==5);
    }

    /* --- entry + delete bounds --- */
    {
        WdGame g; wd_daily(&g, 1);
        CHECK(wd_addch(&g,'c')==1); CHECK(g.row[0]=='C');   /* lowercased */
        CHECK(wd_addch(&g,'1')==0);                          /* non-letter rejected */
        wd_addch(&g,'r'); wd_addch(&g,'a'); wd_addch(&g,'n'); wd_addch(&g,'e');
        CHECK(g.cur==5);
        CHECK(wd_addch(&g,'x')==0);                          /* row full */
        CHECK(wd_del(&g)==1); CHECK(g.cur==4);
        CHECK(wd_enter(&g)==0);                              /* incomplete -> no submit */
        wd_addch(&g,'e'); CHECK(g.cur==5);
    }

    /* --- scoring: exact-position, present, absent against a fixed answer --- */
    {
        WdGame g; memset(&g,0,sizeof g); g.state=WD_PLAY;
        strcpy(g.answer,"CRANE");
        play(&g,"CRANE");
        CHECK(g.state==WD_WON);
        for(int i=0;i<5;i++) CHECK(g.mark[0][i]==WD_CORRECT);
    }
    {
        WdGame g; memset(&g,0,sizeof g); g.state=WD_PLAY;
        strcpy(g.answer,"CRANE");
        play(&g,"RCNEA");            /* a derangement: all five present, none in place */
        for(int i=0;i<5;i++) CHECK(g.mark[0][i]==WD_PRESENT);
        CHECK(g.state==WD_PLAY);
    }
    {
        WdGame g; memset(&g,0,sizeof g); g.state=WD_PLAY;
        strcpy(g.answer,"CRANE");
        play(&g,"GHOST");            /* nothing shared */
        for(int i=0;i<5;i++) CHECK(g.mark[0][i]==WD_ABSENT);
    }

    /* --- duplicate handling: answer has ONE 'E'; a guess with two E's credits only
     *     the correctly-placed one, the extra E is ABSENT (not PRESENT). --- */
    {
        WdGame g; memset(&g,0,sizeof g); g.state=WD_PLAY;
        strcpy(g.answer,"ABIDE");     /* single E, at index 4 */
        play(&g,"EERIE");             /* E E R I E : only the last E is CORRECT */
        CHECK(g.mark[0][0]==WD_ABSENT);   /* first E over-count -> absent */
        CHECK(g.mark[0][1]==WD_ABSENT);
        CHECK(g.mark[0][3]==WD_PRESENT);  /* I is present (answer index 2) */
        CHECK(g.mark[0][4]==WD_CORRECT);  /* final E in place */
    }

    /* --- keyboard state folds to the best (CORRECT beats PRESENT beats ABSENT) --- */
    {
        WdGame g; memset(&g,0,sizeof g); g.state=WD_PLAY;
        strcpy(g.answer,"CRANE");
        play(&g,"ARENA");             /* A present then... exercise several letters */
        CHECK(g.key['R'-'A']==WK_PRESENT || g.key['R'-'A']==WK_CORRECT);
        CHECK(g.key['Z'-'A']==WK_UNUSED);    /* untouched letter stays unused */
        play(&g,"CRANE");
        CHECK(g.key['C'-'A']==WK_CORRECT);   /* now correct */
        CHECK(g.state==WD_WON);
    }

    /* --- lose after six wrong guesses --- */
    {
        WdGame g; memset(&g,0,sizeof g); g.state=WD_PLAY;
        strcpy(g.answer,"CRANE");
        for(int i=0;i<6;i++) play(&g,"GHOST");
        CHECK(g.state==WD_LOST);
        CHECK(g.nrows==6);
        CHECK(wd_addch(&g,'A')==0);          /* no input once the game is over */
    }

    if(fails){ printf("wordie: %d FAILURES\n", fails); return 1; }
    printf("wordie logic: OK\n");
    return 0;
}
