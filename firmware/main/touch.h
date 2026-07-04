/* touch.h -- XPT2046 resistive touch (bit-banged SPI) for the CYD. */
#ifndef TOUCH_H
#define TOUCH_H
#include <stdint.h>

void touch_init(void);
int  touch_pressed(void);                       /* 1 if the panel is being touched */
/* read averaged raw ADC values (0..4095). returns 1 if a valid touch was read. */
int  touch_read_raw(uint16_t *x, uint16_t *y, uint16_t *z);
/* read raw then map to screen pixels via the current calibration. 1 if touched. */
int  touch_read(int *sx, int *sy);

/* unconditional debug read of all four channels (no touch gating) -- U2 tuning. */
void touch_read_debug(uint16_t *x, uint16_t *y, uint16_t *z1, uint16_t *z2);

/* interactive 3-point affine calibration: draws crosshairs, waits for taps,
 * solves screen = A*rawx + B*rawy + C for both axes. */
void touch_calibrate(void);

/* persist / restore the affine calibration in NVS. load returns 1 if found. */
void touch_cal_save(void);
int  touch_cal_load(void);

#endif
