/* mines_test.c -- host gate for minesweeper.c (pure board logic).
 * Asserts first-tap safety, deterministic boards, flood-reveal, flagging, and
 * win/lose. Build+run via `make -C sim mines`. Exits non-zero on any failure. */
#include "minesweeper.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d  %s\n",__FILE__,__LINE__,#c); fails++; } }while(0)

int main(void){
    /* --- first-tap safety: the tapped cell and its 3x3 neighbourhood are mine-free,
     *     and the first reveal never loses --- */
    for(uint32_t seed=1; seed<=200; seed++){
        MsGame g; ms_new(&g, 9, 9, 10, seed);
        int rr=4, cc=4;
        int st = ms_reveal(&g, rr, cc);
        CHECK(st != MS_LOST);
        for(int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++){
            int r=rr+dr, c=cc+dc;
            if(r<0||c<0||r>=9||c>=9) continue;
            CHECK(!(ms_at(&g,r,c) & MS_MINE));
        }
        /* exactly the requested number of mines were placed */
        int m=0; for(int i=0;i<81;i++) if(g.cell[i]&MS_MINE) m++;
        CHECK(m == 10);
    }

    /* --- determinism: same seed => identical mine layout --- */
    {
        MsGame a,b; ms_new(&a,9,9,10,1234); ms_new(&b,9,9,10,1234);
        ms_reveal(&a,0,0); ms_reveal(&b,0,0);
        int same=1; for(int i=0;i<81;i++) if((a.cell[i]&MS_MINE)!=(b.cell[i]&MS_MINE)) same=0;
        CHECK(same);
        MsGame c; ms_new(&c,9,9,10,9999); ms_reveal(&c,0,0);
        int diff=0; for(int i=0;i<81;i++) if((a.cell[i]&MS_MINE)!=(c.cell[i]&MS_MINE)) diff=1;
        CHECK(diff);                                  /* a different seed differs */
    }

    /* --- flood: the first reveal opens more than one cell (a zero region) --- */
    {
        int flooded=0;
        for(uint32_t seed=1; seed<=50; seed++){
            MsGame g; ms_new(&g,9,9,10,seed); ms_reveal(&g,4,4);
            int rev=0; for(int i=0;i<81;i++) if(g.cell[i]&MS_REVEALED) rev++;
            if(rev>1) flooded++;
        }
        CHECK(flooded > 40);                          /* the safe 3x3 makes floods the norm */
    }

    /* --- flagging: toggles, counts, and blocks reveal --- */
    {
        MsGame g; ms_new(&g,9,9,10,7);
        ms_flag(&g,0,0); CHECK(ms_at(&g,0,0)&MS_FLAG); CHECK(ms_flags(&g)==1);
        ms_reveal(&g,0,0); CHECK(!(ms_at(&g,0,0)&MS_REVEALED));  /* flagged => no reveal */
        ms_flag(&g,0,0); CHECK(!(ms_at(&g,0,0)&MS_FLAG)); CHECK(ms_flags(&g)==0);
    }

    /* --- win: revealing every non-mine cell wins --- */
    {
        MsGame g; ms_new(&g,5,5,3,42);
        for(int r=0;r<5;r++) for(int c=0;c<5;c++)
            if(!(ms_at(&g,r,c)&MS_MINE)) ms_reveal(&g,r,c);
        CHECK(g.state == MS_WON);
    }

    /* --- lose: revealing a mine loses --- */
    {
        MsGame g; ms_new(&g,9,9,10,3); ms_reveal(&g,4,4);   /* place mines */
        int mr=-1,mc=-1;
        for(int r=0;r<9&&mr<0;r++) for(int c=0;c<9;c++) if(ms_at(&g,r,c)&MS_MINE){ mr=r; mc=c; break; }
        CHECK(mr>=0);
        CHECK(ms_reveal(&g,mr,mc) == MS_LOST);
        CHECK(g.state == MS_LOST);
    }

    if(fails){ printf("mines: %d FAILURES\n", fails); return 1; }
    printf("mines logic: OK\n");
    return 0;
}
