# Machine-Tachometer-Hobbing

A Raspberry Pi Pico 2W firmware that reads a quadrature encoder, displays live RPM on a GC9A01 round TFT display using LVGL 8, drives a stepper motor via a TMC5160 at a configurable ratio of the measured RPM, and controls a Yaskawa A1000 VFD over MEMOBUS/Modbus RTU. Designed for hobbing machine synchronisation — the stepper speed tracks the spindle speed according to a user-defined hob thread / gear tooth count.

## Features

- Live RPM tachometer gauge on a 240×240 GC9A01 round display (LVGL 8, needle + digital readout)
- Startup sweep animation
- Exponentially smoothed RPM reading from a quadrature encoder (direction-aware)
- TMC5160 stepper driver in StealthChop velocity mode — speed tracks encoder RPM via hob/gear ratio, corrected for belt/pulley gearing
- Stepper driver abstraction — swap to a different driver by replacing two functions only
- Yaskawa A1000 VFD control over MEMOBUS/Modbus RTU (RS-485): run forward, run reverse, stop, set speed, fault reset
- Physical VFD controls: Start/Stop toggle button, Reverse button, potentiometer for continuous speed adjustment
- VFD fault monitoring — on-screen overlay on any new trip, and on fault clearance
- WiFi web UI at **`http://gizmo.mt`** (AP mode) to adjust all parameters at runtime
- AP mode SSID `MTGizmo` — no router needed; DNS resolves `gizmo.mt`
- Station mode support with automatic fallback to AP if connection fails
- Physical button (GPIO 14): short press shows current IP on display for 5 s, long press (2 s) forces AP mode
- On-screen overlay notifications for all WiFi mode transitions and VFD faults
- All runtime settings persist across reboots via EEPROM
- Onboard LED blinks at 1 Hz while the VFD is running, 0.25 Hz while stopped

## Hardware

- Raspberry Pi Pico 2W (RP2350)
- GC9A01 240×240 round TFT display
- Rotary encoder (quadrature, channel A interrupt + channel B for direction)
- BigTreeTech TMC5160 v1.2 stepper driver
- 2-phase stepper motor
- External motor PSU (8–40 V)
- SparkFun RS-485 Breakout (or any MAX485-compatible half-duplex transceiver)
- Yaskawa A1000 VFD
- 3× momentary push buttons (GPIO 14, 4, 5)
- 10 kΩ potentiometer (GPIO 26 / ADC0)

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

### SparkFun RS-485 Breakout → Pico 2W (UART1)

| RS-485 Breakout Pin | Pico 2W GPIO | Pico 2W Pin |
|---|---|---|
| TX-O | GPIO 9 (UART1 RX) | Pin 12 |
| RX-I | GPIO 8 (UART1 TX) | Pin 11 |
| DE | GPIO 7 | Pin 10 |
| VCC | 3.3 V | Pin 36 |
| GND | GND | Any GND |

> DE is driven HIGH to transmit and LOW to receive. The firmware handles this automatically around every Modbus transaction.

### RS-485 → Yaskawa A1000 VFD

| RS-485 Breakout Pin | A1000 Terminal |
|---|---|
| A (+) | S+ (R+) |
| B (−) | S− (R−) |

> Terminate with a 120 Ω resistor across A/B at the far end of the cable if the bus is long or noisy.

### Buttons → Pico 2W

All three buttons wire the same way: one leg to the GPIO, other leg to GND. All use the internal pullup — no external resistors needed.

| Button | Pico 2W GPIO | Pico 2W Pin | Function |
|---|---|---|---|
| WiFi / IP | GPIO 14 | Pin 19 | Short press: show IP on display 5 s · Long press: force AP mode |
| VFD Start/Stop | GPIO 4 | Pin 6 | Press toggles Run ↔ Stop based on current drive state |
| VFD Reverse | GPIO 5 | Pin 7 | Press sends Reverse command |

### Potentiometer → Pico 2W

| Pot terminal | Connection | Pico 2W Pin |
|---|---|---|
| Left (GND end) | GND | Any GND |
| Wiper (centre) | GPIO 26 (ADC0) | Pin 31 |
| Right (3.3 V end) | 3.3 V | Pin 36 |

> A 10 kΩ linear pot is recommended. The wiper voltage is read by the 12-bit ADC and mapped linearly to 0 – max frequency. A deadband of ±8 ADC counts prevents Modbus chatter while the pot is stationary. The pot and the web UI RPM input share the same frequency register; whichever was used last takes effect.

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
| VFD slave address | `1` | A1000 Modbus slave address (matches H5-01) |
| VFD max frequency | `60.00 Hz` | Upper clamp on any speed setpoint sent to the drive |
| VFD baseline frequency | `60.00 Hz` | Drive output frequency that corresponds to baseline RPM |
| VFD baseline RPM | `1750` | Motor shaft RPM at the baseline frequency |
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

## VFD Control (Yaskawa A1000)

The firmware acts as a Modbus RTU master on UART1 at 9600 bps, 8-N-2. The A1000 is polled every 500 ms to read the status word (register `0x0020`) and fault code (`0x0021`).

### A1000 setup

Set these parameters on the A1000 before connecting:

| Parameter | Value | Description |
|---|---|---|
| H5-01 | 1 | Slave address — must match web UI VFD Settings |
| H5-02 | 3 | Baud rate = 9600 bps |
| H5-03 | 0 | Data format = 8-N-2 |
| b1-01 | 2 | Frequency reference source = Modbus |
| b1-02 | 2 | Run command source = Modbus |

### Fault notifications

When a new fault is detected the fault name is shown on the display overlay for 6 s. When the fault clears, "VFD Fault Cleared" is shown for 3 s. Common faults decoded on-device:

| Code | Name |
|---|---|
| 0x01 | oC — Overcurrent |
| 0x02 | ov — Overvoltage |
| 0x03 | oH1 — Heatsink overheat |
| 0x05 | oL1 — Motor overload |
| 0x06 | oL2 — Drive overload |
| 0x0B | EF — External fault |
| 0x0C | EF0 — Modbus-triggered fault |
| 0x11 | LF — Output phase loss |
| 0x17 | CE — Modbus communication error |
| 0x1F | UV1 — DC bus undervoltage |
| 0x23 | CPF — Control circuit fault |

See [`src/a1000_modbus.json`](src/a1000_modbus.json) for the complete register map and fault code list.

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
- **VFD — Yaskawa A1000** card — shows live VFD status (Running / Stopped / No comms) and current output speed in RPM. Controls: RPM input + **Set RPM**, **Run ▶**, **◀ Reverse**, **Stop ■**, **Reset** (fault reset). The RPM is converted to a frequency command using the baseline scaling before sending via Modbus. Status auto-refreshes every 2 s.

### Settings tab

- **Encoder** card — toggle to reverse the encoder direction without rewiring.
- **Encoder PPR** card — set pulses per revolution (1–10 000). Presets: 100, 200, 400, 600, 1000, 2400.
- **Stepper Pulley** card — set driver and driven pulley tooth counts. Set both to `1` for direct drive.
- **VFD Settings** card — set the Modbus slave address (1–31), maximum frequency clamp (Hz), baseline frequency (Hz), and RPM at baseline frequency. All persist to EEPROM.
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
| `/vfd-rpm` | POST | `rpm` | Set VFD speed in RPM (converted to Hz by firmware) |
| `/vfd-run` | POST | — | Send run (forward) command to VFD |
| `/vfd-stop` | POST | — | Send stop command to VFD |
| `/vfd-freq` | POST | `hz` (0.01 Hz units) | Set VFD frequency reference directly in 0.01 Hz units |
| `/vfd-reset` | POST | — | Reset active VFD fault |
| `/vfd-settings` | POST | `slave`, `maxhz`, `basehz`, `baserpm` | Set slave address, max/baseline frequency, baseline RPM |
| `/vfd-status` | GET | — | Returns JSON: `{comms_ok, running, status, fault, freq, base_hz, base_rpm}` |

## Physical Controls

### WiFi / IP button (GPIO 14)

| Press | Action |
|---|---|
| Short press (< 2 s) | Shows the current IP address on the display for 5 s |
| Long press (≥ 2 s) | Forces a switch to AP mode (`MTGizmo` / `192.168.4.1`) |

### VFD Start/Stop button (GPIO 4)

Toggles the drive on each press. If the drive is currently running it sends Stop; if stopped it sends Run forward. Edge-triggered — holding the button does not repeat.

### VFD Reverse button (GPIO 5)

Sends a single Reverse command (command word `0x0002`) on each press. Edge-triggered.

### VFD speed potentiometer (GPIO 26 / ADC0)

Sampled every 50 ms. Maps the 12-bit ADC reading (0–4095) linearly across 0 – `vfd_max_hz`. Only sends a new Modbus write when the reading changes by more than 8 ADC counts.

## Stepper Driver Abstraction

The TMC5160-specific code is isolated in two functions inside a clearly marked section of [`src/main.cpp`](src/main.cpp):

- **`stepper_init()`** — hardware setup, SPI configuration, StealthChop enable
- **`stepper_set_rpm(float rpm)`** — translates a signed RPM value into VMAX register writes

To substitute a different driver (e.g. a step/dir driver like a DRV8825), replace only those two functions plus the `#include`, pin defines, and driver object. Everything else — ratio calculation, web UI, EEPROM, display — is untouched.

## EEPROM Layout

Settings are stored in 148 bytes of emulated EEPROM:

| Bytes | Content |
|---|---|
| 0 | Magic byte (`0xB0`) — detects valid data |
| 1 | WiFi mode (0 = AP, 1 = STA) |
| 2–65 | Station SSID (null-terminated, max 63 chars) |
| 66–129 | Station password (null-terminated, max 63 chars) |
| 130–131 | `hob_threads` (uint16_t, little-endian) |
| 132–133 | `gear_teeth` (uint16_t, little-endian) |
| 134 | Encoder reversed (0 or 1) |
| 135–136 | Encoder PPR (uint16_t, little-endian) |
| 137–138 | `pulley_driver` (uint16_t, little-endian) |
| 139–140 | `pulley_driven` (uint16_t, little-endian) |
| 141 | VFD slave address (uint8_t) |
| 142–143 | VFD max frequency (uint16_t, 0.01 Hz units, little-endian) |
| 144–145 | VFD baseline frequency (uint16_t, 0.01 Hz units, little-endian) |
| 146–147 | VFD baseline RPM (uint16_t, little-endian) |

> The magic byte is checked on every boot. If it does not match `0xB0`, all settings revert to firmware defaults and EEPROM is re-initialised on next save.

## A1000 Modbus RTU Simulator (`pico_simulator`)

[`src/simulator.cpp`](src/simulator.cpp) implements a second firmware target that turns a standard Raspberry Pi Pico (RP2040) into a Yaskawa A1000 stand-in on the RS-485 bus. It is useful for bench-testing the main firmware without a real VFD.

### What it simulates

- Modbus RTU slave at address `1`, 9600 bps, 8-N-2
- **FC 03** Read Holding Registers
- **FC 06** Write Single Register
- **FC 08** Loopback diagnostic (sub-function 0x0000 only)

| Register | Access | Description |
|---|---|---|
| `0x0001` | R/W | Command word — bit 0 = run forward, bit 1 = run reverse, bit 3 = fault reset |
| `0x0002` | R/W | Frequency reference (0.01 Hz units) |
| `0x0020` | R | Status word — mirrors running / reverse / fault / ready bits |
| `0x0021` | R | Fault code (0 = no fault) |
| `0x0025` | R | Output frequency — ramps toward frequency reference when running |

All other registers within the normal A1000 address range return `0x0000` on read; writes to unknown registers return Modbus exception 02 (Illegal Data Address).

The simulated drive ramps its output frequency at ~0.5 Hz/s toward the commanded reference, giving a realistic response to acceleration/deceleration commands.

The onboard LED blinks at **1 Hz** while the simulated drive is running, and **0.25 Hz** while stopped. Both firmware targets use the same blink rates so the behaviour is consistent across the bench setup.

### Simulator wiring (MAX485 breakout → standard Pico)

Same pinout as the main controller — connect both boards to the same A/B bus pair:

| Pico GPIO | Pico Pin | MAX485 Pin | Direction |
|---|---|---|---|
| GPIO 8 (Serial2 TX) | Pin 11 | DI | Pico → MAX485 |
| GPIO 9 (Serial2 RX) | Pin 12 | RO | MAX485 → Pico |
| GPIO 7 | Pin 10 | DE + RE (tied) | Transmit enable |
| 3.3 V | Pin 36 | VCC | — |
| GND | Any GND | GND | — |

### Building and flashing the simulator

```
pio run -e pico_simulator
pio run -e pico_simulator --target upload
```

`pio run` (without `-e`) only builds the `pico2w` main firmware (`default_envs = pico2w`).

## Serial Debug Monitoring

Both firmware targets print VFD activity to USB Serial at **115200 baud**. Connect via USB and open a serial monitor to observe all Modbus transactions in real time.

```
pio device monitor -b 115200
```

### Main firmware (`pico2w`) log messages

| Prefix | When printed |
|---|---|
| `[VFD] init …` | Once at boot, after `vfd_init()` configures UART1 |
| `[VFD TX] 01 06 …` | Every frame sent to the drive (raw hex bytes) |
| `[VFD RX] 01 06 …` | Every frame received from the drive (raw hex bytes) |
| `[VFD RX] timeout — no bytes` | Response window elapsed with no data |
| `[VFD] CMD run-forward` | `vfd_run()` called (button, web UI, or API) |
| `[VFD] CMD run-reverse` | `vfd_reverse()` called |
| `[VFD] CMD stop` | `vfd_stop()` called |
| `[VFD] CMD fault-reset` | `vfd_reset_fault()` called |
| `[VFD] CMD set-freq 6000 (60.00 Hz)` | `vfd_set_freq()` called (potentiometer or web UI) |
| `[VFD] FC06 reg=0x… val=0x…  OK` | Write single register succeeded |
| `[VFD] FC06 reg=0x… val=0x…  FAIL (…)` | Write failed — reason in parentheses |
| `[VFD] FC03 reg=0x… cnt=2  OK  [0]=0x…  [1]=0x…` | Read holding registers succeeded, values shown |
| `[VFD] FC03 reg=0x… cnt=2  FAIL (…)` | Read failed — reason in parentheses |
| `[VFD] poll  status=0x…  fault=0x… (…)  running=…` | Printed whenever the polled status or fault code changes |
| `[VFD] poll — comms lost` | First poll failure after a previously successful comms state |

### Simulator (`pico_simulator`) log messages

| Prefix | When printed |
|---|---|
| `[SIM] A1000 simulator ready …` | Once at boot |
| `[SIM RX] 01 03 …` | Every frame received from the master (raw hex bytes) |
| `[SIM] bad CRC — discarded` | Frame received with invalid CRC |
| `[SIM] FC=0x03 slave=1` | Decoded function code and slave address |
| `[SIM] FC03 read reg=0x… cnt=…` | FC 03 read request decoded |
| `[SIM] FC06 write reg=0x… val=0x…` | FC 06 write request decoded |
| `[SIM] FC06 reg=0x… — illegal address, sending exception` | Write to unimplemented register |
| `[SIM TX] 01 03 …` | Every response frame sent back to the master (raw hex bytes) |

> Only changes to the VFD status word or fault code are logged during polling — steady-state polls that return the same values produce no output, keeping the monitor readable.

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

- [`src/a1000_modbus.json`](src/a1000_modbus.json) — complete Yaskawa A1000 MEMOBUS/Modbus register map: command/status bit definitions, all monitor registers, parameter addresses, fault codes, example raw byte sequences, and scaling reference.
- [`pico-arduino/`](pico-arduino/) — original simpler tachometer prototype (Pico, not 2W; encoder-only; no stepper, WiFi, VFD, or EEPROM). Kept as a reference.
