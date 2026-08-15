/**
 * lv_conf.h — LVGL 8.3 configuration for Raspberry Pi Pico + GC9A01
 */

#if 1 /* Set to "1" to enable content */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* ---- Colour depth ---- */
#define LV_COLOR_DEPTH 16   /* 16-bit RGB565 */

/* ---- Memory ---- */
#define LV_MEM_SIZE (24U * 1024U)  /* 24 KB for LVGL heap */

/* ---- Display resolution (must match DISP_HOR_RES / DISP_VER_RES) ---- */
#define LV_HOR_RES_MAX 240
#define LV_VER_RES_MAX 240

/* ---- Tick source ---- */
#define LV_TICK_CUSTOM 0   /* We call lv_tick_inc() manually from loop() */

/* ---- Features ---- */
#define LV_USE_LABEL    1
#define LV_USE_BTN      1
#define LV_USE_IMG      1
#define LV_USE_LINE     1
#define LV_USE_ARC      1
#define LV_USE_BAR      1
#define LV_USE_SLIDER   1
#define LV_USE_SWITCH   1
#define LV_USE_METER    1   /* Tachometer needle widget */
#define LV_USE_TEXTAREA 0
#define LV_USE_SPINBOX  0
#define LV_USE_KEYBOARD 0
#define LV_USE_CHART    0
#define LV_USE_TABLE    0

/* ---- Font ---- */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_UNSCII_16     1   /* Fixed-width font for RPM readout */
#define LV_FONT_DEFAULT       &lv_font_montserrat_14

/* ---- Logging ---- */
#define LV_USE_LOG 0

/* ---- Asserts (disable in production) ---- */
#define LV_USE_ASSERT_NULL   0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_ASSERT_OBJ    0
#define LV_USE_ASSERT_STYLE  0

#endif /* LV_CONF_H */
#endif /* Content enable */
