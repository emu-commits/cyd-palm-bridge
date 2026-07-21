/* sudoku_test.c -- host gate for the pure Sudoku logic (no LVGL). */
#include <stdio.h>
#include <string.h>
#include "sudoku.h"

static int fails = 0;
#define CK(c,m) do{ if(!(c)){ fails++; printf("  FAIL: %s\n",(m)); } else printf("  ok: %s\n",(m)); }while(0)

/* count solutions of a board's `given` clues, capped at `limit`. Re-implements a
 * tiny solver here so the test is independent of the module's internals. */
static int can_place(const uint8_t *b, int p, int v){
    int r=p/9,c=p%9,br=(r/3)*3,bc=(c/3)*3;
    for(int i=0;i<9;i++){ if(b[r*9+i]==v) return 0; if(b[i*9+c]==v) return 0; }
    for(int dr=0;dr<3;dr++) for(int dc=0;dc<3;dc++) if(b[(br+dr)*9+bc+dc]==v) return 0;
    return 1;
}
static void solve_count(uint8_t *b,int p,int limit,int *n){
    if(*n>=limit) return;
    while(p<81 && b[p]) p++;
    if(p>=81){ (*n)++; return; }
    for(int v=1;v<=9;v++) if(can_place(b,p,v)){ b[p]=v; solve_count(b,p+1,limit,n); b[p]=0; if(*n>=limit) return; }
}
/* reference first-solution solver, filling `b` in place. */
static int tst_fill(uint8_t *b, int p){
    while(p<81 && b[p]) p++;
    if(p>=81) return 1;
    for(int v=1;v<=9;v++) if(can_place(b,p,v)){ b[p]=v; if(tst_fill(b,p+1)) return 1; b[p]=0; }
    return 0;
}

int main(void){
    printf("== sudoku ==\n");

    /* a generated puzzle: valid clue layout, unique solution, not already solved */
    SdGame g; sd_new(&g, 12345u, 48);
    int clues=0; for(int i=0;i<81;i++) if(g.given[i]) clues++;
    CK(clues>=17 && clues<=81, "clue count in range");
    CK(clues < 81, "some cells were dug out");

    /* clues must themselves be conflict-free */
    int cluebad=0;
    for(int r=0;r<9;r++) for(int c=0;c<9;c++) if(sd_is_given(&g,r,c) && sd_conflict(&g,r,c)) cluebad=1;
    CK(!cluebad, "clues have no conflicts");

    /* the puzzle has exactly one solution */
    uint8_t tmp[81]; memcpy(tmp,g.given,81);
    int n=0; solve_count(tmp,0,2,&n);
    CK(n==1, "puzzle has a unique solution");

    /* board starts unsolved with empty cells */
    CK(g.state==SD_PLAY, "starts in play");
    CK(sd_remaining(&g) == 81-clues, "remaining == empty cells");
    CK(!sd_solved(&g), "not solved at start");

    /* determinism: same seed -> identical puzzle */
    SdGame g2; sd_new(&g2, 12345u, 48);
    CK(memcmp(g.given,g2.given,81)==0, "same seed -> same clues");
    SdGame g3; sd_new(&g3, 999u, 48);
    CK(memcmp(g.given,g3.given,81)!=0, "different seed -> different clues");

    /* placement rules: cannot edit a clue; can set/clear a blank */
    int gr=-1,gc=-1,br=-1,bc=-1;
    for(int r=0;r<9 && gr<0;r++) for(int c=0;c<9 && gr<0;c++) if(sd_is_given(&g,r,c)){ gr=r; gc=c; }
    for(int r=0;r<9 && br<0;r++) for(int c=0;c<9 && br<0;c++) if(!sd_is_given(&g,r,c)){ br=r; bc=c; }
    CK(sd_set(&g,gr,gc,5)==0, "clue cell rejects edits");
    CK(sd_set(&g,br,bc,7)==1, "blank cell accepts a digit");
    CK(sd_at(&g,br,bc)==7, "digit landed");
    CK(sd_set(&g,br,bc,0)==1, "blank cell clears");
    CK(sd_at(&g,br,bc)==0, "cleared to empty");
    CK(sd_set(&g,br,bc,99)==0, "out-of-range value rejected");

    /* conflict detection: place a value that already sits in the row */
    int rr=-1,rc=-1; uint8_t dup=0;
    for(int r=0;r<9 && rr<0;r++){
        int blank=-1, val=0;
        for(int c=0;c<9;c++){ if(!sd_is_given(&g,r,c) && sd_at(&g,r,c)==0) blank=c; else if(g.given[r*9+c]) val=g.given[r*9+c]; }
        if(blank>=0 && val){ rr=r; rc=blank; dup=val; }
    }
    CK(rr>=0, "found a row with a blank + a clue to duplicate");
    sd_set(&g,rr,rc,dup);
    CK(sd_conflict(&g,rr,rc)==1, "duplicate in row flagged as conflict");
    sd_set(&g,rr,rc,0);
    CK(sd_conflict(&g,rr,rc)==0, "cleared cell no longer conflicts");

    /* a WRONG-but-feasible digit is NOT a conflict: only true rule violations
     * (row/col/box duplicates) get flagged in the UI. Compute the unique solution,
     * then find a blank cell that admits a legal digit other than its solution
     * value and assert placing it raises no conflict. */
    uint8_t soln[81]; memcpy(soln, g.given, 81); tst_fill(soln,0);
    int fr=-1,fc=-1,fv=0;
    for(int r=0;r<9 && fr<0;r++) for(int c=0;c<9 && fr<0;c++){
        if(sd_is_given(&g,r,c) || sd_at(&g,r,c)) continue;      /* blank, editable cell */
        for(int v=1;v<=9;v++){
            if(v==soln[r*9+c]) continue;                        /* skip the correct answer */
            if(can_place(g.cell,r*9+c,v)){ fr=r; fc=c; fv=v; break; }   /* legal but wrong */
        }
    }
    CK(fr>=0, "found a blank cell with a feasible-but-wrong digit");
    sd_set(&g,fr,fc,fv);
    CK(sd_at(&g,fr,fc)==fv, "feasible-but-wrong digit landed");
    CK(sd_conflict(&g,fr,fc)==0, "feasible-but-wrong digit is NOT flagged (no slash)");
    CK(fv != soln[fr*9+fc], "...and it really is the wrong answer");
    sd_set(&g,fr,fc,0);

    /* solving it: compute the unique solution with the reference solver, then fill
     * every blank of a fresh copy of the same puzzle -> SD_SOLVED. */
    uint8_t full[81]; memcpy(full, g.given, 81);
    CK(tst_fill(full,0)==1, "reference solver solved the clues");
    SdGame gs; sd_new(&gs, 12345u, 48);
    for(int r=0;r<9;r++) for(int c=0;c<9;c++) if(!sd_is_given(&gs,r,c)) sd_set(&gs,r,c, full[r*9+c]);
    CK(sd_remaining(&gs)==0, "board filled");
    CK(sd_solved(&gs), "correctly filled board is solved");
    CK(gs.state==SD_SOLVED, "state flips to solved");

    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails ? 1 : 0;
}
