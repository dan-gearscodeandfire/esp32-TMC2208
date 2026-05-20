# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Embedded firmware for an ESP32 driving a TMC2208 stepper driver. **Built with ESP-IDF** (CMake-based, `main/` component). The original Arduino sketch `shopsmartfan.ino` is preserved at the repo root as a porting reference — it is **not** part of the ESP-IDF build (`idf.py` only picks up sources listed in `main/CMakeLists.txt`).

## Hardware

- **MCU:** ESP32 DevKit V1 (30-pin, ESP-WROOM-32, CP2102 USB-serial)
- **Driver:** Dorhea TMC2208 V1.2 (FYSETC v1.2 clone — 0.11Ω sense resistor)
- **Motor:** 12V 1.5A NEMA 17 stepper, 200 steps/rev
- **PSU:** Single external 12V supply. Feeds the TMC2208 `VM` directly, and an adjustable buck converter (LM2596 or similar) that steps down to ~5V for the ESP32 `Vin` pin.

### Pin mapping

| TMC2208 pin | ESP32 GPIO | Notes |
|---|---|---|
| DIR  | 32 | |
| STEP | 33 | |
| UART (via 1kΩ) | 26 (TX) | 1kΩ between GPIO 26 and the UART bus |
| UART (direct)  | 27 (RX) | bus is shared — RX taps it, TX joins through resistor |
| EN   | 13 | 10kΩ external pull-up to VIO recommended (keeps driver disabled during ESP32 boot) |
| VIO  | 3.3V from ESP32 | **NOT 5V** |
| VM   | external 12V | 100µF+ electrolytic across VM↔GND, leads short |
| CLK, PDN, MS1, MS2 | unconnected | internal oscillator, UART overrides MS pins |

### Driver left-side header (motor outputs + power)

Silkscreen on the Dorhea TMC2208 V1.2 left edge, top to bottom:

| Position | Label | Function |
|---|---|---|
| 1 | GND  | logic ground (top) |
| 2 | VIO  | 3.3V logic supply (ties to ESP32 3.3V) |
| 3 | M2B  | H-bridge M2, one end |
| 4 | M2A  | H-bridge M2, other end |
| 5 | M1A  | H-bridge M1, one end |
| 6 | M1B  | H-bridge M1, other end |
| 7 | GND  | motor-side ground |
| 8 | VM   | 12V motor supply |

`M1` and `M2` are the two H-bridge output pairs on the board. The `A`/`B` suffixes are just position labels for the two ends of each H-bridge — **they are not related to the motor's own coil-A / coil-B labels**.

### Stepper motor connector

The NEMA 17 has a 6-position connector with two unused middle positions (where center taps would sit on a unipolar variant):

| Position | Label | Meaning |
|---|---|---|
| 1 | A   | motor coil A, one end |
| 2 | —   | unused (center-tap position) |
| 3 | Ā   | motor coil A, other end |
| 4 | B   | motor coil B, one end |
| 5 | —   | unused (center-tap position) |
| 6 | B̄   | motor coil B, other end |

The bar (`Ā`, `B̄`) is sometimes written `/A`, `/B` in datasheets — same meaning: *the other end of that coil*.

### Wiring rule (motor → driver)

The motor's `A`/`B` naming and the driver's `M1`/`M2` naming are **two independent labeling systems** — don't try to read them as a 1:1 mapping. The trailing `A`/`B` on `M1A`/`M1B` is just a position identifier within H-bridge M1, unrelated to the motor's coil-A.

The only rule that matters is:

> **Both ends of one motor coil go to the same H-bridge pair.**

Concretely:
- Pick one driver H-bridge pair (either `M1A`+`M1B` or `M2A`+`M2B`); wire both ends of motor coil A (`A` and `Ā`) to it.
- Wire both ends of motor coil B (`B` and `B̄`) to the other H-bridge pair.
- Within a pair, which end goes to `<X>A` vs `<X>B` doesn't matter electrically — it just sets that coil's rotation direction. If the motor vibrates without rotating after first power-up, swap one coil's two leads (e.g., `A` ↔ `Ā`), not both.

What's **not allowed** is splitting one motor coil across two H-bridges (e.g., `A` to `M1A` and `Ā` to `M2A`) — that shorts the bridges through the wrong winding.

### UART hardware setup (one-time)

The TMC2208 board ships in standalone mode. To enable UART:
1. On the back of the driver, bridge the **middle pad** of the 3-pad jumper to the pad labeled **`UART`** (verified by continuity: that pad is the one electrically connected to the `UART` header pin, not the `PDN` header pin).
2. Verify after soldering: chip pin 4 ↔ `UART` header pin should now have continuity.

### Buck converter setup (one-time)

The buck steps 12V → ~5V to feed ESP32 `Vin`. Cheap adjustable modules (LM2596/MP1584/XL4015) regulate poorly at near-zero load — they drop into pulse-skipping mode and the output reads 0.3–1V higher than the actual loaded voltage. Adjust under load:

1. Apply 12V to the buck input. Do **not** connect the ESP32 yet.
2. Hang a **100Ω 1W resistor** across the buck output as a dummy load (50mA @ 5V, ~0.25W dissipated — barely warm). A 47Ω 1W is closer to the ESP32's actual operating draw if you want a more accurate set point.
3. Measure across the resistor with a multimeter. Turn the trimpot until you read **5.0–5.2V**.
4. Disconnect the dummy resistor. Wire the buck output to ESP32 `Vin` (and GND to GND — see common-ground rule below).
5. Re-check the voltage at `Vin` with the ESP32 running. Should be within ~0.1V of the set point.

Why **not** higher than ~5.2V on `Vin`: the DevKit V1's `AMS1117-3.3` LDO burns the excess as heat (12V on `Vin` → ~9V across the LDO at ~150mA = ~1.3W dissipated, gets uncomfortably hot). 5V minimizes LDO heat while still leaving the ~1V dropout headroom the LDO needs.

### Pre-power checklist

- Vref ≈ 1.2V at trimpot (sets current ceiling; UART scales below it).
- **Common ground:** ESP32 GND ↔ buck output GND ↔ 12V PSU GND ↔ TMC2208 GND. Non-negotiable — UART needs a shared reference, and the buck's output ground must tie to the same point as the motor PSU ground.
- **Never plug/unplug the motor with VM live** — inductive spike destroys the driver.
- Drive EN HIGH in `app_main` *before* initializing UART2 (`uart_driver_install` for the TMC2208) so the driver stays disabled during config. The firmware does this already; see `main/main.c`.

## Build / upload (ESP-IDF)

Installed via the Espressif Windows Offline Installer at `C:\Espressif` (default). **Always launch the shell from the `ESP-IDF 5.x PowerShell` Start Menu shortcut** — it sets `IDF_PATH`, prepends the toolchain to `PATH`, and activates the Python venv. From a plain PowerShell, `idf.py` will not resolve.

From the project root:

```
idf.py set-target esp32        # one-time per project (writes sdkconfig)
idf.py build
idf.py -p COM3 flash
idf.py -p COM3 monitor         # exit with Ctrl+]
idf.py -p COM3 flash monitor   # combined build/flash/monitor in one shot
```

`COM3` is the ESP32's CP2102 enumeration on this machine. Verify with `[System.IO.Ports.SerialPort]::GetPortNames()` if it changes (e.g., after a Windows update or different USB port).

**Resetting the chip from automation:** use `python -m esptool --port COM3 --before default_reset --after hard_reset chip_id` rather than toggling DTR/RTS through PowerShell `System.IO.Ports.SerialPort`. The latter can leave the serial reader bit-misaligned and produce "wrong baud" symptoms that aren't real.

## Software architecture

```
shopsmartfan/
├── CMakeLists.txt              top-level ESP-IDF project
├── sdkconfig.defaults          project-pinned config (see Memory strategy below)
├── main/
│   ├── CMakeLists.txt          requires "driver" and "tmc2208"
│   └── main.c                  app_main: GPIO init → TMC2208 UART probe → heartbeat loop with retry
├── components/
│   └── tmc2208/
│       ├── CMakeLists.txt
│       ├── tmc2208.h           public API + register address constants
│       └── tmc2208.c           CRC8, single-wire UART framing, register R/W
└── shopsmartfan.ino            legacy Arduino sketch, kept as porting reference (NOT compiled)
```

`main/main.c` orchestrates the application. `components/tmc2208/` is a self-contained driver that knows nothing about pins or the project — it takes a `tmc2208_config_t` (UART port, pins, baud, slave address) at init time. Future peripherals (e.g., MCPWM-based step generation) should go in their own `components/<name>/` for the same reason.

### TMC2208 driver implementation notes

ESP-IDF has no equivalent of the Arduino `TMCStepper` library — `components/tmc2208/` implements the protocol from scratch. Key things in there worth knowing before editing:

- **Single-wire UART echoes:** TX and RX share one bus wire on the chip side. Every byte we transmit is read back on RX before any reply. The driver consumes those echo bytes (4 for a read request, 8 for a write) before reading the chip's reply. Don't skip the echo-consume or replies will look corrupted.
- **CRC8:** polynomial `0x07`, init 0, LSB-first bit shift. Don't substitute a generic CRC8 — the bit order matters.
- **Slave address:** `0x00` on this hardware (MS1 and MS2 unconnected). If the user ever straps those pins, the address changes — see the IOIN bits and `tmc2208_config_t.slave_addr`.
- **Writes have no reply** but DO increment `IFCNT` on success — `tmc2208_get_ifcnt()` is exposed for write verification.

`shopsmartfan.ino` (Arduino reference) shows the high-level config sequence we still need to port for Phase 3: set `EN` HIGH → init UART → `toff(5)` → `rms_current(800)` → `microsteps(16)` → `intpol(true)` → `pwm_autoscale(true)` → confirm `VERSION` → set `EN` LOW.

## Memory strategy

ESP32 has 320 KB of on-chip SRAM split between **IRAM** (instruction RAM for hot-path code, with cache, ~70 KB usable after IDF reserves) and **DRAM** (data RAM for runtime state, ~200 KB usable after BSS/heap setup). Our budget:

| Bucket | Usage | Notes |
|---|---|---|
| ESP-IDF system | ~80 KB DRAM, ~50 KB IRAM | bootloader, FreeRTOS, drivers — non-negotiable |
| Main task stack | 4 KB DRAM | bumped from 3584 default; ESP_LOGI + UART init want margin |
| TMC2208 driver state | <100 B | static struct, no heap |
| UART2 driver buffers | 256 B RX ring + 0 B TX ring | TX writes are blocking, no ring buffer needed |
| TMC2208 frame buffers | 8 B each, on stack | tiny |
| Step generation (future) | TBD | MCPWM is hardware — minimal RAM; mark any ISR with `IRAM_ATTR` |
| Heap (free, after init) | ~290 KB | mostly DRAM; reserved for FreeRTOS internals and future tasks |

Configured via `sdkconfig.defaults`:
- `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` + `CONFIG_ESPTOOLPY_FLASHSIZE="4MB"` — matches the Boya chip on this board; without this, boot logs a flash-size mismatch warning.
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE=4096` — margin for ESP_LOG + UART transactions.
- `CONFIG_FREERTOS_HZ=1000` — 1 ms tick for finer motor-orchestration timing. MCPWM does step generation in hardware so this doesn't gate step accuracy, but `vTaskDelay`-based ramps and command sequencing benefit.
- `CONFIG_LOG_DEFAULT_LEVEL_INFO=y` — development default. Drop to WARN/ERROR for release.

To re-tune, edit `sdkconfig.defaults`, then `rm sdkconfig && idf.py reconfigure` (or `set-target esp32`) — `sdkconfig.defaults` only seeds initial generation, it doesn't override an existing `sdkconfig`.

## Smoke test status

### Phase 1 — Toolchain & USB serial: **PASS** (2026-05-19)

Verified end-to-end with USB only (no 12V, TMC2208 VIO unwired):
- ESP-IDF v5.5.4 build succeeds (binary ~184 KB, 82% of factory partition free).
- `idf.py -p COM3 flash` flashes bootloader + partition table + app cleanly.
- Boot sequence at 115200 baud: ROM → 2nd-stage bootloader → `app_main`.
- `SPI Flash Size: 4MB` in boot log (sdkconfig.defaults applied).
- Onboard blue LED (GPIO 2) blinks at 1 Hz; `ESP_LOGI` heartbeats print every 1000 ms (`heartbeat #1`, `#2`, ...).

If you ever re-run and get the boot banner but no `smoke:` lines, the flash worked but `app_main` isn't running — check `idf.py build` output. If `idf.py flash` fails to connect, hold `BOOT` on the DevKit while flash starts, or try a lower `--baud 115200`.

### Phase 2 — TMC2208 UART: **PASS** (2026-05-19)

`VERSION=0x20` read cleanly at slave 0x00, 19200 baud. Reads (IOIN, CHOPCONF) and writes (IFCNT increments, CHOPCONF readback exact-matches) both confirmed.

> **CRITICAL GOTCHA — the UART needs VM (12V motor supply) connected, not just VIO.** The chip's digital core (including the UART state machine) is powered from an internal regulator fed by VM. VIO alone only powers the I/O level-translation buffers. With VIO-only, the ESP32 sees its own TX **echo cleanly** (looks like the bus works) but the chip never replies — every read times out `reply read got 0/8`. **If the UART is silent but echoes are clean and wiring checks out, verify VM before suspecting anything else.** This cost a multi-hour debug session and an unnecessary chip swap.

**Read reliability is marginal at the current wiring** (~70-80% at 19200 baud — some reads time out, retries succeed). Writes are unaffected (chip drives nothing during a write). To make reads rock-solid, swap the **1 kΩ TX series resistor for 4.7 kΩ** (lets the chip's LOW drive win the voltage divider against GPIO 26's idle-HIGH push-pull). Not yet done.

If the UART ever goes silent again, check in order:
1. **VM (12V) present** — the #1 cause (see gotcha above).
2. Driver `VIO` tied to ESP32 `3.3V` (NOT 5V).
3. UART solder jumper on driver back bridged to `UART` pad (not `PDN`).
4. 1 kΩ (or 4.7 kΩ) in series between GPIO 26 (TX) and the shared UART bus.
5. RX/TX not swapped (ESP32 RX=27, TX=26); common ground (ESP32 GND ↔ driver GND).

### Phase 3 — Motor spin: **PARTIALLY VALIDATED** (2026-05-19)

`run_sanity_test()` in `main/main.c` drove the motor 1 rev forward + 1 rev back at ~275 mA RMS (IRUN=8, vsense=1), 16 microsteps, via busy-wait STEP pulses. `DRV_STATUS` showed no faults: no overtemp, no shorts, **no open-load (`ola=0 olb=0` → both coils connected and conducting)**, `cs_actual=8`.

**Still TODO:**
- Replace the busy-wait `pulse_steps()` (sanity-test scaffold, blocks the task) with **MCPWM hardware step generation** (`driver/mcpwm_prelude.h`). Validate smooth motion + correct speed with eyes on the motor.
- Tune current to the real target; pick microstep resolution for the use case.
- Wrap motor control in a task with a clean API (set_speed / set_direction / run / stop).
- **Remove the `run_sanity_test()` call from `app_main` once real motor code lands** — it currently re-runs the 1-rev-each-way move on every boot/reset.

If motor vibrates but doesn't rotate: one coil is reversed — swap the two leads of either coil A or coil B (not both).

## Things to be careful about when editing

- **EN pin polarity:** active LOW. Driving `EN` HIGH disables the driver. The `EN` GPIO must be configured as OUTPUT and driven HIGH **before** initializing UART2 to the TMC2208 — the driver has to be disabled while you write its config registers. In ESP-IDF: `gpio_set_direction(EN_PIN, GPIO_MODE_OUTPUT)` and `gpio_set_level(EN_PIN, 1)` before `uart_driver_install()` on UART2.
- **Motor current safety limit:** keep the configured RMS current under ~1A without active cooling; the chip gets hot at 1.5A. In the Arduino reference the setting is `rms_current(800)` (mA). At the register level (what ESP-IDF code will write), this maps to the `IRUN`/`IHOLD` fields of the `IHOLD_IRUN` register, scaled against the trimpot Vref ceiling.
- **GPIO 12 is a strapping pin** (flash voltage select, must be LOW at boot). It's unused in the current pin map — keep it that way. Anything else in the 32/33/25/26/27/13 range is safe.
- **UART pins (26/27) are remappable** — ESP32 UART peripherals are software-mapped to any GPIO via the GPIO matrix. In ESP-IDF: `uart_set_pin(UART_NUM_2, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)`. The default UART2 pins (17/16) are *not* used here — don't accidentally pass `UART_PIN_NO_CHANGE` for TX/RX.
