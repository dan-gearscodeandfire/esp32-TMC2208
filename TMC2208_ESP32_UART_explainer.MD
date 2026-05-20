# How the ESP32 Communicates with the TMC2208

The key insight: **the ESP32 talks to the TMC2208 over two completely separate
channels, and they do different jobs.** Mixing them up is the #1 source of
confusion with these drivers.

---

## Channel 1: STEP / DIR / EN — the *motion* channel (real-time hardware signals)

This is how the motor actually **moves**. No protocol, no bytes — just logic
levels and pulses on dedicated pins:

| Pin    | GPIO | What it does |
|--------|------|--------------|
| `STEP` | 33   | **Each rising edge = one microstep.** Want to move 3200 microsteps? Send 3200 pulses. |
| `DIR`  | 32   | **Logic level = direction.** HIGH = one way, LOW = the other. Sampled at each STEP edge. |
| `EN`   | 13   | **Power stage on/off.** LOW = coils energized, HIGH = driver off (active-low). |

So "move the lid 2 revolutions to open" becomes: set `DIR` to the open
direction, then pulse `STEP` 6400 times (2 × 3200) at whatever rate sets the
speed. The *timing between pulses* is the speed; the *count* is the distance.
That's literally what `pulse_steps()` does in `main/main.c`:
`gpio_set_level(STEP_PIN, 1)`, wait, `gpio_set_level(STEP_PIN, 0)`, wait, repeat.

This channel is dead simple and works on *every* stepper driver (A4988, DRV8825,
TMC2208 in standalone mode, etc.). It's the universal language of stepper control.

---

## Channel 2: UART — the *configuration & telemetry* channel

This is the single-wire UART (GPIO 26 TX through 1 kΩ + GPIO 27 RX → the chip's
`PDN_UART` pin). It does **not** move the motor step-by-step. It's how the ESP32
*configures the chip's behavior* and *reads its status* — the things that, on a
dumber driver, you'd set with physical jumpers and a trimpot:

- **Motor current** (`IHOLD_IRUN` register) — instead of the Vref trimpot
- **Microstep resolution** (`CHOPCONF.MRES`) — instead of the MS1/MS2 pins
- **StealthChop vs SpreadCycle**, interpolation, blank time, etc.
- **Reading back status** — `DRV_STATUS` (overtemp, shorts, open-load), `IFCNT`,
  chip `VERSION`

### How a UART request is physically structured

When the ESP32 wants to write a register, it sends an **8-byte datagram** on the wire:

```
[0x05]  [slave addr]  [reg | 0x80]  [data3] [data2] [data1] [data0]  [CRC8]
 sync      0x00        write bit                32-bit value         checksum
```

- **Sync byte `0x05`** tells the chip "a frame is starting" (it also auto-detects
  the baud rate from this byte's edges).
- **Slave address** (0x00 here) — which chip on the bus, set by MS1/MS2 at power-up.
- **Register address** with bit 7 = 1 means *write*, bit 7 = 0 means *read*.
- **4 data bytes** = the 32-bit register value, big-endian.
- **CRC8** (polynomial 0x07) so the chip can reject corrupted frames.

A **read** is a 4-byte request (no data bytes); the chip replies with its own
8-byte datagram containing the value. That's `tmc2208_write_register()` /
`tmc2208_read_register()` in `components/tmc2208/tmc2208.c`.

> **Note on this hardware:** the chip's UART core is powered from an internal
> regulator fed by **VM (the 12 V motor supply)** — not VIO. With VIO alone the
> ESP32 sees its own TX echo cleanly but the chip never replies. VM must be
> present for any UART communication.

---

## How they work together — the typical sequence

```
1. EN = HIGH                    (driver disabled — Channel 1)
2. UART: write IHOLD_IRUN       (set current     — Channel 2)
   UART: write CHOPCONF         (set microsteps  — Channel 2)
   UART: write GCONF/PWMCONF    (chopper mode    — Channel 2)
3. EN = LOW                     (enable power    — Channel 1)
4. DIR = open direction         (Channel 1)
   pulse STEP x N               (move the lid    — Channel 1)
5. EN = HIGH                    (de-energize     — Channel 1)
6. UART: read DRV_STATUS        (check for faults — Channel 2)
```

**Mental model:** UART is the *setup and the dashboard*; STEP/DIR is the *gas
pedal and steering*. You configure the chip's "personality" over UART once, then
drive it with pulses.

There's one exception — the `VACTUAL` register lets you command continuous
velocity purely over UART, no STEP pulses — but for precise positioning like a
lid, you want STEP/DIR counting.

For this lid actuator: **UART sets the holding torque and smoothness; STEP-counting
is what gives you precise "open this far / closed" positioning.**
