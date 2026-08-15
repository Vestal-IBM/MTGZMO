#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <math.h>

/* ---- Display size (must match lv_conf.h) ---- */
#define DISP_HOR_RES 240
#define DISP_VER_RES 240

/* ---- Encoder ---- */
#define ENCODER_A     2
#define ENCODER_B     3
#define ENCODER_PPR   600           /* Pulses per revolution (one channel) */
#define RPM_UPDATE_MS 50            /* Recalculate RPM every 50 ms */
#define METER_MAX_RPM 4000

/* Pulse counter incremented in ISR */
static volatile uint32_t pulse_count = 0;

void encoderA_isr()
{
    pulse_count++;
}

/* ---- Draw buffer ---- */
#define DRAW_BUF_ROWS 10
static lv_color_t    draw_buf_1[DISP_HOR_RES * DRAW_BUF_ROWS];

static TFT_eSPI      tft;
static lv_disp_t    *disp;

/* LVGL widgets updated each RPM cycle */
static lv_obj_t              *meter;
static lv_meter_indicator_t  *needle;
static lv_obj_t              *rpm_label;
static lv_obj_t              *rpm_readout;

/* ---- LVGL flush callback ---- */
static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)color_p, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(drv);
}

/* ---- Build the tachometer UI ---- */
static void create_tachometer(void)
{
    /* Dark charcoal screen background */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x1A1A1A), LV_PART_MAIN);

    meter = lv_meter_create(lv_scr_act());
    lv_obj_set_size(meter, 240, 240);
    lv_obj_center(meter);

    /* Dark face, no visible border, no padding so ticks reach the edge */
    lv_obj_set_style_bg_color(meter, lv_color_hex(0x2B2B2B), LV_PART_MAIN);
    lv_obj_set_style_border_width(meter, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(meter, 0, LV_PART_MAIN);

    /* Scale: 0–400 internally (labels show 0–4 = ×1000 RPM), 240° sweep.
       100 internal units = 1000 RPM, giving smooth per-100-RPM needle steps. */
    lv_meter_scale_t *scale = lv_meter_add_scale(meter);
    lv_meter_set_scale_range(meter, scale, 0, 400, 240, 150);

    /* Minor ticks: 41 total (every 10 units = 100 RPM) — white, 4px long */
    lv_meter_set_scale_ticks(meter, scale, 41, 2, 4, lv_color_hex(0xDDDDDD));

    /* Major ticks every 1000 RPM (every 10th minor) — 50% wider (5px), 14px long */
    lv_meter_set_scale_major_ticks(meter, scale, 10, 5, 14,
                                   lv_color_hex(0xFFFFFF), 0);

    /* Hide auto-generated tick labels by making them transparent */
    lv_obj_set_style_text_opa(meter, LV_OPA_TRANSP, LV_PART_TICKS);

    /* Mid ticks at every 500 RPM: second scale with 9 ticks across 0–400,
       every tick is a "major" (step=1) drawn at 8px long, 2px wide — white.
       9 ticks = 8 intervals of 50 units = 50,100,150,200,250,300,350,400.
       The ones that fall on 1000 RPM boundaries overlap the main scale ticks
       which is fine — they are the same colour and will just reinforce them. */
    lv_meter_scale_t *scale_mid = lv_meter_add_scale(meter);
    lv_meter_set_scale_range(meter, scale_mid, 0, 400, 240, 150);
    lv_meter_set_scale_ticks(meter, scale_mid, 9, 2, 14, lv_color_hex(0xDDDDDD));
    lv_meter_set_scale_major_ticks(meter, scale_mid, 1, 2, 14,
                                   lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_text_opa(meter, LV_OPA_TRANSP, LV_PART_TICKS);

    /* Manually place labels 0–4 at the correct angular positions.
       LVGL start_angle=150 is 150° CW from top (12-o'clock).
       In standard trig (CCW from right): 150° CW from top = 300° CCW from right.
       Each label step = 60° CW in screen = -60° in trig. */
    static const char *tick_strs[] = { "0", "1", "2", "3", "4" };
    static const int label_r = 93;   /* radius from meter centre */
    static const int cx = 120;       /* meter centre x on screen */
    static const int cy = 120;       /* meter centre y on screen */
    for (int i = 0; i <= 4; i++) {
        double angle_deg = 207.0 - i * 60.0;
        double angle_rad = angle_deg * 3.14159265 / 180.0;
        int lx = (int)(cx + label_r * cos(angle_rad));
        int ly = (int)(cy - label_r * sin(angle_rad));
        lv_obj_t *lbl = lv_label_create(lv_scr_act());
        lv_label_set_text(lbl, tick_strs[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_set_pos(lbl, lx - 6, ly - 10);  /* offset to centre the glyph */
    }

    /* Red arc: 3500–4000 RPM — pushed outward */
    lv_meter_indicator_t *arc_red =
        lv_meter_add_arc(meter, scale, 10, lv_color_hex(0xCC0000), -4);
    lv_meter_set_indicator_start_value(meter, arc_red, 350);
    lv_meter_set_indicator_end_value(meter, arc_red, 400);

    /* Yellow arc: 3300–3500 RPM — same thickness, same outward position */
    lv_meter_indicator_t *arc_yellow =
        lv_meter_add_arc(meter, scale, 10, lv_color_hex(0xDDAA00), -4);
    lv_meter_set_indicator_start_value(meter, arc_yellow, 330);
    lv_meter_set_indicator_end_value(meter, arc_yellow, 350);

    /* White needle — position driven by rpm/10 in loop() */
    needle = lv_meter_add_needle_line(meter, scale, 3,
                                      lv_color_hex(0xEEEEEE), -10);
    lv_meter_set_indicator_start_value(meter, needle, 0);
    lv_meter_set_indicator_end_value(meter, needle, 0);

    /* "×1000 rpm" static label near top-centre */
    rpm_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(rpm_label, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_label_set_text(rpm_label, "x1000 rpm");
    lv_obj_align(rpm_label, LV_ALIGN_CENTER, 0, -30);

    /* RPM readout box — dark rounded rectangle in the lower centre */
    lv_obj_t *rpm_box = lv_obj_create(lv_scr_act());
    lv_obj_set_size(rpm_box, 90, 30);
    lv_obj_align(rpm_box, LV_ALIGN_CENTER, 0, 60);
    lv_obj_set_style_bg_color(rpm_box, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_border_color(rpm_box, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_border_width(rpm_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(rpm_box, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rpm_box, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rpm_box, LV_OBJ_FLAG_SCROLLABLE);

    rpm_readout = lv_label_create(rpm_box);
    lv_label_set_text(rpm_readout, "0000");
    lv_obj_set_style_text_color(rpm_readout, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(rpm_readout, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(rpm_readout);
}

void setup()
{
    /* Encoder — interrupt on rising edge of channel A */
    pinMode(ENCODER_A, INPUT_PULLUP);
    pinMode(ENCODER_B, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(ENCODER_A), encoderA_isr, RISING);

    /* Initialise display */
    tft.begin();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);

    /* Initialise LVGL 8 */
    lv_init();

    /* Register display driver (LVGL 8 API) */
    static lv_disp_draw_buf_t draw_buf_dsc;
    lv_disp_draw_buf_init(&draw_buf_dsc, draw_buf_1, nullptr,
                          DISP_HOR_RES * DRAW_BUF_ROWS);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res   = DISP_HOR_RES;
    disp_drv.ver_res   = DISP_VER_RES;
    disp_drv.flush_cb  = disp_flush;
    disp_drv.draw_buf  = &draw_buf_dsc;
    lv_disp_drv_register(&disp_drv);

    create_tachometer();
}

void loop()
{
    /* Drive LVGL tick from millis() */
    static uint32_t last_tick_ms = 0;
    uint32_t now = millis();
    lv_tick_inc(now - last_tick_ms);
    last_tick_ms = now;

    static uint32_t last_rpm_ms = 0;
    static uint32_t last_pulses = 0;
    static uint32_t smooth_rpm  = 0;   /* exponentially smoothed RPM */

    if (now - last_rpm_ms >= RPM_UPDATE_MS)
    {
        /* Snapshot pulse count atomically */
        noInterrupts();
        uint32_t current_pulses = pulse_count;
        interrupts();

        uint32_t delta  = current_pulses - last_pulses;
        last_pulses     = current_pulses;
        last_rpm_ms     = now;

        /* RPM = (Δpulses × 60000) / (PPR × interval_ms) */
        uint32_t raw_rpm = (delta * 60000UL) / (ENCODER_PPR * RPM_UPDATE_MS);
        if (raw_rpm > METER_MAX_RPM) raw_rpm = METER_MAX_RPM;

        /* Exponential smoothing: smooth = 0.6*smooth + 0.4*raw
           Multiply by 10 to keep integer precision, then divide back. */
        smooth_rpm = (smooth_rpm * 6 + raw_rpm * 4) / 10;

        /* Scale needle to 0–400 range (scale units = RPM / 10) */
        lv_meter_set_indicator_end_value(meter, needle, (int32_t)(smooth_rpm / 10));

        /* Update live RPM readout */
        char buf[8];
        snprintf(buf, sizeof(buf), "%04lu", smooth_rpm);
        lv_label_set_text(rpm_readout, buf);
    }

    lv_timer_handler();
    delay(5);
}
