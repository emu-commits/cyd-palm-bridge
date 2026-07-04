/* touch.c -- XPT2046 resistive touch, bit-banged (both HW SPI hosts are taken).
 *
 * Pins (CYD ESP32-2432S028R): CLK 25, MOSI(DIN) 32, MISO(DOUT) 39, CS 33,
 * IRQ(PENIRQ) 36. GPIO 39/36 are input-only (no internal pulls) -- fine, the
 * XPT2046 drives DOUT and idles PENIRQ high, pulling low on touch.
 *
 * Calibration is applied in touch_read(): raw ADC (~200..3900) -> screen px.
 * The constants below are refined from real corner taps during U2 (see
 * docs/BUILD_PROGRESS.md); start as identity-ish and get replaced with measured
 * values. Portrait 240x320.
 */
#include "touch.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define T_CLK  25
#define T_MOSI 32
#define T_MISO 39
#define T_CS   33
#define T_IRQ  36

/* XPT2046 control bytes: start bit + channel + 12-bit differential mode. */
#define CMD_X 0xD0    /* X position */
#define CMD_Y 0x90    /* Y position */
#define CMD_Z1 0xB0
#define CMD_Z2 0xC0

/* ---- affine calibration: screen = A*rawx + B*rawy + C (per axis) ----
 * A general affine handles axis swap, flip, scale and skew in one shot, solved
 * from 3 on-screen crosshair taps (touch_calibrate). Defaults are a rough
 * identity so touch_read is safe before calibration. */
static float ax_=0.06f, bx_=0.0f,  cx_=-18.0f;
static float ay_=0.0f,  by_=0.09f, cy_=-18.0f;

static uint16_t read_ch(uint8_t cmd);   /* fwd decl (used by touch_pressed) */

static void clk_pulse(void){
    gpio_set_level(T_CLK, 1); esp_rom_delay_us(1);
    gpio_set_level(T_CLK, 0); esp_rom_delay_us(1);
}

static uint16_t xfer(uint8_t cmd){
    gpio_set_level(T_CS, 0);
    for(int i=7;i>=0;i--){                 /* 8-bit command, MSB first */
        gpio_set_level(T_MOSI, (cmd>>i)&1);
        clk_pulse();
    }
    gpio_set_level(T_MOSI, 0);
    clk_pulse();                           /* 1 busy clock after the command */
    uint16_t v = 0;                        /* then exactly 12 data bits, MSB first */
    for(int i=0;i<12;i++){
        gpio_set_level(T_CLK, 1); esp_rom_delay_us(1);
        v = (v<<1) | (gpio_get_level(T_MISO) & 1);
        gpio_set_level(T_CLK, 0); esp_rom_delay_us(1);
    }
    gpio_set_level(T_CS, 1);
    return v & 0x0FFF;
}

void touch_init(void){
    gpio_config_t out = {
        .pin_bit_mask = (1ULL<<T_CLK)|(1ULL<<T_MOSI)|(1ULL<<T_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_config_t in = {
        .pin_bit_mask = (1ULL<<T_MISO)|(1ULL<<T_IRQ),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);
    gpio_set_level(T_CS, 1);
    gpio_set_level(T_CLK, 0);
}

/* Pressure-based touch detection (independent of the PENIRQ pin, which varies by
 * board). When untouched z1 reads near 0. A press raises z1 -- but the panel reads
 * a much LOWER z1 near its edges (resistive-divider geometry), so a low threshold
 * is needed to reach the right edge, with a double-read debounce to reject the
 * occasional idle spike. Measured: center press ~800, right-edge press ~150-196,
 * idle mostly <30 with rare spikes to ~250. */
#define TOUCH_Z1_MIN 110

int touch_pressed(void){
    if(read_ch(CMD_Z1) <= TOUCH_Z1_MIN) return 0;
    return read_ch(CMD_Z1) > TOUCH_Z1_MIN;   /* confirm (kills single-sample glitches) */
}

void touch_read_debug(uint16_t *x, uint16_t *y, uint16_t *z1, uint16_t *z2){
    uint16_t a=read_ch(CMD_Z1), b=read_ch(CMD_Z2), rx=read_ch(CMD_X), ry=read_ch(CMD_Y);
    if(z1)*z1=a;
    if(z2)*z2=b;
    if(x)*x=rx;
    if(y)*y=ry;
}

/* median of 5 to reject spikes */
static uint16_t read_ch(uint8_t cmd){
    uint16_t s[5];
    for(int i=0;i<5;i++) s[i]=xfer(cmd);
    for(int i=1;i<5;i++){ uint16_t k=s[i]; int j=i-1;
        while(j>=0 && s[j]>k){ s[j+1]=s[j]; j--; } s[j+1]=k; }
    return s[2];
}

int touch_read_raw(uint16_t *x, uint16_t *y, uint16_t *z){
    if(!touch_pressed()) return 0;
    uint16_t rx = read_ch(CMD_X);
    uint16_t ry = read_ch(CMD_Y);
    uint16_t z1 = xfer(CMD_Z1), z2 = xfer(CMD_Z2);
    uint16_t pz = (uint16_t)(z1 + 4095 - z2);
    if(x)*x=rx;
    if(y)*y=ry;
    if(z)*z=pz;
    return 1;
}

int touch_read(int *sx, int *sy){
    uint16_t rx, ry, rz;
    if(!touch_read_raw(&rx, &ry, &rz)) return 0;
    float fx = ax_*rx + bx_*ry + cx_;
    float fy = ay_*rx + by_*ry + cy_;
    int x = (int)(fx + 0.5f), y = (int)(fy + 0.5f);
    if(x<0)x=0; else if(x>=LCD_W)x=LCD_W-1;
    if(y<0)y=0; else if(y>=LCD_H)y=LCD_H-1;
    if(sx)*sx=x;
    if(sy)*sy=y;
    return 1;
}

/* solve [rx ry 1]·[a b c]ᵀ = t for the 3 sample points (exact affine). */
static void solve3(const int rx[3], const int ry[3], const int t[3],
                   float *a, float *b, float *c){
    float det =  rx[0]*(float)(ry[1]-ry[2]) - ry[0]*(float)(rx[1]-rx[2])
               + (float)(rx[1]*ry[2] - rx[2]*ry[1]);
    if(det==0.0f){ *a=0;*b=0;*c=0; return; }
    float da = t[0]*(float)(ry[1]-ry[2]) - ry[0]*(float)(t[1]-t[2])
             + (float)(t[1]*ry[2] - t[2]*ry[1]);
    float db = rx[0]*(float)(t[1]-t[2]) - t[0]*(float)(rx[1]-rx[2])
             + (float)(rx[1]*t[2] - rx[2]*t[1]);
    float dc = rx[0]*(float)(ry[1]*t[2]-t[1]*ry[2]) - ry[0]*(float)(rx[1]*t[2]-t[1]*rx[2])
             + t[0]*(float)(rx[1]*ry[2]-rx[2]*ry[1]);
    *a=da/det; *b=db/det; *c=dc/det;
}

static void draw_cross(int x, int y, uint16_t col){
    display_fill_rect(x-10, y-1, 21, 3, col);
    display_fill_rect(x-1, y-10, 3, 21, col);
}
static void avg_touch(int *rx, int *ry){
    long sx=0, sy=0; int n=0;
    for(int i=0;i<24 && n<16;i++){
        uint16_t x,y,z1,z2; touch_read_debug(&x,&y,&z1,&z2);
        if(z1>TOUCH_Z1_MIN){ sx+=x; sy+=y; n++; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    *rx = n? (int)(sx/n):0;
    *ry = n? (int)(sy/n):0;
}

void touch_calibrate(void){
    /* inset 40px: the touch digitizer's active area is slightly smaller than the
     * LCD, so targets at the extreme edge can be unreachable. Affine extrapolates. */
    const int tx[3] = { 40, LCD_W-40, 40 };
    const int ty[3] = { 40, 40, LCD_H-40 };
    int rx[3], ry[3];
    for(int i=0;i<3;i++){
        display_fill(C_BLACK);
        draw_cross(tx[i], ty[i], C_WHITE);
        while(!touch_pressed()) vTaskDelay(pdMS_TO_TICKS(20));   /* wait for tap */
        avg_touch(&rx[i], &ry[i]);
        draw_cross(tx[i], ty[i], C_GREEN);                       /* confirm      */
        while(touch_pressed()) vTaskDelay(pdMS_TO_TICKS(20));    /* wait release */
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    solve3(rx, ry, tx, &ax_, &bx_, &cx_);
    solve3(rx, ry, ty, &ay_, &by_, &cy_);
}

void touch_cal_save(void){
    nvs_handle_t h;
    if(nvs_open("touch", NVS_READWRITE, &h) != ESP_OK) return;
    float c[6] = { ax_, bx_, cx_, ay_, by_, cy_ };
    nvs_set_blob(h, "cal", c, sizeof c);
    nvs_commit(h);
    nvs_close(h);
}

int touch_cal_load(void){
    nvs_handle_t h;
    if(nvs_open("touch", NVS_READONLY, &h) != ESP_OK) return 0;
    float c[6]; size_t sz = sizeof c;
    esp_err_t e = nvs_get_blob(h, "cal", c, &sz);
    nvs_close(h);
    if(e != ESP_OK || sz != sizeof c) return 0;
    ax_=c[0]; bx_=c[1]; cx_=c[2]; ay_=c[3]; by_=c[4]; cy_=c[5];
    return 1;
}
