#include "motor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

static const char *TAG = "motor";

// Pins — match the Hardware section of CLAUDE.md.
#define EN_PIN    GPIO_NUM_13
#define DIR_PIN   GPIO_NUM_32
#define STEP_PIN  GPIO_NUM_33

#define RSENSE_OHM    0.11f
#define RSENSE_EXTRA  0.02f   // internal parasitic resistance, per datasheet
#define SQRT2         1.41421356f

static tmc2208_t     *s_drv;
static motor_config_t s_cfg;
static int32_t        s_pos;   // position in pulses from zero

// Translate a target RMS current (mA) to the TMC2208 IRUN code (0-31) and pick
// the vsense bit. Tries vsense=1 (fine resolution, ~1 A ceiling) first; falls
// back to vsense=0 (~1.77 A ceiling) when needed. Clamps only at the hardware
// limit set by the sense resistor — no artificial firmware cap.
static uint8_t current_to_irun(uint16_t ma, uint8_t *vsense)
{
    const float r = RSENSE_OHM + RSENSE_EXTRA;
    const float I = ma / 1000.0f;

    *vsense = 1;
    float cs = 32.0f * I * SQRT2 * r / 0.180f - 1.0f;   // Vfs = 0.180 V
    if (cs > 31.0f) {
        *vsense = 0;
        cs = 32.0f * I * SQRT2 * r / 0.325f - 1.0f;     // Vfs = 0.325 V
    }
    if (cs < 0.0f) cs = 0.0f;
    if (cs > 31.0f) {
        ESP_LOGW(TAG, "%u mA exceeds ~1770 mA hardware ceiling (Rsense=%.2f); clamping", ma, RSENSE_OHM);
        cs = 31.0f;
    }
    return (uint8_t)(cs + 0.5f);
}

static uint8_t microsteps_to_mres(uint8_t us)
{
    switch (us) {
        case 1:  return 8;   // full step
        case 2:  return 7;
        case 4:  return 6;
        case 8:  return 5;
        case 16: return 4;
        default:
            ESP_LOGW(TAG, "unsupported microsteps=%u; using full step", us);
            return 8;
    }
}

void motor_configure(const motor_config_t *cfg)
{
    s_cfg = *cfg;

    uint8_t vsense;
    uint8_t irun = current_to_irun(s_cfg.run_current_ma, &vsense);
    uint8_t mres = microsteps_to_mres(s_cfg.microsteps);

    // GCONF: I_scale_analog=0 (internal ref), pdn_disable=1, mstep_reg_select=1,
    //        en_spreadCycle per config.
    uint32_t gconf = (1u << 6) | (1u << 7);
    if (s_cfg.chop == MOTOR_CHOP_SPREAD) gconf |= (1u << 2);
    tmc2208_write_register(s_drv, TMC2208_REG_GCONF, gconf);

    // IHOLD_IRUN: IRUN as computed; IHOLD low (moot — we de-energize when parked).
    uint32_t ihold_irun = (2u << 16) | ((uint32_t)irun << 8) | 2u;
    tmc2208_write_register(s_drv, TMC2208_REG_IHOLD_IRUN, ihold_irun);

    // CHOPCONF: TOFF=5, HSTRT=4, TBL=2, vsense, MRES, intpol=1.
    uint32_t chopconf = 0x5u
                      | (4u << 4)
                      | (2u << 15)
                      | ((uint32_t)(vsense & 1u) << 17)
                      | ((uint32_t)mres << 24)
                      | (1u << 28);
    tmc2208_write_register(s_drv, TMC2208_REG_CHOPCONF, chopconf);

    ESP_LOGI(TAG, "config: %u mA (IRUN=%u vsense=%u), %u ustep, %s, %u->%u sps",
             s_cfg.run_current_ma, irun, vsense, s_cfg.microsteps,
             s_cfg.chop == MOTOR_CHOP_SPREAD ? "SpreadCycle" : "StealthChop",
             s_cfg.start_sps, s_cfg.cruise_sps);
}

esp_err_t motor_init(tmc2208_t *drv, const motor_config_t *cfg)
{
    s_drv = drv;
    s_pos = 0;

    gpio_reset_pin(EN_PIN);
    gpio_set_direction(EN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(EN_PIN, 1);   // disabled until a move

    gpio_reset_pin(DIR_PIN);
    gpio_set_direction(DIR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DIR_PIN, 0);

    gpio_reset_pin(STEP_PIN);
    gpio_set_direction(STEP_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(STEP_PIN, 0);

    motor_configure(cfg);
    return ESP_OK;
}

void motor_enable(void)  { gpio_set_level(EN_PIN, 0); }
void motor_disable(void) { gpio_set_level(EN_PIN, 1); }

// half-period (microseconds) for a given pulse rate
static inline uint32_t sps_to_half_us(uint32_t sps)
{
    if (sps == 0) sps = 1;
    return 500000u / sps;   // 1e6 / (2 * sps)
}

void motor_move_pulses(int32_t pulses)
{
    if (pulses == 0) return;
    const int dir = (pulses > 0) ? 1 : 0;
    const uint32_t n = (pulses > 0) ? (uint32_t)pulses : (uint32_t)(-pulses);

    uint32_t ramp = s_cfg.accel_pulses;
    if (ramp == 0) ramp = (uint32_t)s_cfg.microsteps * MOTOR_FULLSTEPS_REV / 2;  // half rev
    if (ramp > n / 2) ramp = n / 2;

    uint32_t slow = sps_to_half_us(s_cfg.start_sps);
    uint32_t fast = sps_to_half_us(s_cfg.cruise_sps);
    if (slow < fast) slow = fast;   // guard against start_sps > cruise_sps

    gpio_set_level(DIR_PIN, dir);
    motor_enable();
    vTaskDelay(pdMS_TO_TICKS(10));  // let the power stage settle before stepping

    for (uint32_t i = 0; i < n; i++) {
        uint32_t hp;
        if (ramp > 0 && i < ramp) {
            hp = slow - (slow - fast) * i / ramp;            // accelerate
        } else if (ramp > 0 && i >= n - ramp) {
            hp = slow - (slow - fast) * (n - 1 - i) / ramp;  // decelerate
        } else {
            hp = fast;                                       // cruise
        }
        gpio_set_level(STEP_PIN, 1);
        esp_rom_delay_us(hp);
        gpio_set_level(STEP_PIN, 0);
        esp_rom_delay_us(hp);
        if ((i & 0x1FF) == 0x1FF) vTaskDelay(1);  // feed the task watchdog
    }

    s_pos += pulses;
    motor_disable();   // lid self-holds; de-energize to stay cool
    ESP_LOGI(TAG, "moved %ld pulses (dir=%d) -> position %ld", (long)pulses, dir, (long)s_pos);
}

void motor_move_steps(int32_t full_steps)
{
    motor_move_pulses(full_steps * (int32_t)s_cfg.microsteps);
}

void motor_move_revs(float revs)
{
    float steps = revs * MOTOR_FULLSTEPS_REV;
    motor_move_steps((int32_t)(steps + (revs >= 0 ? 0.5f : -0.5f)));
}

void motor_zero(void)
{
    s_pos = 0;
    ESP_LOGI(TAG, "position zeroed (closed reference)");
}

int32_t motor_position_pulses(void) { return s_pos; }

void motor_open(void)
{
    int32_t limit = (int32_t)(MOTOR_TRAVEL_REVS * MOTOR_FULLSTEPS_REV * s_cfg.microsteps + 0.5f);
    motor_move_pulses(limit - s_pos);
}

void motor_close(void)
{
    motor_move_pulses(-s_pos);
}
