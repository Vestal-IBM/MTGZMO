#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <math.h>
#include <SPI.h>
/* ---- Stepper driver (TMC5160 via TMCStepper library) ----
   To swap drivers: replace this include, the defines, the driver object,
   stepper_init(), and stepper_set_rpm() below. Nothing else needs to change. */
#include <TMCStepper.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>

/* ---- Display size (must match lv_conf.h) ---- */
#define DISP_HOR_RES 240
#define DISP_VER_RES 240

/* ---- Encoder ---- */
#define ENCODER_A     2
#define ENCODER_B     3
static volatile uint32_t encoder_ppr  = 600;   /* Pulses per revolution — runtime adjustable */
static volatile uint32_t hob_threads    = 1;   /* Stepper RPM units — ratio numerator   */
static volatile uint32_t gear_teeth     = 32;  /* Encoder RPM units — ratio denominator */
static volatile uint32_t pulley_driver  = 1;   /* Teeth on stepper-side pulley           */
static volatile uint32_t pulley_driven  = 1;   /* Teeth on output-side pulley            */
#define RPM_UPDATE_MS 50            /* Recalculate RPM every 50 ms */
#define METER_MAX_RPM 4000

/* ---- Stepper pin / parameter defines (TMC5160-specific) ---- */
#define TMC_MOSI        11          /* SPI1 TX  */
#define TMC_MISO        12          /* SPI1 RX  */
#define TMC_SCK         10          /* SPI1 SCK */
#define TMC_CS          13          /* Chip select */
#define TMC_EN          15          /* Enable — LOW = enabled, HIGH = disabled */
#define TMC_R_SENSE     0.075f      /* Sense resistor value in ohms */
#define TMC_RMS_CURRENT 1000        /* Motor RMS current in mA */

#define BTN_PIN         14          /* IP display button — pulled up, active LOW */
#define MOTOR_STEPS     200         /* Full steps per revolution */
#define MICROSTEPS      256         /* Microstep resolution */

/* ---- WiFi ---- */
#define WIFI_AP_SSID  "MTGizmo"
#define WIFI_AP_PASS  "hobbing1"        /* min 8 chars for WPA2 */
#define WIFI_HOSTNAME "gizmo.mt"        /* DNS name in AP mode */

enum WifiMode { MODE_AP, MODE_STA };
static WifiMode   wifi_mode              = MODE_AP;
static String     sta_ssid               = "";
static String     sta_password           = "";
static bool       wifi_reconfig_pending  = false;  /* set by web handler, applied in loop() */
static String     current_ip   = "192.168.4.1";
static DNSServer  dnsServer;

/* output_rpm  = encoder_rpm × (hob_threads / gear_teeth)
   stepper_rpm = output_rpm  × (pulley_driven / pulley_driver)
   combined:  stepper_rpm = encoder_rpm × (hob_threads / gear_teeth) × (pulley_driven / pulley_driver) */
static volatile bool  encoder_reversed  = false;  /* flip encoder direction via web UI */

static TMC5160Stepper  driver(TMC_CS, TMC_R_SENSE, TMC_MOSI, TMC_MISO, TMC_SCK);
static WebServer       server(80);

/* Signed position counter — incremented or decremented in ISR based on direction */
static volatile int32_t encoder_pos = 0;

void encoderA_isr()
{
    /* Channel B HIGH when A rises → forward; LOW → reverse.
       encoder_reversed flips the sense without rewiring. */
    bool forward = (digitalRead(ENCODER_B) != encoder_reversed);
    if (forward)
        encoder_pos++;
    else
        encoder_pos--;
}

/* ---- Draw buffer ---- */
#define DRAW_BUF_ROWS 10
static lv_color_t    draw_buf_1[DISP_HOR_RES * DRAW_BUF_ROWS];

static TFT_eSPI      tft;

/* LVGL widgets updated each RPM cycle */
static lv_obj_t              *meter;
static lv_meter_indicator_t  *needle;
static lv_obj_t              *rpm_label;
static lv_obj_t              *rpm_readout;
static lv_obj_t              *ip_overlay;     /* IP address pop-up label */

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
    lv_obj_set_style_text_font(rpm_readout, &lv_font_unscii_16, LV_PART_MAIN);
    lv_obj_center(rpm_readout);

    /* IP address overlay — dark pill centred on screen, hidden until button press */
    lv_obj_t *ip_box = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ip_box, 210, 54);
    lv_obj_align(ip_box, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_color(ip_box, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_border_color(ip_box, lv_color_hex(0x3b82d4), LV_PART_MAIN);
    lv_obj_set_style_border_width(ip_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(ip_box, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ip_box, 6, LV_PART_MAIN);
    lv_obj_clear_flag(ip_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ip_box, LV_OBJ_FLAG_HIDDEN);

    ip_overlay = lv_label_create(ip_box);
    lv_label_set_text(ip_overlay, "");
    lv_label_set_long_mode(ip_overlay, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ip_overlay, 198);
    lv_obj_set_style_text_color(ip_overlay, lv_color_hex(0x3b82d4), LV_PART_MAIN);
    lv_obj_set_style_text_font(ip_overlay, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(ip_overlay, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(ip_overlay);
}

/* ---- Show overlay notification ---- */
static void show_overlay(const char *text, uint32_t duration_ms);   /* forward decl */

/* ==========================================================================
   STEPPER DRIVER ABSTRACTION — TMC5160 implementation
   To substitute a different driver, replace stepper_init() and
   stepper_set_rpm() only. The rest of the firmware calls only these two.
   ========================================================================== */

/* TMC5160 VMAX conversion:
   VMAX [internal units] = RPM × MOTOR_STEPS × MICROSTEPS × 2^24 / 60 / fCLK
   fCLK = 12 MHz (internal oscillator) */
#define TMC_FCLK     12000000UL
#define RPM_TO_VMAX(rpm) \
    ((uint32_t)((float)(rpm) * MOTOR_STEPS * MICROSTEPS * 16777216.0f / 60.0f / TMC_FCLK))

/* Initialise the driver. Called once from setup() after the display is ready. */
static void stepper_init()
{
    pinMode(TMC_EN, OUTPUT);
    digitalWrite(TMC_EN, HIGH);     /* Keep disabled until fully configured */

    driver.begin();
    driver.reset();
    driver.rms_current(TMC_RMS_CURRENT);
    driver.microsteps(MICROSTEPS);
    driver.en_pwm_mode(true);       /* StealthChop */
    driver.pwm_autoscale(true);
    driver.RAMPMODE(1);             /* Start in positive velocity mode */
    driver.AMAX(500);               /* Acceleration limit (tune to taste) */
    driver.DMAX(500);               /* Deceleration limit */
    driver.VMAX(0);                 /* Start stopped */

    digitalWrite(TMC_EN, LOW);      /* Enable driver */
}

/* Set stepper shaft speed in RPM. Positive = forward, negative = reverse, 0 = stop. */
static void stepper_set_rpm(float rpm)
{
    uint32_t vmax = RPM_TO_VMAX(fabsf(rpm));
    if (rpm > 0.0f) {
        driver.RAMPMODE(1);
    } else if (rpm < 0.0f) {
        driver.RAMPMODE(2);
    } else {
        driver.VMAX(0);
        return;
    }
    driver.VMAX(vmax);
}

/* ==========================================================================
   END STEPPER DRIVER ABSTRACTION
   ========================================================================== */

/* ---- Compute required stepper RPM from encoder RPM and apply it ---- */
static void set_stepper_rpm(int32_t signed_rpm)
{
    /* stepper must produce the hobbing-ratio output at the final shaft, corrected for belt/pulley */
    float hobbing     = (float)hob_threads   / (float)gear_teeth;
    float belt        = (float)pulley_driven / (float)pulley_driver;
    float target_rpm  = (float)signed_rpm * hobbing * belt;
    stepper_set_rpm(target_rpm);
}

/* ---- Startup sweep animation ---- */
static void needle_anim_cb(void *obj, int32_t val)
{
    /* val is the scale value (0–400); update needle and readout in sync */
    lv_meter_set_indicator_end_value(meter, needle, val);

    char buf[8];
    snprintf(buf, sizeof(buf), "%04lu", (uint32_t)(val * 10));
    lv_label_set_text(rpm_readout, buf);
}

static void run_startup_animation(void)
{
    /* Sweep up 0 → 400 in 1200 ms, then back 400 → 0 in 1200 ms */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, needle_anim_cb);
    lv_anim_set_var(&a, NULL);          /* unused — we access globals directly */
    lv_anim_set_values(&a, 0, 400);
    lv_anim_set_time(&a, 1200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_playback_time(&a, 1200);
    lv_anim_set_playback_delay(&a, 100);
    lv_anim_start(&a);

    /* Run LVGL until the full animation (sweep up + sweep down) completes.
       Total duration = time + playback_delay + playback_time = 2500 ms */
    uint32_t start = millis();
    uint32_t last_tick = start;
    while (millis() - start < 2600)
    {
        uint32_t now = millis();
        lv_tick_inc(now - last_tick);
        last_tick = now;
        lv_timer_handler();
        delay(5);
    }

    /* Reset needle and readout to zero before handing off to loop() */
    lv_meter_set_indicator_end_value(meter, needle, 0);
    lv_label_set_text(rpm_readout, "0000");
}

/* ---- Web UI ---- */
static const String WEB_PAGE =
    "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Machine Tool Gizmo</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,sans-serif;background:#111;color:#eee;"
    "display:flex;flex-direction:column;align-items:center;"
    "min-height:100vh;padding:0}"
    /* tab bar */
    ".tabs{display:flex;width:100%;max-width:420px;border-bottom:1px solid #333;"
    "margin-top:24px}"
    ".tab{flex:1;padding:12px 0;text-align:center;font-size:.9rem;font-weight:600;"
    "color:#666;cursor:pointer;border-bottom:2px solid transparent;transition:.15s}"
    ".tab.active{color:#fff;border-bottom-color:#3b82d4}"
    /* pages */
    ".page{display:none;flex-direction:column;align-items:center;"
    "width:100%;max-width:420px;gap:20px;padding:24px 16px 40px}"
    ".page.active{display:flex}"
    /* cards */
    "h1{font-size:1.3rem;font-weight:600;color:#fff;align-self:flex-start;padding:0 4px}"
    ".card{background:#1e1e1e;border:1px solid #333;border-radius:10px;"
    "padding:24px 28px;width:100%;display:flex;flex-direction:column;gap:16px}"
    "h2{font-size:.9rem;font-weight:600;color:#aaa}"
    "label{font-size:.78rem;color:#888;text-transform:uppercase;letter-spacing:.08em}"
    ".current{font-size:2.4rem;font-weight:700;color:#3b82d4;text-align:center;"
    "font-variant-numeric:tabular-nums}"
    "input[type=number],input[type=text],input[type=password]{"
    "width:100%;padding:10px 14px;background:#2a2a2a;border:1px solid #444;"
    "border-radius:6px;color:#fff;font-size:1rem;outline:none}"
    "input:focus{border-color:#3b82d4}"
    "select{width:100%;padding:10px 14px;background:#2a2a2a;border:1px solid #444;"
    "border-radius:6px;color:#fff;font-size:1rem;outline:none}"
    ".presets{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}"
    ".preset{padding:8px;background:#2a2a2a;border:1px solid #444;border-radius:6px;"
    "color:#ccc;font-size:.8rem;cursor:pointer;text-align:center}"
    ".preset:hover{border-color:#3b82d4;color:#fff}"
    ".toggle{display:flex;align-items:center;gap:12px;cursor:pointer}"
    ".toggle input{display:none}"
    ".slider{width:44px;height:24px;background:#444;border-radius:12px;"
    "position:relative;transition:.2s}"
    ".slider::after{content:'';position:absolute;width:18px;height:18px;"
    "background:#fff;border-radius:50%;top:3px;left:3px;transition:.2s}"
    "input:checked+.slider{background:#3b82d4}"
    "input:checked+.slider::after{left:23px}"
    ".tlabel{font-size:.9rem;color:#ccc}"
    "button[type=submit]{width:100%;padding:12px;background:#3b82d4;border:none;"
    "border-radius:6px;color:#fff;font-size:1rem;font-weight:600;cursor:pointer}"
    "button[type=submit]:hover{background:#2563eb}"
    ".msg{font-size:.85rem;text-align:center;color:#4ade80;min-height:1.2em}"
    ".err{color:#f87171}"
    ".info{font-size:.75rem;color:#666;text-align:center}"
    "</style></head><body>"
    /* ---- Tab bar ---- */
    "<div class='tabs'>"
    "<div class='tab active' id='tabHome' onclick='showPage(\"home\")'>Home</div>"
    "<div class='tab' id='tabSettings' onclick='showPage(\"settings\")'>Settings</div>"
    "</div>"
    /* ---- Home page ---- */
    "<div class='page active' id='pageHome'>"
    "<h1>Machine Tool Gizmo</h1>"
    "<div class='card'>"
    "<h2>Gear Ratio</h2>"
    "<label>Current ratio</label>"
    "<div class='current' id='cur'>__HOB__ : __TEETH__</div>"
    "<form id='fRatio'>"
    "<label for='hobVal'>Threads on hob</label>"
    "<input type='number' id='hobVal' name='hob' min='1' max='9999' step='1'"
    " placeholder='e.g. 1' required style='margin-top:6px'>"
    "<label for='teethVal' style='margin-top:10px'>Gear teeth</label>"
    "<input type='number' id='teethVal' name='teeth' min='1' max='9999' step='1'"
    " placeholder='e.g. 32' required style='margin-top:6px'>"
    "<button type='submit' style='margin-top:14px'>Set Ratio</button>"
    "</form>"
    "<div class='msg' id='msgRatio'></div>"
    "</div>"
    "</div>"
    /* ---- Settings page ---- */
    "<div class='page' id='pageSettings'>"
    "<h1>Settings</h1>"
    /* Encoder card */
    "<div class='card'>"
    "<h2>Encoder</h2>"
    "<label class='toggle'>"
    "<input type='checkbox' id='chkRev' __REVCHECKED__>"
    "<span class='slider'></span>"
    "<span class='tlabel'>Reverse direction</span>"
    "</label>"
    "<div class='msg' id='msgEnc'></div>"
    "</div>"
    /* PPR card */
    "<div class='card'>"
    "<h2>Encoder PPR</h2>"
    "<label>Current pulses per revolution</label>"
    "<div class='current' id='curPpr' style='font-size:2rem'>__PPR__</div>"
    "<form id='fPpr'>"
    "<label for='pprVal'>New PPR</label>"
    "<input type='number' id='pprVal' name='ppr' min='1' max='10000' step='1'"
    " placeholder='e.g. 600' required style='margin-top:6px'>"
    "<div class='presets' style='margin-top:10px'>"
    "<div class='preset' onclick='setPpr(100)'>100</div>"
    "<div class='preset' onclick='setPpr(200)'>200</div>"
    "<div class='preset' onclick='setPpr(400)'>400</div>"
    "<div class='preset' onclick='setPpr(600)'>600</div>"
    "<div class='preset' onclick='setPpr(1000)'>1000</div>"
    "<div class='preset' onclick='setPpr(2400)'>2400</div>"
    "</div>"
    "<button type='submit' style='margin-top:14px'>Set PPR</button>"
    "</form>"
    "<div class='msg' id='msgPpr'></div>"
    "</div>"
    /* Pulley / belt card */
    "<div class='card'>"
    "<h2>Stepper Pulley</h2>"
    "<label>Driver (stepper) : Driven (output)</label>"
    "<div class='current' id='curPulley'>__PULLEY_DRV__ : __PULLEY_DRN__</div>"
    "<form id='fPulley'>"
    "<label for='drvVal'>Driver teeth (stepper pulley)</label>"
    "<input type='number' id='drvVal' name='driver' min='1' max='9999' step='1'"
    " placeholder='e.g. 20' required style='margin-top:6px'>"
    "<label for='drnVal' style='margin-top:10px'>Driven teeth (output pulley)</label>"
    "<input type='number' id='drnVal' name='driven' min='1' max='9999' step='1'"
    " placeholder='e.g. 20' required style='margin-top:6px'>"
    "<button type='submit' style='margin-top:14px'>Set Pulley</button>"
    "</form>"
    "<div class='msg' id='msgPulley'></div>"
    "</div>"
    /* WiFi card */
    "<div class='card'>"
    "<h2>WiFi</h2>"
    "<div class='info'>Current IP: __IP__ (__WIFIMODE__)</div>"
    "<form id='fWifi'>"
    "<label for='wmode'>Mode</label>"
    "<select id='wmode' name='mode' onchange='toggleSTA()' style='margin-top:6px'>"
    "<option value='ap' __APSEL__>Access Point (AP)</option>"
    "<option value='sta' __STASEL__>Station (connect to router)</option>"
    "</select>"
    "<div id='staFields' style='margin-top:10px;flex-direction:column;gap:10px'>"
    "<div><label for='wssid'>SSID</label>"
    "<input type='text' id='wssid' name='ssid' placeholder='Network name' style='margin-top:4px'></div>"
    "<div><label for='wpass'>Password</label>"
    "<div style='display:flex;gap:8px;margin-top:4px'>"
    "<input type='password' id='wpass' name='pass' placeholder='Password' style='flex:1'>"
    "<button type='button' id='btnShow' onclick='togglePw()'"
    " style='padding:0 12px;background:#2a2a2a;border:1px solid #444;border-radius:6px;"
    "color:#ccc;cursor:pointer;font-size:.8rem;white-space:nowrap'>Show</button>"
    "</div></div>"
    "</div>"
    "<button type='submit' style='margin-top:14px'>Apply WiFi</button>"
    "</form>"
    "<div class='msg' id='msgWifi'></div>"
    "</div>"
    "</div>"
    "<script>"
    /* page switching */
    "function showPage(p){"
    "document.getElementById('pageHome').classList.toggle('active',p==='home');"
    "document.getElementById('pageSettings').classList.toggle('active',p==='settings');"
    "document.getElementById('tabHome').classList.toggle('active',p==='home');"
    "document.getElementById('tabSettings').classList.toggle('active',p==='settings');"
    "}"
    /* ratio form */
    "document.getElementById('fRatio').addEventListener('submit',async function(e){"
    "e.preventDefault();"
    "var h=parseInt(document.getElementById('hobVal').value);"
    "var t=parseInt(document.getElementById('teethVal').value);"
    "if(isNaN(h)||h<1||isNaN(t)||t<1){showMsg('msgRatio','Invalid value',true);return;}"
    "var res=await fetch('/set?hob='+h+'&teeth='+t,{method:'POST'});"
    "var txt=await res.text();"
    "document.getElementById('cur').textContent=h+' : '+t;"
    "showMsg('msgRatio',txt,false);"
    "});"
    /* encoder toggle */
    "document.getElementById('chkRev').addEventListener('change',async function(){"
    "var v=this.checked?'1':'0';"
    "var res=await fetch('/set-encoder?reversed='+v,{method:'POST'});"
    "var txt=await res.text();"
    "showMsg('msgEnc',txt,false);"
    "});"
    /* PPR form */
    "function setPpr(v){document.getElementById('pprVal').value=v;}"
    "document.getElementById('fPpr').addEventListener('submit',async function(e){"
    "e.preventDefault();"
    "var v=parseInt(document.getElementById('pprVal').value);"
    "if(isNaN(v)||v<1||v>10000){showMsg('msgPpr','Must be 1–10000',true);return;}"
    "var res=await fetch('/set-ppr?ppr='+v,{method:'POST'});"
    "var txt=await res.text();"
    "document.getElementById('curPpr').textContent=v;"
    "showMsg('msgPpr',txt,false);"
    "});"
    /* wifi mode toggle */
    "function toggleSTA(){"
    "var m=document.getElementById('wmode').value;"
    "document.getElementById('staFields').style.display=(m==='sta'?'flex':'none');"
    "}"
    "toggleSTA();"
    /* password reveal */
    "function togglePw(){"
    "var inp=document.getElementById('wpass');"
    "var btn=document.getElementById('btnShow');"
    "if(inp.type==='password'){inp.type='text';btn.textContent='Hide';}"
    "else{inp.type='password';btn.textContent='Show';}"
    "}"
    /* wifi form */
    "document.getElementById('fWifi').addEventListener('submit',async function(e){"
    "e.preventDefault();"
    "var m=document.getElementById('wmode').value;"
    "var s=document.getElementById('wssid').value;"
    "var p=document.getElementById('wpass').value;"
    "if(m==='sta'&&!s){showMsg('msgWifi','SSID required',true);return;}"
    "var url='/set-wifi?mode='+encodeURIComponent(m)"
    "+'&ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p);"
    "showMsg('msgWifi','Applying... reconnect if needed.',false);"
    "try{await fetch(url,{method:'GET'});}catch(e){}"
    "});"
    /* pulley form */
    "document.getElementById('fPulley').addEventListener('submit',async function(e){"
    "e.preventDefault();"
    "var d=parseInt(document.getElementById('drvVal').value);"
    "var n=parseInt(document.getElementById('drnVal').value);"
    "if(isNaN(d)||d<1||isNaN(n)||n<1){showMsg('msgPulley','Invalid value',true);return;}"
    "var res=await fetch('/set-pulley?driver='+d+'&driven='+n,{method:'POST'});"
    "var txt=await res.text();"
    "document.getElementById('curPulley').textContent=d+' : '+n;"
    "showMsg('msgPulley',txt,false);"
    "});"
    /* helper */
    "function showMsg(id,txt,err){"
    "var el=document.getElementById(id);"
    "el.textContent=txt;el.className=err?'msg err':'msg';"
    "setTimeout(function(){el.textContent='';},4000);"
    "}"
    "</script></body></html>";

static void apply_wifi_config();   /* forward declarations */
static void save_config();

static void handle_root()
{
    String page = WEB_PAGE;
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)hob_threads);
    page.replace("__HOB__", buf);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)gear_teeth);
    page.replace("__TEETH__", buf);
    page.replace("__REVCHECKED__", encoder_reversed ? "checked" : "");
    char ppr_buf[8];
    snprintf(ppr_buf, sizeof(ppr_buf), "%lu", (unsigned long)encoder_ppr);
    page.replace("__PPR__", ppr_buf);
    char pulley_drv_buf[8], pulley_drn_buf[8];
    snprintf(pulley_drv_buf, sizeof(pulley_drv_buf), "%lu", (unsigned long)pulley_driver);
    snprintf(pulley_drn_buf, sizeof(pulley_drn_buf), "%lu", (unsigned long)pulley_driven);
    page.replace("__PULLEY_DRV__", pulley_drv_buf);
    page.replace("__PULLEY_DRN__", pulley_drn_buf);
    page.replace("__IP__",       current_ip);
    page.replace("__WIFIMODE__", wifi_mode == MODE_AP ? "AP" : "STA");
    page.replace("__APSEL__",    wifi_mode == MODE_AP  ? "selected" : "");
    page.replace("__STASEL__",   wifi_mode == MODE_STA ? "selected" : "");
    server.send(200, "text/html", page);
}

static void handle_set()
{
    if (!server.hasArg("hob") || !server.hasArg("teeth")) {
        server.send(400, "text/plain", "Missing hob or teeth parameter");
        return;
    }
    uint32_t hob = (uint32_t)server.arg("hob").toInt();
    uint32_t tth = (uint32_t)server.arg("teeth").toInt();
    if (hob < 1 || hob > 9999 || tth < 1 || tth > 9999) {
        server.send(400, "text/plain", "hob and teeth must be 1–9999");
        return;
    }
    hob_threads = hob;
    gear_teeth  = tth;
    char buf[48];
    snprintf(buf, sizeof(buf), "Ratio set to %lu:%lu", (unsigned long)hob, (unsigned long)tth);
    server.send(200, "text/plain", buf);
    save_config();
}

static void handle_set_encoder()
{
    if (!server.hasArg("reversed")) {
        server.send(400, "text/plain", "Missing parameter");
        return;
    }
    encoder_reversed = (server.arg("reversed") == "1");
    server.send(200, "text/plain",
                encoder_reversed ? "Encoder reversed" : "Encoder normal");
    save_config();
}

static void handle_set_ppr()
{
    if (!server.hasArg("ppr")) {
        server.send(400, "text/plain", "Missing ppr parameter");
        return;
    }
    int val = server.arg("ppr").toInt();
    if (val < 1 || val > 10000) {
        server.send(400, "text/plain", "PPR must be between 1 and 10000");
        return;
    }
    encoder_ppr = (uint32_t)val;
    char buf[32];
    snprintf(buf, sizeof(buf), "PPR set to %d", val);
    server.send(200, "text/plain", buf);
    save_config();
}

static void handle_set_pulley()
{
    if (!server.hasArg("driver") || !server.hasArg("driven")) {
        server.send(400, "text/plain", "Missing driver or driven parameter");
        return;
    }
    uint32_t drv = (uint32_t)server.arg("driver").toInt();
    uint32_t drn = (uint32_t)server.arg("driven").toInt();
    if (drv < 1 || drv > 9999 || drn < 1 || drn > 9999) {
        server.send(400, "text/plain", "Pulley teeth must be 1–9999");
        return;
    }
    pulley_driver = drv;
    pulley_driven = drn;
    char buf[48];
    snprintf(buf, sizeof(buf), "Pulley set to %lu:%lu", (unsigned long)drv, (unsigned long)drn);
    server.send(200, "text/plain", buf);
    save_config();
}

/* ip_show_until is defined in loop() as a static — declare it here so show_overlay can set it */
static uint32_t g_ip_show_until = 0;

static void show_overlay(const char *text, uint32_t duration_ms)
{
    lv_label_set_text(ip_overlay, text);
    lv_obj_t *box = lv_obj_get_parent(ip_overlay);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_HIDDEN);
    g_ip_show_until = duration_ms ? (millis() + duration_ms) : 0;
}

static void handle_set_wifi()
{
    if (!server.hasArg("mode")) {
        server.send(400, "text/plain", "Missing mode");
        return;
    }
    String mode = server.arg("mode");
    if (mode == "ap") {
        wifi_mode = MODE_AP;
        server.send(200, "text/plain", "Switching to AP mode — connect to " WIFI_AP_SSID);
    } else if (mode == "sta") {
        if (!server.hasArg("ssid") || server.arg("ssid").length() == 0) {
            server.send(400, "text/plain", "SSID required for station mode");
            return;
        }
        sta_ssid     = server.arg("ssid");
        sta_password = server.hasArg("pass") ? server.arg("pass") : "";
        wifi_mode    = MODE_STA;
        server.send(200, "text/plain", "Connecting to " + sta_ssid + "...");
    } else {
        server.send(400, "text/plain", "Unknown mode");
        return;
    }
    /* Set flag — loop() will call apply_wifi_config() after handleClient() returns */
    wifi_reconfig_pending = true;
}

/* Helper: pump LVGL for ms milliseconds */
static void lvgl_delay(uint32_t ms)
{
    uint32_t t      = millis();
    uint32_t last_lv = t;
    while (millis() - t < ms) {
        uint32_t now2 = millis();
        lv_tick_inc(now2 - last_lv);
        last_lv = now2;
        lv_timer_handler();
        delay(10);
    }
}

static void apply_wifi_config()
{
    server.stop();
    dnsServer.stop();
    WiFi.disconnect(false);       /* disconnect without powering down radio */
    WiFi.softAPdisconnect(true);

    if (wifi_mode == MODE_STA) {
        show_overlay(("Connecting to " + sta_ssid).c_str(), 0);
        lvgl_delay(100);          /* let overlay render before radio init blocks */

        WiFi.mode(WIFI_STA);
        lvgl_delay(200);          /* settle after mode switch */
        WiFi.begin(sta_ssid.c_str(), sta_password.c_str());

        /* Wait up to 20 s, pumping LVGL so the display stays alive */
        uint32_t t       = millis();
        uint32_t last_lv = t;
        while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) {
            uint32_t now2 = millis();
            lv_tick_inc(now2 - last_lv);
            last_lv = now2;
            lv_timer_handler();
            delay(100);
        }

        if (WiFi.status() == WL_CONNECTED) {
            current_ip = WiFi.localIP().toString();
            show_overlay((sta_ssid + "\n" + current_ip).c_str(), 5000);
        } else {
            /* STA failed — fall back to AP visually but DO NOT change wifi_mode
               so saved config still has STA and will retry on next reboot */
            show_overlay(("Failed: " + sta_ssid + "\n" + WIFI_AP_SSID).c_str(), 4000);
            lvgl_delay(500);

            IPAddress local(192, 168, 4, 1);
            WiFi.mode(WIFI_AP);
            WiFi.softAPConfig(local, local, IPAddress(255, 255, 255, 0));
            WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
            current_ip = "192.168.4.1";
            dnsServer.start(53, WIFI_HOSTNAME, local);
            server.begin();
            return;   /* return early — don't save, preserving STA config */
        }
    }

    if (wifi_mode == MODE_AP) {
        show_overlay("Starting AP...", 0);
        lvgl_delay(100);

        IPAddress local(192, 168, 4, 1);
        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(local, local, IPAddress(255, 255, 255, 0));
        WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
        current_ip = "192.168.4.1";
        dnsServer.start(53, WIFI_HOSTNAME, local);

        show_overlay((String(WIFI_AP_SSID) + "\n192.168.4.1").c_str(), 4000);
    }

    server.begin();
    save_config();   /* only reached on successful connect or explicit AP switch */
}

/* ---- Persistent config (EEPROM) ----
   Layout (byte offsets):
     0        : magic byte (0xAE = valid data present)
     1        : wifi_mode (0 = AP, 1 = STA)
     2–65     : sta_ssid  (null-terminated, max 63 chars)
     66–129   : sta_password (null-terminated, max 63 chars)
     130–131  : hob_threads    (uint16_t, 2 bytes)
     132–133  : gear_teeth     (uint16_t, 2 bytes)
     134      : encoder_reversed (0 or 1)
     135–136  : encoder_ppr    (uint16_t, 2 bytes)
     137–138  : pulley_driver  (uint16_t, 2 bytes)
     139–140  : pulley_driven  (uint16_t, 2 bytes)         */
#define EE_MAGIC_ADDR      0
#define EE_MODE_ADDR       1
#define EE_SSID_ADDR       2
#define EE_PASS_ADDR       66
#define EE_HOB_ADDR        130   /* hob_threads uint16_t */
#define EE_TEETH_ADDR      132   /* gear_teeth  uint16_t */
#define EE_REV_ADDR        134
#define EE_PPR_ADDR        135
#define EE_PULLEY_DRV_ADDR 137   /* pulley_driver uint16_t */
#define EE_PULLEY_DRN_ADDR 139   /* pulley_driven uint16_t */
#define EE_TOTAL_SIZE      141
#define EE_MAGIC_VAL       0xAE   /* bumped from 0xAD — forces re-init on first boot */

static void save_config()
{
    EEPROM.begin(EE_TOTAL_SIZE);
    EEPROM.write(EE_MAGIC_ADDR, EE_MAGIC_VAL);
    EEPROM.write(EE_MODE_ADDR,  (uint8_t)wifi_mode);

    /* Write SSID — zero-pad the full field so stale data can't bleed through */
    for (int i = 0; i < 64; i++)
        EEPROM.write(EE_SSID_ADDR + i,
                     i < (int)sta_ssid.length() ? sta_ssid[i] : 0);

    /* Write password */
    for (int i = 0; i < 64; i++)
        EEPROM.write(EE_PASS_ADDR + i,
                     i < (int)sta_password.length() ? sta_password[i] : 0);

    /* Write hob_threads and gear_teeth as uint16_t (little-endian) */
    uint16_t hob = (uint16_t)hob_threads;
    uint16_t tth = (uint16_t)gear_teeth;
    EEPROM.write(EE_HOB_ADDR,     (uint8_t)(hob & 0xFF));
    EEPROM.write(EE_HOB_ADDR + 1, (uint8_t)(hob >> 8));
    EEPROM.write(EE_TEETH_ADDR,     (uint8_t)(tth & 0xFF));
    EEPROM.write(EE_TEETH_ADDR + 1, (uint8_t)(tth >> 8));

    /* Write encoder_reversed */
    EEPROM.write(EE_REV_ADDR, encoder_reversed ? 1 : 0);

    /* Write encoder_ppr as uint16_t (little-endian) */
    uint16_t ppr = (uint16_t)encoder_ppr;
    EEPROM.write(EE_PPR_ADDR,     (uint8_t)(ppr & 0xFF));
    EEPROM.write(EE_PPR_ADDR + 1, (uint8_t)(ppr >> 8));

    /* Write pulley_driver and pulley_driven as uint16_t (little-endian) */
    uint16_t pdrv = (uint16_t)pulley_driver;
    uint16_t pdrn = (uint16_t)pulley_driven;
    EEPROM.write(EE_PULLEY_DRV_ADDR,     (uint8_t)(pdrv & 0xFF));
    EEPROM.write(EE_PULLEY_DRV_ADDR + 1, (uint8_t)(pdrv >> 8));
    EEPROM.write(EE_PULLEY_DRN_ADDR,     (uint8_t)(pdrn & 0xFF));
    EEPROM.write(EE_PULLEY_DRN_ADDR + 1, (uint8_t)(pdrn >> 8));

    EEPROM.commit();
    EEPROM.end();
}

static void load_config()
{
    EEPROM.begin(EE_TOTAL_SIZE);

    if (EEPROM.read(EE_MAGIC_ADDR) != EE_MAGIC_VAL) {
        /* No valid data or layout changed — stay with defaults */
        EEPROM.end();
        return;
    }

    wifi_mode = (EEPROM.read(EE_MODE_ADDR) == 1) ? MODE_STA : MODE_AP;

    /* Read SSID */
    char buf[64];
    for (int i = 0; i < 63; i++) buf[i] = (char)EEPROM.read(EE_SSID_ADDR + i);
    buf[63] = '\0';
    sta_ssid = String(buf);

    /* Read password */
    for (int i = 0; i < 63; i++) buf[i] = (char)EEPROM.read(EE_PASS_ADDR + i);
    buf[63] = '\0';
    sta_password = String(buf);

    /* Read hob_threads and gear_teeth */
    uint16_t hob_load = (uint16_t)EEPROM.read(EE_HOB_ADDR)
                      | ((uint16_t)EEPROM.read(EE_HOB_ADDR + 1) << 8);
    uint16_t tth_load = (uint16_t)EEPROM.read(EE_TEETH_ADDR)
                      | ((uint16_t)EEPROM.read(EE_TEETH_ADDR + 1) << 8);
    if (hob_load >= 1 && hob_load <= 9999)
        hob_threads = hob_load;
    if (tth_load >= 1 && tth_load <= 9999)
        gear_teeth = tth_load;

    /* Read encoder_reversed */
    encoder_reversed = (EEPROM.read(EE_REV_ADDR) == 1);

    /* Read encoder_ppr */
    uint16_t ppr = (uint16_t)EEPROM.read(EE_PPR_ADDR)
                 | ((uint16_t)EEPROM.read(EE_PPR_ADDR + 1) << 8);
    if (ppr >= 1 && ppr <= 10000)
        encoder_ppr = ppr;

    /* Read pulley_driver and pulley_driven */
    uint16_t pdrv_load = (uint16_t)EEPROM.read(EE_PULLEY_DRV_ADDR)
                       | ((uint16_t)EEPROM.read(EE_PULLEY_DRV_ADDR + 1) << 8);
    uint16_t pdrn_load = (uint16_t)EEPROM.read(EE_PULLEY_DRN_ADDR)
                       | ((uint16_t)EEPROM.read(EE_PULLEY_DRN_ADDR + 1) << 8);
    if (pdrv_load >= 1 && pdrv_load <= 9999)
        pulley_driver = pdrv_load;
    if (pdrn_load >= 1 && pdrn_load <= 9999)
        pulley_driven = pdrn_load;

    EEPROM.end();
}

static void setup_wifi()
{
    load_config();
    server.on("/",             HTTP_GET,  handle_root);
    server.on("/set",          HTTP_POST, handle_set);
    server.on("/set-encoder",  HTTP_POST, handle_set_encoder);
    server.on("/set-ppr",      HTTP_POST, handle_set_ppr);
    server.on("/set-pulley",  HTTP_POST, handle_set_pulley);
    server.on("/set-wifi",    HTTP_GET,  handle_set_wifi);
    apply_wifi_config();
}

void setup()
{
    /* Encoder — interrupt on rising edge of channel A */
    pinMode(ENCODER_A, INPUT_PULLUP);
    pinMode(ENCODER_B, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(ENCODER_A), encoderA_isr, RISING);

    /* IP button */
    pinMode(BTN_PIN, INPUT_PULLUP);

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
    run_startup_animation();

    /* ---- Initialise stepper driver ---- */
    stepper_init();

    /* ---- Start WiFi AP and web server ---- */
    setup_wifi();
}

void loop()
{
    /* Drive LVGL tick from millis() */
    static uint32_t last_tick_ms = 0;
    static bool     btn_last     = HIGH;
    static uint32_t btn_down_at  = 0;      /* millis() when button was pressed */
    static bool     long_fired   = false;  /* long-press action already triggered */
    uint32_t now = millis();
    lv_tick_inc(now - last_tick_ms);
    last_tick_ms = now;

    /* ---- Button state machine ----
       Short press (< 2 s): show IP for 5 s
       Long press (>= 2 s): switch to AP mode            */
    const uint32_t LONG_PRESS_MS = 2000;

    bool btn_now = digitalRead(BTN_PIN);

    if (btn_last == HIGH && btn_now == LOW) {
        /* Falling edge — button just pressed */
        btn_down_at = now;
        long_fired  = false;
    }

    if (btn_now == LOW && !long_fired &&
        btn_down_at && (now - btn_down_at >= LONG_PRESS_MS))
    {
        /* Long press: switch to AP — apply_wifi_config() will show notifications */
        long_fired = true;
        wifi_mode  = MODE_AP;
        apply_wifi_config();
    }

    if (btn_last == LOW && btn_now == HIGH) {
        /* Rising edge — button released */
        if (!long_fired)
            show_overlay(current_ip.c_str(), 5000);  /* short press: show IP */
        btn_down_at = 0;
    }

    btn_last = btn_now;

    /* Hide overlay once the display window expires */
    if (g_ip_show_until && now >= g_ip_show_until) {
        lv_obj_add_flag(lv_obj_get_parent(ip_overlay), LV_OBJ_FLAG_HIDDEN);
        g_ip_show_until = 0;
    }

    static uint32_t last_rpm_ms  = 0;
    static int32_t  last_pos     = 0;
    static int32_t  smooth_rpm   = 0;   /* signed, exponentially smoothed RPM */

    if (now - last_rpm_ms >= RPM_UPDATE_MS)
    {
        /* Snapshot encoder position atomically */
        noInterrupts();
        int32_t current_pos = encoder_pos;
        interrupts();

        int32_t delta   = current_pos - last_pos;
        last_pos        = current_pos;
        last_rpm_ms     = now;

        /* Signed RPM = (Δpulses × 60000) / (PPR × interval_ms) */
        int32_t raw_rpm = (delta * 60000L) / ((int32_t)encoder_ppr * RPM_UPDATE_MS);
        if (raw_rpm >  (int32_t)METER_MAX_RPM) raw_rpm =  (int32_t)METER_MAX_RPM;
        if (raw_rpm < -(int32_t)METER_MAX_RPM) raw_rpm = -(int32_t)METER_MAX_RPM;

        /* Exponential smoothing (signed) */
        smooth_rpm = (smooth_rpm * 6 + raw_rpm * 4) / 10;

        /* Drive stepper — direction follows sign of smooth_rpm */
        set_stepper_rpm(smooth_rpm);

        /* Gauge and readout show absolute speed */
        uint32_t abs_rpm = (uint32_t)abs(smooth_rpm);

        /* Scale needle to 0–400 range (scale units = RPM / 10) */
        lv_meter_set_indicator_end_value(meter, needle, (int32_t)(abs_rpm / 10));

        /* Update live RPM readout */
        char buf[8];
        snprintf(buf, sizeof(buf), "%04lu", abs_rpm);
        lv_label_set_text(rpm_readout, buf);
    }

    lv_timer_handler();

    /* Service web requests */
    server.handleClient();
    dnsServer.processNextRequest();

    /* Apply any pending WiFi reconfiguration requested by the web handler */
    if (wifi_reconfig_pending) {
        wifi_reconfig_pending = false;
        apply_wifi_config();
    }

    delay(5);
}
