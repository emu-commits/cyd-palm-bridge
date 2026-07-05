/* calc_test.c -- exercise the calculator engine. No server. */
#include <stdio.h>
#include <math.h>
#include "../bridge/calc.h"

static int fails=0;
/* expect a successful eval close to want */
static void OK(const char*e,double want){
    double v=0; int rc=calc_eval(e,&v);
    int ok = (rc==CALC_OK) && fabs(v-want) < 1e-9;
    printf("  %-16s = %-10g rc=%d %s\n",e,v,rc,ok?"ok":"FAIL");
    if(!ok) fails++;
}
/* expect a specific error code */
static void ERR(const char*e,int want){
    double v=0; int rc=calc_eval(e,&v);
    int ok = (rc==want);
    printf("  %-16s -> rc=%d (want %d) %s\n",e?e:"(null)",rc,want,ok?"ok":"FAIL");
    if(!ok) fails++;
}

int main(void){
    printf("== calculator ==\n");
    OK("2+3",5);
    OK("2+3*4",14);            /* precedence */
    OK("(2+3)*4",20);          /* parentheses override */
    OK("10/4",2.5);            /* real division */
    OK("-5+2",-3);             /* leading unary minus */
    OK("2*-3",-6);             /* unary after operator */
    OK("  3 +   4 ",7);        /* whitespace ignored */
    OK("3.5*2",7);
    OK("2*(3+(4-1))",12);      /* nesting */
    OK("100-1-1",98);          /* left-associative */
    OK("+7",7);                /* leading unary plus */
    OK("2*3+4*5",26);
    OK(".5+.5",1);             /* leading-dot decimals */

    printf("-- errors --\n");
    ERR("1/0",CALC_ERR_DIVZERO);
    ERR("(2+3)/(1-1)",CALC_ERR_DIVZERO);
    ERR("2+",CALC_ERR_SYNTAX);
    ERR("2 3",CALC_ERR_SYNTAX);      /* two numbers, no operator */
    ERR("((1+2)",CALC_ERR_SYNTAX);   /* unbalanced */
    ERR("",CALC_ERR_SYNTAX);
    ERR("   ",CALC_ERR_SYNTAX);
    ERR("abc",CALC_ERR_SYNTAX);
    ERR("*3",CALC_ERR_SYNTAX);
    ERR(0,CALC_ERR_SYNTAX);

    /* pathological deep nesting must be rejected, not overflow the stack */
    char deep[512]; int k=0;
    for(int i=0;i<200;i++) deep[k++]='(';
    deep[k++]='1'; deep[k]=0;
    ERR(deep,CALC_ERR_SYNTAX);   /* 200 '(' exceeds the depth cap */

    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
