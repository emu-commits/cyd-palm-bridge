/* sim_heap.h -- a device-like ceiling on the general heap (see heap_budget.c).
 *
 * The LVGL object pool is already exact device parity (24 KB, lv_conf.h). This
 * adds the OTHER half of the device's memory model: general malloc/calloc/
 * realloc -- the record sort buffers, Find results, row-uid arrays, stdio
 * buffers -- is capped at a budget approximating the CYD's interactive-mode
 * free heap, so allocations that would fail on hardware fail here too and the
 * UI's "(low memory)" degrade paths are exercised.
 *
 * The budget is ARMED after boot (sim_heap_arm): boot-time allocations are the
 * baseline, and the budget models "free heap once the device is up" -- the
 * UI_ROADMAP Mode A analysis measures that at ~140 KB with Wi-Fi off. Override
 * with -DSIM_HEAP_BUDGET=<bytes> for experiments.
 */
#ifndef SIM_HEAP_H
#define SIM_HEAP_H
#include <stddef.h>

#ifndef SIM_HEAP_BUDGET
#define SIM_HEAP_BUDGET (144 * 1024)   /* ~ Mode A interactive free heap */
#endif

void   sim_heap_arm(size_t budget);    /* start enforcing; used resets to 0 */
size_t sim_heap_used(void);            /* bytes allocated since arming      */
size_t sim_heap_peak(void);            /* high-water mark since arming      */
size_t sim_heap_freebytes(void);       /* budget - used (0 when exhausted)  */

#endif
