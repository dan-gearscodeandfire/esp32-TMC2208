#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "tmc2208.h"

// Which driver board is wired up. The STEP/DIR/EN motion engine is identical
// for all of them; the difference is *configuration*:
//   - TMC2208/2209 take current, microsteps and chopper mode over UART.
//   - A4988/DRV8825 have no UART — current is set by the board trimpot and
//     microstepping by the MS jumpers. For those, the firmware can only DRIVE
//     (step/dir/enable + speed/accel); it can't change current/microsteps. The
//     `microsteps` field becomes a *declaration* of the jumper setting so the
//     step-count and ramp math stay correct.
// TMC2208 vs TMC2209 use the same registers for everything we configure, so
// they share the UART config path; they're auto-detected from VERSION at boot.
typedef enum {
    MOTOR_DRIVER_TMC2208 = 0,  // UART: current, microsteps, chopper all software-set
    MOTOR_DRIVER_TMC2209,      // UART: same config registers as the 2208 for our purposes
    MOTOR_DRIVER_A4988,        // STEP/DIR/EN only; 1..16 microsteps via MS jumpers
    MOTOR_DRIVER_DRV8825,      // STEP/DIR/EN only; like A4988 but also 1/32
} motor_driver_t;

bool            motor_driver_has_uart(motor_driver_t d);   // true for the TMC family
const char     *motor_driver_token(motor_driver_t d);      // "tmc2208", "a4988", ...
motor_driver_t  motor_driver_from_token(const char *tok);  // inverse; unknown -> TMC2208
motor_driver_t  motor_active_driver(void);                 // currently configured driver

// Chopper mode. SpreadCycle = more torque under load (use for the lid);
// StealthChop = quieter but softer. (TMC family only.)
typedef enum {
    MOTOR_CHOP_STEALTH = 0,
    MOTOR_CHOP_SPREAD  = 1,
} motor_chop_t;

// All move parameters live here and are overridable at runtime via
// motor_configure(). The firmware imposes no artificial ceiling: run_current_ma
// is translated to the TMC2208's IRUN/vsense fields and only clamps at the
// hardware limit (~1770 mA, set by the 0.11 Ohm sense resistor).
typedef struct {
    motor_driver_t driver;        // which driver board is wired up
    uint16_t     run_current_ma;  // RMS run current target (TMC only; trimpot otherwise)
    uint8_t      microsteps;      // 1, 2, 4, 8, 16 (TMC/A4988) or 32 (DRV8825)
    motor_chop_t chop;            // chopper mode (TMC only)
    uint16_t     start_sps;       // start/stop pulse rate (pull-in) — keep low
    uint16_t     cruise_sps;      // cruise pulse rate
    uint32_t     accel_sps2;      // acceleration in steps/s^2; 0 => firmware default,
                                  //   huge value (e.g. 0xFFFF) => effectively no ramp
    uint8_t      sg_threshold;    // TMC2209 StallGuard threshold (0..255). 0 = disabled.
                                  // Stall declared when SG_RESULT <= sg_threshold*2.
                                  // Higher = more sensitive. Ignored on non-2209 drivers.
} motor_config_t;

// Max-torque defaults for the vent lid — see planning_TORQUE.MD.
// (vsense=0 / ~1.5 A is selected automatically from run_current_ma.)
#define MOTOR_CONFIG_DEFAULT {           \
    .driver         = MOTOR_DRIVER_TMC2208, \
    .run_current_ma = 1500,              \
    .microsteps     = 1,                 \
    .chop           = MOTOR_CHOP_SPREAD, \
    .start_sps      = 200,               \
    .cruise_sps     = 1200,              \
    .accel_sps2     = 0,                 \
    .sg_threshold   = 0,                 \
}

#define MOTOR_TRAVEL_REVS   2.75f   // vent fully closed -> fully open
#define MOTOR_FULLSTEPS_REV 200     // 1.8 deg/step NEMA 17

esp_err_t motor_init(tmc2208_t *drv, const motor_config_t *cfg);

// Re-push the config to the chip at any time (override current/speed/usteps/chop).
// NOTE: changing microsteps rescales the pulse-position counter — call
// motor_zero() afterward to re-establish the reference.
void motor_configure(const motor_config_t *cfg);

void motor_enable(void);   // EN low  (energize power stage)
void motor_disable(void);  // EN high (de-energize; lid self-holds when parked)

// Relative move, signed, with trapezoidal ramp. De-energizes when the move
// completes (lid self-holds, so no holding current needed).
void motor_move_pulses(int32_t pulses);          // pulses = microsteps at current res
void motor_move_steps(int32_t full_steps);       // scaled by microsteps
void motor_move_revs(float revs);
void motor_move_to_pulses(int32_t target_pulses); // absolute move to a position

// Position-aware open/close. Require motor_zero() first to set the closed
// reference (no endstops on this hardware — manual/homing zero).
void    motor_zero(void);   // declare current shaft position as 0 (closed)
void    motor_open(void);   // move to the +2.75-rev travel limit
void    motor_close(void);  // move back to 0
int32_t motor_position_pulses(void);

// TMC2209 StallGuard load reading. Returns ESP_ERR_NOT_SUPPORTED on any other
// driver, or when sg_threshold is 0. *load is 0..1023 (lower = higher torque);
// *stalled is true when load <= sg_threshold*2. SG_RESULT is only meaningful
// while the motor is stepping above some minimum velocity — readings at rest
// or during the very first part of a ramp are noise.
esp_err_t motor_read_sg(uint16_t *load, bool *stalled);
