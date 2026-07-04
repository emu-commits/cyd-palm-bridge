/* display.c -- dependency-free SPI TFT driver for the CYD (ILI9341 default).
 *
 * U1 bring-up: enough to init the panel and blit rectangles. No managed
 * components. TFT is on SPI3 (SD is on SPI2 -> no bus contention). If the panel
 * turns out to be an ST7789 (CYD batch variance), set PANEL_ST7789 to 1; if
 * colors look swapped/negative, tweak MADCTL_VAL / INVERT below.
 */
#include "display.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "lcd";

/* ---- CYD ESP32-2432S028R display pins ---- */
#define PIN_SCLK 14
#define PIN_MOSI 13
#define PIN_MISO -1
#define PIN_DC    2
#define PIN_CS   15
#define PIN_RST  -1        /* not wired on typical CYD; use software reset */
#define PIN_BL   21
#define LCD_HOST SPI3_HOST

/* portrait 240x320; flip bits here if orientation/color is wrong */
#define MADCTL_VAL 0x48    /* MV=0 (portrait), MX=1, BGR=1 */
#define INVERT     0       /* 1 = INVON (ST7789-style panels often need this) */
#define PANEL_ST7789 0

static spi_device_handle_t s_spi;

static void lcd_dc(int v){ gpio_set_level(PIN_DC, v); }

static void lcd_cmd(uint8_t c){
    lcd_dc(0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &c };
    spi_device_polling_transmit(s_spi, &t);
}
static void lcd_data(const uint8_t *d, int n){
    if(n <= 0) return;
    lcd_dc(1);
    spi_transaction_t t = { .length = 8*n, .tx_buffer = d };
    spi_device_polling_transmit(s_spi, &t);
}
static void lcd_data8(uint8_t d){ lcd_data(&d, 1); }

static void set_window(int x, int y, int w, int h){
    uint8_t b[4];
    int x1 = x+w-1, y1 = y+h-1;
    lcd_cmd(0x2A); b[0]=x>>8; b[1]=x; b[2]=x1>>8; b[3]=x1; lcd_data(b,4);
    lcd_cmd(0x2B); b[0]=y>>8; b[1]=y; b[2]=y1>>8; b[3]=y1; lcd_data(b,4);
    lcd_cmd(0x2C);
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color){
    if(w<=0 || h<=0) return;
    set_window(x, y, w, h);
    static uint8_t line[LCD_W*2];
    for(int i=0;i<w;i++){ line[2*i]=color>>8; line[2*i+1]=color; }
    lcd_dc(1);
    for(int r=0;r<h;r++){
        spi_transaction_t t = { .length = 8*2*w, .tx_buffer = line };
        spi_device_polling_transmit(s_spi, &t);
    }
}

void display_fill(uint16_t color){ display_fill_rect(0,0,LCD_W,LCD_H,color); }

void display_blit(int x, int y, int w, int h, const void *px){
    if(w<=0 || h<=0) return;
    set_window(x, y, w, h);
    lcd_dc(1);
    spi_transaction_t t = { .length = (size_t)w*h*16, .tx_buffer = px };
    spi_device_polling_transmit(s_spi, &t);
}

static void panel_init_seq(void){
    lcd_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));   /* software reset */
    lcd_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));   /* sleep out       */
    lcd_cmd(0x3A); lcd_data8(0x55);                  /* 16-bit RGB565   */
    lcd_cmd(0x36); lcd_data8(MADCTL_VAL);            /* mem access ctl  */
#if PANEL_ST7789
    lcd_cmd(0xB2); { uint8_t p[]={0x0C,0x0C,0x00,0x33,0x33}; lcd_data(p,sizeof p); }
    lcd_cmd(0xB7); lcd_data8(0x35);
    lcd_cmd(0xBB); lcd_data8(0x19);
    lcd_cmd(0xC0); lcd_data8(0x2C);
    lcd_cmd(0xC2); lcd_data8(0x01);
    lcd_cmd(0xC3); lcd_data8(0x12);
    lcd_cmd(0xC4); lcd_data8(0x20);
#endif
    lcd_cmd(INVERT ? 0x21 : 0x20);                   /* inversion on/off */
    lcd_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(20));    /* display on       */
}

void display_init(void){
    gpio_config_t io = {
        .pin_bit_mask = (1ULL<<PIN_DC) | (1ULL<<PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_BL, 1);          /* backlight on */

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK, .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W*60*2,   /* fits an LVGL partial buffer blit */
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        /* TFT pins route via the GPIO matrix on SPI3 (they're SPI2's IOMUX pins),
         * which caps SPI at ~26.7 MHz. 20 MHz is safe and ample for the UI. */
        .clock_speed_hz = 20*1000*1000,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &dev, &s_spi));

    panel_init_seq();
    ESP_LOGI(TAG, "panel init done (%dx%d, MADCTL 0x%02X, invert %d)", LCD_W, LCD_H, MADCTL_VAL, INVERT);
}

/* Portrait layout preview + display test:
 *   - top: PDA area = a blue title bar over a light-gray body
 *   - bottom: Graffiti input strip in mid-gray, split by a dark divider
 *   - orientation dots: RED top-left, GREEN top-right (confirms (0,0)=top-left)
 *   - yellow border to confirm full addressing with no offset          */
void display_test_pattern(void){
    display_fill(C_LGRAY);                                   /* PDA body bg   */
    display_fill_rect(0, 0, LCD_W, 22, C_BLUE);              /* title bar     */
    display_fill_rect(0, PDA_H, LCD_W, 2, C_DGRAY);          /* PDA|Graffiti divider */
    display_fill_rect(0, PDA_H+2, LCD_W, GRAFFITI_H-2, C_MGRAY);          /* Graffiti bg */
    display_fill_rect(LCD_W*3/5, PDA_H+2, 2, GRAFFITI_H-2, C_DGRAY);      /* letters|numbers split (Palm-style) */
    display_fill_rect(2, 2, 10, 10, C_RED);                  /* TL dot        */
    display_fill_rect(LCD_W-12, 2, 10, 10, C_GREEN);         /* TR dot        */
    display_fill_rect(0, 0, LCD_W, 3, C_YELLOW);             /* borders       */
    display_fill_rect(0, LCD_H-3, LCD_W, 3, C_YELLOW);
    display_fill_rect(0, 0, 3, LCD_H, C_YELLOW);
    display_fill_rect(LCD_W-3, 0, 3, LCD_H, C_YELLOW);
    ESP_LOGI(TAG, "portrait layout: blue title bar, gray PDA body, gray Graffiti strip at bottom; RED dot TL, GREEN dot TR");
}
