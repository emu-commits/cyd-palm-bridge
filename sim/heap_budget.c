/* heap_budget.c -- see sim_heap.h. Linked with
 *   -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
 * so every libc allocation routes through the budget. Accounting uses
 * malloc_usable_size() (glibc and Emscripten's dlmalloc both provide it), so
 * no headers are prepended and pointers from any allocator free correctly.
 * Enforcement starts only once armed (sim_heap_arm, called after boot):
 * boot-time allocations are the baseline the device also pays before its
 * "free heap" is measured. */
#include "sim_heap.h"
#include <malloc.h>   /* malloc_usable_size */
#include <string.h>

void *__real_malloc(size_t n);
void *__real_calloc(size_t nm, size_t sz);
void *__real_realloc(void *p, size_t n);
void  __real_free(void *p);

static long long s_used;    /* signed: a free of a pre-arm block may dip below 0 */
static long long s_peak;
static size_t    s_budget;  /* 0 = not armed (account only, never refuse) */

void sim_heap_arm(size_t budget){ s_budget = budget; s_used = 0; s_peak = 0; }
size_t sim_heap_used(void)     { return s_used > 0 ? (size_t)s_used : 0; }
size_t sim_heap_peak(void)     { return s_peak > 0 ? (size_t)s_peak : 0; }
size_t sim_heap_freebytes(void){
    long long f = (long long)s_budget - s_used;
    return f > 0 ? (size_t)f : 0;
}

static int over(size_t want){
    return s_budget && s_used + (long long)want > (long long)s_budget;
}
static void acct(void *p){
    if(!p) return;
    s_used += (long long)malloc_usable_size(p);
    if(s_used > s_peak) s_peak = s_used;
}

void *__wrap_malloc(size_t n){
    if(over(n)) return 0;
    void *p = __real_malloc(n);
    acct(p);
    return p;
}

void *__wrap_calloc(size_t nm, size_t sz){
    if(sz && nm > (size_t)-1 / sz) return 0;     /* overflow */
    if(over(nm * sz)) return 0;
    void *p = __real_calloc(nm, sz);
    acct(p);
    return p;
}

void *__wrap_realloc(void *p, size_t n){
    long long old = p ? (long long)malloc_usable_size(p) : 0;
    if(s_budget && s_used - old + (long long)n > (long long)s_budget) return 0;
    void *q = __real_realloc(p, n);
    if(q){
        s_used -= old;
        acct(q);
    }
    return q;
}

void __wrap_free(void *p){
    if(!p) return;
    s_used -= (long long)malloc_usable_size(p);
    __real_free(p);
}
