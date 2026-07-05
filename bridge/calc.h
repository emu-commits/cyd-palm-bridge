/* calc.h -- a small arithmetic expression evaluator for the Calculator app.
 *
 * Evaluates an infix expression string with the four operators, parentheses,
 * unary +/-, and decimals, honouring precedence. Pure + allocation-free, so it
 * runs the same on the host (tested) and on the device. The Calculator UI feeds
 * it the entered expression and shows the result. */
#ifndef CALC_H
#define CALC_H

enum { CALC_OK = 0, CALC_ERR_SYNTAX, CALC_ERR_DIVZERO };

/* Evaluate `expr` into *out. Returns CALC_OK, or a CALC_ERR_* code (in which
 * case *out is untouched). Whitespace is ignored; supports + - * / ( ) and a
 * leading/factor-level unary - or +. */
int calc_eval(const char *expr, double *out);

#endif
