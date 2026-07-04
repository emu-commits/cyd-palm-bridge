/* display.h -- minimal SPI TFT driver for the CYD (ILI9341/ST7789), U1 bring-up. */
#ifndef DISPLAY_H
#define DISPLAY_H
#include <stdint.h>

/* portrait: Palm-style PDA area on top, Graffiti input strip on the bottom */
#define LCD_W 240
#define LCD_H 320
#define GRAFFITI_H 112                /* bottom input strip height (usable writing box) */
#define PDA_H (LCD_H - GRAFFITI_H)    /* top app area height */

/* RGB565 */
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_RED    0xF800
#define C_GREEN  0x07E0
#define C_BLUE   0x001F
#define C_YELLOW 0xFFE0
#define C_LGRAY  0xC618
#define C_MGRAY  0x8410
#define C_DGRAY  0x2104

void display_init(void);
void display_fill(uint16_t color);
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_test_pattern(void);   /* 4 colored quadrants + yellow border */
/* blit a w*h block of RGB565 pixels (already in wire byte order) to (x,y). */
void display_blit(int x, int y, int w, int h, const void *px);

#endif
