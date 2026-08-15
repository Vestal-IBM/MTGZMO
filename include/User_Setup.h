/**
 * User_Setup.h — TFT_eSPI configuration for GC9A01 on Raspberry Pi Pico
 *
 * Wiring (default SPI0):
 *   GC9A01 SCLK  → GPIO 18
 *   GC9A01 MOSI  → GPIO 19
 *   GC9A01 CS    → GPIO 17
 *   GC9A01 DC    → GPIO 16
 *   GC9A01 RST   → GPIO 20
 *   GC9A01 BL    → 3.3 V (or GPIO for PWM control)
 *
 * Adjust pin numbers below to match your wiring.
 */

#define GC9A01_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 240

/* SPI pins */
#define TFT_SCLK 18
#define TFT_MOSI 19
#define TFT_MISO -1   /* Not used */
#define TFT_CS   17
#define TFT_DC   16
#define TFT_RST  20
#define TFT_BL   -1   /* Set to a GPIO number if backlight is PWM-controlled */

/* SPI frequency */
#define SPI_FREQUENCY       40000000   /* 40 MHz */
#define SPI_READ_FREQUENCY   8000000

/* Colour order — most GC9A01 panels use BGR */
#define TFT_RGB_ORDER TFT_BGR

/* Use hardware SPI */
#define USE_HSPI_PORT  /* ignored on RP2040; uses SPI0 by default */

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
