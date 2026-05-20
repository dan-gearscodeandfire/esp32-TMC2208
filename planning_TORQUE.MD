# Planning — Motor Torque Budget

> **Status: educated estimate, NOT a measured spec.** The motor's holding torque
> was never stated on a datasheet we have. Values below are inferred from frame
> size, body length, and rated current. Replace with real numbers if the actual
> motor model is identified.

## Motor under consideration

- NEMA 17 frame (42 mm faceplate)
- **39 mm body depth**
- 12 V, **1.5 A** rated, 200 steps/rev (1.8°/step), bipolar
- **Phase resistance: 2.3 Ω** (listed spec)

### What the 2.3 Ω phase resistance tells us

- **Rated (DC) voltage ≈ I × R = 1.5 A × 2.3 Ω ≈ 3.45 V.** That's the voltage
  needed to push rated current through one winding statically. It's far below our
  12 V supply — which is exactly why the TMC2208 is a *current-controlled chopper*:
  it PWM-switches the 12 V rail to regulate winding current to the IRUN target,
  rather than applying voltage directly. The headroom (12 V vs 3.45 V) is what
  lets current rise quickly through the winding inductance at higher step rates.
- **Per-phase resistive dissipation at rated current ≈ I²R = 1.5² × 2.3 ≈ 5.2 W.**
  At rated current the motor itself runs warm — another reason to stay below rated
  unless the lid actually needs the torque. (This is motor heating, separate from
  the driver heating noted below.)
- 2.3 Ω with a 1.5 A rating is consistent with the ~40 N·cm class estimated above —
  it doesn't change the torque guess (torque tracks current and magnetics, not
  winding resistance), but it confirms the motor is the expected low-voltage,
  current-driven NEMA 17 type.

## How the estimate was derived

Holding torque on a NEMA 17 scales mainly with **rotor/stator stack length**
(≈ body depth) and **rated current** — same faceplate, longer body = more
magnetic material = more torque, roughly proportionally.

| Body depth | Typical holding torque (at rated A) |
|------------|-------------------------------------|
| ~20 mm     | ~13 N·cm |
| ~34 mm     | ~28–40 N·cm |
| **~38–40 mm** | **~38–45 N·cm** |
| ~48 mm     | ~45–59 N·cm |

39 mm @ 1.5 A matches a very common reference motor, the **17HS15-1504S**
(38 mm body, 1.5 A, 1.8°), spec'd at **~40 N·cm**. Our numbers track it closely.

## Estimate

**≈ 40 N·cm holding torque at full 1.5 A** (range ~36–44 N·cm).

Equivalent units:
- **0.40 N·m** (≈ 0.36–0.44 N·m)
- **≈ 57 oz·in** (≈ 51–62 oz·in)

## Torque vs. drive current (the part that matters for the lid)

Holding torque is spec'd at **rated current**. Torque is roughly proportional to
current until magnetic saturation, and the firmware currently drives well below
rated:

| Setting             | Current (RMS) | Approx. torque |
|---------------------|---------------|----------------|
| Sanity test (IRUN=8)  | ~275 mA     | ~7 N·cm  |
| Demo (IRUN=16)        | ~520 mA     | ~14 N·cm |
| **Rated (IRUN=31)**   | **~1.5 A**  | **~40 N·cm** |

→ At present settings we're using roughly **a third** of the motor's torque.

## Implications for the RV vent lid actuator

- The lid cranks against a hinge, likely a weather seal, and wind load on the
  roof — so plan to **run closer to rated current during the actual move**.
- **Thermal constraint** (`CLAUDE.md`): keep under ~1 A without active cooling;
  the driver heats up. Sensible target: **~800 mA–1 A while moving** (≈ 21–27 N·cm),
  dropping to a low `IHOLD` — or fully de-energized — once parked.
- **Running torque < holding torque:** back-EMF reduces available torque as step
  rate rises. Keep travel speed modest for maximum grunt.
- **Gear reduction in the vent mechanism** (worm/crank) multiplies torque — the
  motor may not need to be near rated at all. Depends on the mechanism; revisit
  once the lid's force requirement is characterized.

## Open questions to resolve before finalizing current settings

1. Actual force/torque needed to move the lid (measure or estimate from the
   mechanism's gear ratio and seal friction).
2. Whether the parked lid self-holds mechanically (worm drive) or needs active
   `IHOLD` current to stay put against wind.
3. Real motor model, if it can be read off the label — to replace this estimate.

## Design intent — firmware as a transparent conduit

The firmware should expose the **TMC2208's full parameter space** to the ESP32:
a thin, transparent pass-through with **no artificial software ceiling**. Any
current, any microstep mode, any chopper mode, any register — whatever the ESP32
writes, the chip gets. The driver does not clamp or validate values.

The only real ceilings are **hardware and physics, not firmware**:
- **Sense resistor (0.11 Ω)** → absolute max ~1.77 A RMS (`vsense=0`, CS=31). To
  exceed this you change the resistor, not the code.
- **Thermal/physics** → the "~1 A without cooling" note is heat *advice*, not a limit
  (and the lid's tiny duty cycle makes it moot).

The "1 A" seen earlier was **not a limit** — it was the demo's `vsense=1` setting,
which caps the current-scale range at ~0.98 A. Setting `vsense=0` opens it to ~1.77 A
with the same code. Bottom line: keep the firmware an unrestricted interface to the
chip; let hardware be the only boundary.
