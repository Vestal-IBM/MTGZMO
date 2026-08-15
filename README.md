# Machine-Tachometer-Hobbing

A Raspberry Pi Pico 2W firmware that reads a quadrature encoder, displays live RPM on a GC9A01 round TFT display using LVGL 8, and drives a stepper motor via a TMC5160 at a configurable ratio of the measured RPM. Designed for hobbing machine synchronisation — the stepper speed tracks the spindle speed according to a user-defined hob thread / gear tooth count.

## Features

- Live RPM tachometer gauge on a 240×240 GC9A01 round display (LVGL 8, needle + digital readout)
- Startup sweep animation
- Exponentially smoothed RPM reading from a quadrature encoder (direction-aware)
- TMC5160 stepper driver in StealthChop velocity mode — speed tracks encoder RPM via hob/gear ratio, corrected for belt/pulley gearing
- Stepper driver abstraction — swap to a different driver by replacing two functions only
- WiFi web UI at **`http://gizmo.mt`** (AP mode) to adjust all parameters at runtime
- AP mode SSID `MTGizmo` — no router needed; DNS resolves `gizmo.mt`
- Station mode support with automatic fallback to AP if connection fails
- Physical button (GPIO 14): short press shows current IP on display for 5 s, long press (2 s) forces AP mode
- On-screen overlay notifications for all WiFi mode transitions
- All runtime settings persist across reboots via EEPROM

## Hardware

- Raspberry Pi Pico 2W (RP2350)
- GC9A01 240×240 round TFT display
- Rotary encoder (quadrature, channel A interrupt + channel B for direction)
- BigTreeTech TMC5160 v1.2 stepper driver
- 2-phase stepper motor
- External motor PSU (8–40 V)
- Momentary push button (GPIO 14 to GND)

## Wiring

### GC9A01 Display → Pico 2W (SPI0)

| GC9A01 Pin | Pico 2W GPIO | Pico 2W Pin |
|---|---|---|
| SCLK | GPIO 18 | Pin 24 |
| MOSI | GPIO 19 | Pin 25 |
| CS | GPIO 17 | Pin 22 |
| DC | GPIO 20 | Pin 26 |
| RST | — | Not connected |
| BL | 3.3 V | Pin 36 (or PWM GPIO for dimming) |
| VCC | 3.3 V | Pin 36 |
| GND | GND | Any GND |

### Rotary Encoder → Pico 2W

| Encoder Pin | Pico 2W GPIO | Pico 2W Pin |
|---|---|---|
| Channel A | GPIO 2 | Pin 4 |
| Channel B | GPIO 3 | Pin 5 |
| VCC | 3.3 V | Pin 36 |
| GND | GND | Any GND |

> Channel B is sampled inside the channel-A ISR for direction detection. Both channels are pulled up internally. Direction can be software-reversed in the web UI without rewiring.

### BTT TMC5160 v1.2 → Pico 2W (SPI1)

| Pico 2W GPIO | Pico 2W Pin | BTT TMC5160 v1.2 Pin |
|---|---|---|
| GPIO 10 | Pin 14 | SCK |
| GPIO 11 | Pin 15 | SDI |
| GPIO 12 | Pin 16 | SDO |
| GPIO 13 | Pin 17 | CS |
| GPIO 15 | Pin 20 | EN |
| 3.3 V | Pin 36 | VIO |
| GND | Any GND | GND |

> **VM** on the TMC5160 connects to your external motor PSU (8–40 V), **not** the Pico.  
> GND must be common between the Pico and the motor PSU.  
> EN is driven HIGH (disabled) during startup and pulled LOW (enabled) once the driver is fully configured.

### Motor → BTT TMC5160 v1.2

Connect your 2-phase stepper motor coils to the **A1/A2** and **B1/B2** terminals on the TMC5160 board.

### Button → Pico 2W

| Button Pin | Pico 2W GPIO | Pico 2W Pin |
|---|---|---|
| One leg | GPIO 14 | Pin 19 |
| Other leg | GND | Any GND |

> GPIO 14 is pulled up internally. Pressing the button pulls it LOW.

## Configuration

Compile-time constants at the top of [`src/main.cpp`](src/main.cpp):

| Define | Default | Description |
|---|---|---|
| `MOTOR_STEPS` | `200` | Full steps per revolution of your stepper |
| `MICROSTEPS` | `256` | TMC5160 microstep resolution |
| `TMC_RMS_CURRENT` | `1000` | Motor RMS current limit in mA |
| `TMC_R_SENSE` | `0.075` | Sense resistor in ohms (0.075 Ω for BTT TMC5160 v1.2) |
| `METER_MAX_RPM` | `4000` | Maximum RPM shown on the gauge |
| `RPM_UPDATE_MS` | `50` | RPM recalculation interval in milliseconds |

Runtime-adjustable settings (survive reboot via EEPROM):

| Setting | Default | Description |
|---|---|---|
| Hob threads | `1` | Number of starts on the hob cutter |
| Gear teeth | `32` | Number of teeth on the gear being cut |
| Encoder PPR | `600` | Encoder pulses per revolution (single channel) |
| Encoder reversed | `false` | Flip encoder direction without rewiring |
| Stepper driver pulley | `1 : 1` | Driver and driven pulley tooth counts for belt correction |
| WiFi mode | AP | AP or Station |
| Station SSID / password | — | Router credentials for Station mode |

## Hobbing Ratio & Pulley Gearing

### Hobbing ratio

The required output shaft speed (gear blank) relative to the encoder (spindle/hob) is:

```
output_rpm = encoder_rpm × (hob_threads / gear_teeth)
```

**`hob_threads`** is the number of starts (threads) on the hob cutter.  
**`gear_teeth`** is the number of teeth on the gear being cut.

For every full rotation of the output shaft (gear blank), the encoder (spindle/hob) must complete `gear_teeth / hob_threads` rotations.

### Stepper pulley / belt correction

If the stepper drives the output shaft through a belt-and-pulley reduction, the stepper must spin faster or slower than the required output RPM. The firmware applies the belt ratio automatically so the **final output always matches the hobbing ratio**, regardless of pulley size:

```
stepper_rpm = output_rpm × (pulley_driven / pulley_driver)
```

Combined into a single formula:

```
stepper_rpm = encoder_rpm × (hob_threads / gear_teeth) × (pulley_driven / pulley_driver)
```

**`pulley_driver`** is the tooth count on the pulley attached to the stepper shaft.  
**`pulley_driven`** is the tooth count on the pulley attached to the output shaft.

Set both to `1` (default) when the stepper drives the output directly with no belt.

### Examples

| Hob threads | Gear teeth | Driver pulley | Driven pulley | Behaviour |
|---|---|---|---|---|
| 1 | 32 | 1 | 1 | Direct drive — spindle turns 32× per 1 blank rotation |
| 1 | 40 | 1 | 1 | Direct drive — spindle turns 40× per 1 blank rotation |
| 2 | 40 | 1 | 1 | 2-start hob — spindle turns 20× per blank rotation |
| 1 | 32 | 20 | 40 | Belt 1:2 reduction — stepper spins at 2× output RPM |
| 1 | 32 | 40 | 20 | Belt 2:1 step-up — stepper spins at 0.5× output RPM |

## Web UI

Connect to the `MTGizmo` WiFi network (password: `hobbing1`) and open **`http://gizmo.mt`** in any browser.

| Setting | Value |
|---|---|
| SSID | `MTGizmo` |
| Password | `hobbing1` |
| IP | `192.168.4.1` |
| Hostname (AP mode) | `http://gizmo.mt` |

The UI has two tabs:

### Home tab

- **Gear Ratio** card — enter *Threads on hob* and *Gear teeth*; the display shows the current ratio as `H : T`. Takes effect immediately and persists to EEPROM.

### Settings tab

- **Encoder** card — toggle to reverse the encoder direction without rewiring.
- **Encoder PPR** card — set pulses per revolution (1–10 000). Presets: 100, 200, 400, 600, 1000, 2400.
- **Stepper Pulley** card — set driver and driven pulley tooth counts. Set both to `1` for direct drive.
- **WiFi** card — switch between AP and Station mode. In Station mode enter your router SSID and password; if connection fails within 20 s the device falls back to AP mode automatically. The Station credentials are preserved even after a fallback.

> `http://gizmo.mt` resolves only while connected to the Pico's own AP. In Station mode use the IP address shown in the WiFi card, or press the physical button to display it on screen.

### Web API endpoints

| Endpoint | Method | Parameters | Description |
|---|---|---|---|
| `/` | GET | — | Serve the web UI |
| `/set` | POST | `hob`, `teeth` | Set hobbing ratio |
| `/set-encoder` | POST | `reversed` (0/1) | Set encoder direction |
| `/set-ppr` | POST | `ppr` | Set encoder PPR |
| `/set-pulley` | POST | `driver`, `driven` | Set pulley tooth counts |
| `/set-wifi` | GET | `mode`, `ssid`, `pass` | Configure WiFi |

## Physical Button (GPIO 14)

| Press | Action |
|---|---|
| Short press (< 2 s) | Shows the current IP address on the display for 5 s |
| Long press (≥ 2 s) | Forces a switch to AP mode (`MTGizmo` / `192.168.4.1`) |

## Stepper Driver Abstraction

The TMC5160-specific code is isolated in two functions inside a clearly marked section of [`src/main.cpp`](src/main.cpp):

- **`stepper_init()`** — hardware setup, SPI configuration, StealthChop enable
- **`stepper_set_rpm(float rpm)`** — translates a signed RPM value into VMAX register writes

To substitute a different driver (e.g. a step/dir driver like a DRV8825), replace only those two functions plus the `#include`, pin defines, and driver object. Everything else — ratio calculation, web UI, EEPROM, display — is untouched.

## EEPROM Layout

Settings are stored in 141 bytes of emulated EEPROM:

| Bytes | Content |
|---|---|
| 0 | Magic byte (`0xAE`) — detects valid data |
| 1 | WiFi mode (0 = AP, 1 = STA) |
| 2–65 | Station SSID (null-terminated, max 63 chars) |
| 66–129 | Station password (null-terminated, max 63 chars) |
| 130–131 | `hob_threads` (uint16_t, little-endian) |
| 132–133 | `gear_teeth` (uint16_t, little-endian) |
| 134 | Encoder reversed (0 or 1) |
| 135–136 | Encoder PPR (uint16_t, little-endian) |
| 137–138 | `pulley_driver` (uint16_t, little-endian) |
| 139–140 | `pulley_driven` (uint16_t, little-endian) |

> The magic byte is checked on every boot. If it does not match `0xAE`, all settings revert to firmware defaults and EEPROM is re-initialised on next save.

## Building

Built with [PlatformIO](https://platformio.org/). Open the project folder and run:

```
pio run
```

To build and upload:

```
pio run --target upload
```

### Dependencies (resolved automatically by PlatformIO)

| Library | Version |
|---|---|
| `lvgl/lvgl` | ~8.3.11 |
| `Bodmer/TFT_eSPI` | ^2.5.43 |
| `teemuatlut/TMCStepper` | ^0.7.3 |
| `DNSServer` | (bundled with arduino-pico) |
| `WebServer` | (bundled with arduino-pico) |
| `WiFi` | (bundled with arduino-pico) |
| `EEPROM` | (bundled with arduino-pico) |

Platform: `https://github.com/maxgerhardt/platform-raspberrypi.git`  
Board: `rpipico2w`

## Reference Files

- [`pico-arduino/`](pico-arduino/) — original simpler tachometer prototype (Pico, not 2W; encoder-only; no stepper, WiFi, or EEPROM). Kept as a reference.
