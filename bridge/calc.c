/* calc.c -- recursive-descent arithmetic evaluator. See calc.h.
 *
 * Grammar (standard precedence, left-associative):
 *   expr   = term  (('+' | '-') term)*
 *   term   = factor(('*' | '/') factor)*
 *   factor = ('+' | '-') factor | number | '(' expr ')'
 *   number = digits ['.' digits]  |  '.' digits
 */
#include <ctype.h>
#include <stdlib.h>
#include "calc.h"

/* depth-bounded so a pathological input ("((((((...") can't overflow the stack
 * on the device -- recursion tracks paren nesting + unary chains. */
#define CALC_MAX_DEPTH 64
typedef struct { const char *p; int err; int depth; } P;

static void skip(P *s){ while(*s->p==' '||*s->p=='\t') s->p++; }

static double expr(P *s);   /* fwd */

static double factor(P *s){
    if(++s->depth > CALC_MAX_DEPTH){ s->err=CALC_ERR_SYNTAX; s->depth--; return 0; }
    skip(s);
    char c=*s->p;
    if(c=='+'){ s->p++; double v=factor(s); s->depth--; return v; }
    if(c=='-'){ s->p++; double v=-factor(s); s->depth--; return v; }
    if(c=='('){
        s->p++;
        double v=expr(s);
        skip(s);
        if(*s->p==')') s->p++; else s->err=CALC_ERR_SYNTAX;
        s->depth--;
        return v;
    }
    if(isdigit((unsigned char)c) || c=='.'){
        char *end=NULL;
        double v=strtod(s->p,&end);
        if(end==s->p) s->err=CALC_ERR_SYNTAX; else s->p=end;
        s->depth--;
        return v;
    }
    s->err=CALC_ERR_SYNTAX;
    s->depth--;
    return 0;
}

static double term(P *s){
    double v=factor(s);
    for(;;){
        skip(s);
        char c=*s->p;
        if(c=='*'){ s->p++; v*=factor(s); }
        else if(c=='/'){
            s->p++;
            double d=factor(s);
            if(d==0.0){ s->err=CALC_ERR_DIVZERO; return 0; }
            v/=d;
        } else break;
        if(s->err) return 0;
    }
    return v;
}

static double expr(P *s){
    double v=term(s);
    for(;;){
        skip(s);
        char c=*s->p;
        if(c=='+'){ s->p++; v+=term(s); }
        else if(c=='-'){ s->p++; v-=term(s); }
        else break;
        if(s->err) return 0;
    }
    return v;
}

int calc_eval(const char *expr_str, double *out){
    if(!expr_str) return CALC_ERR_SYNTAX;
    P s={ .p=expr_str, .err=CALC_OK };
    skip(&s);
    if(!*s.p) return CALC_ERR_SYNTAX;      /* empty expression */
    double v=expr(&s);
    if(s.err) return s.err;
    skip(&s);
    if(*s.p) return CALC_ERR_SYNTAX;       /* trailing garbage */
    if(out) *out=v;
    return CALC_OK;
}
