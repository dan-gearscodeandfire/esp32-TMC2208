# shopsmartfan — ESP32 + TMC2208 stepper control

ESP-IDF firmware that drives a **TMC2208** stepper driver from an **ESP32**, with a
**web GUI** for configuring and commanding moves. The mechanical target is the
**lid actuator of an RV-style roof vent** mounted on a shop roof. It doubles as a
hands-on **ESP32 / single-wire UART / stepper** learning project, so the code
favors clarity over cleverness.

The motor talks over **two independent channels** — STEP/DIR/EN pulses for motion,
and single-wire UART for configuration/telemetry. See
[`TMC2208_ESP32_UART_explainer.MD`](TMC2208_ESP32_UART_explainer.MD) for the full
explanation.

---

## Hardware

| Part | Detail |
|---|---|
| MCU | ESP32 DevKit V1 (ESP-WROOM-32, CP2102 USB-serial) |
| Driver | Dorhea TMC2208 V1.2 (FYSETC v1.2 clone, 0.11 Ω sense resistor) |
| Motor | NEMA 17, 12 V, 1.5 A, 200 steps/rev, 2.3 Ω/phase, ~40 N·cm holding (est.), 39 mm body |
| Power | 12 V PSU → TMC2208 `VM`, and a buck converter → 5 V → ESP32 `Vin` |

### Pin map

| TMC2208 | ESP32 GPIO | Notes |
|---|---|---|
| DIR | 32 | |
| STEP | 33 | |
| UART | 26 (TX) | **1 kΩ in series** to the single-wire UART bus |
| UART | 27 (RX) | direct tap on the same bus |
| EN | 13 | active LOW (HIGH = driver disabled) |
| VIO | 3.3 V | **NOT 5 V** |
| VM | 12 V | **required for UART to work — see Lessons Learned** |

Full hardware/wiring notes (motor-connector pinout, back-side UART jumper, buck
setup, pre-power checklist) are in [`CLAUDE.md`](CLAUDE.md).

---

## Build & flash (ESP-IDF)

Requires **ESP-IDF v5.5.x**. Launch the *ESP-IDF PowerShell* shortcut so `idf.py`
is on PATH, then from the project root:

```
idf.py set-target esp32      # once
idf.py build
idf.py -p COM3 flash         # COM3 = the board's CP2102 port
idf.py -p COM3 monitor       # Ctrl+] to exit
```

`sdkconfig` is generated from the committed `sdkconfig.defaults` (4 MB flash,
4 KB main stack, 1 kHz tick) — it is gitignored, not committed.

---

## WiFi & web GUI

1. Copy the template and fill in your network:
   ```
   cp main/wifi_secrets.example.h main/wifi_secrets.h
   ```
   Edit `WIFI_SSID` / `WIFI_PASS`. **`wifi_secrets.h` is gitignored** — credentials
   never get committed.
2. Flash. On boot the ESP32 joins your WLAN (STA mode) and prints its IP:
   ```
   web: connected. Open  http://192.168.1.202  in a browser.
   ```
3. Open that IP. The page has:
   - **Drive parameters** — microsteps, run current (mA), max-torque toggle,
     speed (rev/s ↔ step-rate, linked), acceleration, chopper mode. Derived fields
     grey out and recompute live; hover any field for its formula.
   - **Action** — acceleration mode (none / default / custom), relative or
     to-position moves, set-zero, and a live profile preview (pulses / ramp / peak
     speed / time). "Apply" pushes config to the chip over UART; "Execute" moves.

API (query-param style): `GET /api/status`, `POST /api/config`, `POST /api/move?revs=`,
`POST /api/moveto?pulses=`, `POST /api/zero`.

---

## Project structure

```
main/
  main.c            app entry: probe UART, init motor, start web
  motor.{c,h}       ramped moves, open/close/zero, current(mA)->IRUN+vsense
  web.{c,h}         WiFi STA + HTTP server + the served single-page UI
  wifi_secrets.h    your WLAN creds (GITIGNORED)
  wifi_secrets.example.h   template
components/tmc2208/ from-scratch single-wire UART driver (CRC8, register R/W)
CMakeLists.txt, sdkconfig.defaults
CLAUDE.md                       hardware / build / wiring reference
TMC2208_ESP32_UART_explainer.MD how the two comms channels work
planning_TORQUE.MD              torque budget + design intent
shopsmartfan.ino                original Arduino sketch (reference only, not built)
```

---

## Lessons Learned (a.k.a. things that cost me hours)

These are real debugging war stories from bring-up — documented so they don't bite
again.

- **The TMC2208's UART needs VM (12 V motor supply) connected — not just VIO.**
  This was the big one (~hours lost + an unnecessary chip swap). With only VIO
  (3.3 V logic) powered, the ESP32 could drive the bus and saw its own TX **echo
  cleanly**, which *looked* like working comms — but the chip never replied
  (`reply read got 0/8`). The chip's digital core, including the UART state
  machine, runs off an internal regulator fed by **VM**. VIO alone only powers the
  I/O buffers. **If UART is silent but echoes are clean, check VM first.**

- **Don't bypass the 1 kΩ TX resistor.** A stray wire ran from GPIO 26 straight to
  the `PDN` pin *in addition to* the intended `GPIO 26 → 1 kΩ → UART` path, shorting
  out the resistor. When the chip tried to reply, it fought the ESP32's push-pull
  output and lost. Sanity check with a meter: `GPIO 26 → chip pin 4` should read
  **~1 kΩ**, not ~0.

- **The driver ships in standalone mode.** UART does nothing until you bridge the
  back-side solder jumper (middle pad → `UART` pad). Verify continuity to the
  `UART` header pin, not `PDN`.

- **`vsense=1` caps current at ~1 A.** With the 0.11 Ω sense resistor, high-
  sensitivity mode tops out near 0.98 A. To reach the motor's rated 1.5 A you need
  `vsense=0` (the firmware now auto-selects this from the requested mA).

- **GPIO 12 is a strapping pin (flash-voltage select).** `MS1` had been wired to
  GPIO 12; an external pull-up there can stop the ESP32 from booting. It's snipped
  now — don't tie MS1 to GPIO 12 with a pull-up.

- **Hand-gripping a free shaft to "feel" torque stalls it and corrupts position.**
  Open-loop step counting has no feedback: if you stall the motor by hand, the
  firmware still believes it moved. (This produced a false "it only ran once"
  report.) To measure holding torque, use a known moment arm + force gauge against
  the **energized, stationary** motor — don't fight it mid-move.

- **Slow full-step can hit resonance.** Dropping speed for "max torque" put a
  full-step move (~1.25 rev/s) near the NEMA 17 mid-band resonance, where it can
  vibrate without rotating. If it stalls *unloaded*, switch to microstepping (1/8
  or 1/16) to smooth it.

- **Two unrelated "A/B" naming systems.** The motor's coils are `A`/`Ā`/`B`/`B̄`;
  the driver's H-bridges are `M1A`/`M1B`/`M2A`/`M2B`. The trailing A/B on the driver
  is just a position label — *not* the motor's coil A/B. Rule: both ends of one
  motor coil go to the **same** H-bridge pair; never split a coil across M1/M2.

---

## Status & roadmap

**Working:** UART driver, ramped/position-aware motor moves, max-torque profile
(bench-validated), WiFi STA + web GUI (dependency-aware parameter form + action
builder with live preview).

**TODO:**
- Raw-register advanced panel (every TMC2208 register over UART, friendly↔raw linked).
- Non-blocking moves on a task + a Stop button (page currently waits during a move).
- Homing routine so `open()`/`close()` work against a known closed reference.
- Pick the real-world trigger for the vent (button / sensor / schedule / web).
