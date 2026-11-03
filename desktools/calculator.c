#include "calculator.h"
#include <stdlib.h>
#include <math.h>
#include <errno.h>
struct parser { const char *p,*error;unsigned depth; };
static void spaces(struct parser *s) { while(*s->p==' ' || *s->p=='\t') s->p++; }
static double sum(struct parser *s);
static double atom(struct parser *s) {
    spaces(s);
    if(++s->depth>32) { s->error="Expression is too deeply nested.";s->depth--;return 0; }
    double value=0;
    if(*s->p=='+' || *s->p=='-') { int negative=*s->p++=='-';value=atom(s);if(negative) value=-value; }
    else if(*s->p=='(') {
        s->p++;value=sum(s);spaces(s);
        if(*s->p!=')') s->error="A closing parenthesis is missing.";else s->p++;
    } else {
        char *end;errno=0;value=strtod(s->p,&end);
        if(errno==ERANGE) s->error="The number is outside the supported range.";
        if(end==s->p) s->error="Enter a number or parenthesized expression.";
        else s->p=end;
    }
    if(!s->error && !isfinite(value)) s->error="The result is outside the supported range.";
    s->depth--;return value;
}
static double product(struct parser *s) {
    double value=atom(s);spaces(s);
    while(!s->error && (*s->p=='*' || *s->p=='/')) {
        char op=*s->p++;double rhs=atom(s);
        if(!s->error && op=='/' && rhs==0) { s->error="Division by zero.";return 0; }
        value=op=='*'?value*rhs:value/rhs;spaces(s);
        if(!s->error && !isfinite(value)) s->error="The result is outside the supported range.";
    }
    return value;
}
static double sum(struct parser *s) {
    double value=product(s);spaces(s);
    while(!s->error && (*s->p=='+' || *s->p=='-')) {
        char op=*s->p++;double rhs=product(s);value=op=='+'?value+rhs:value-rhs;spaces(s);
        if(!s->error && !isfinite(value)) s->error="The result is outside the supported range.";
    }
    return value;
}
const char *desk_calculate(const char *expression,double *result) {
    struct parser s={.p=expression};double value=sum(&s);spaces(&s);
    if(!s.error && *s.p) s.error="Unexpected character in the expression.";
    if(!s.error) *result=value;
    return s.error;
}
