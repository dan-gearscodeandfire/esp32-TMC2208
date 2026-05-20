#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "tmc2208.h"

// Chopper mode. SpreadCycle = more torque under load (use for the lid);
// StealthChop = quieter but softer.
typedef enum {
    MOTOR_CHOP_STEALTH = 0,
    MOTOR_CHOP_SPREAD  = 1,
} motor_chop_t;

// All move parameters live here and are overridable at runtime via
// motor_configure(). The firmware imposes no artificial ceiling: run_current_ma
// is translated to the TMC2208's IRUN/vsense fields and only clamps at the
// hardware limit (~1770 mA, set by the 0.11 Ohm sense resistor).
typedef struct {
    uint16_t     run_current_ma;  // RMS run current target
    uint8_t      microsteps;      // 1, 2, 4, 8, or 16
    motor_chop_t chop;            // chopper mode
    uint16_t     start_sps;       // start/stop pulse rate (pull-in) — keep low
    uint16_t     cruise_sps;      // cruise pulse rate
    uint32_t     accel_sps2;      // acceleration in steps/s^2; 0 => firmware default,
                                  //   huge value (e.g. 0xFFFF) => effectively no ramp
} motor_config_t;

// Max-torque defaults for the vent lid — see planning_TORQUE.MD.
// (vsense=0 / ~1.5 A is selected automatically from run_current_ma.)
#define MOTOR_CONFIG_DEFAULT {           \
    .run_current_ma = 1500,              \
    .microsteps     = 1,                 \
    .chop           = MOTOR_CHOP_SPREAD, \
    .start_sps      = 200,               \
    .cruise_sps     = 1200,              \
    .accel_sps2     = 0,                 \
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
