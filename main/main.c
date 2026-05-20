#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

#include "tmc2208.h"

// Pin map — mirrors the Hardware section of CLAUDE.md.
#define LED_PIN   GPIO_NUM_2
#define EN_PIN    GPIO_NUM_13   // active LOW; HIGH = driver disabled
#define DIR_PIN   GPIO_NUM_32
#define STEP_PIN  GPIO_NUM_33
#define UART_TX   GPIO_NUM_26
#define UART_RX   GPIO_NUM_27

#define PROBE_RETRY_EVERY_HEARTBEATS 10
#define USTEP                        16
#define STEPS_PER_REV                (200 * USTEP)   // 3200

static const char *TAG = "fan";
static tmc2208_t s_driver;
static bool s_uart_initialized = false;
static bool s_phase2_passed = false;

static void init_gpios(void)
{
    gpio_reset_pin(EN_PIN);
    gpio_set_direction(EN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(EN_PIN, 1);   // disabled until configured

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    gpio_reset_pin(DIR_PIN);
    gpio_set_direction(DIR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DIR_PIN, 0);

    gpio_reset_pin(STEP_PIN);
    gpio_set_direction(STEP_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(STEP_PIN, 0);
}

static esp_err_t init_tmc2208_uart(void)
{
    tmc2208_config_t cfg = {
        .uart_port  = UART_NUM_2,
        .tx_pin     = UART_TX,
        .rx_pin     = UART_RX,
        .baud       = 19200,
        .slave_addr = 0x00,
    };
    esp_err_t err = tmc2208_init(&s_driver, &cfg);
    if (err == ESP_OK) s_uart_initialized = true;
    else ESP_LOGE(TAG, "tmc2208_init failed: %s", esp_err_to_name(err));
    return err;
}

static void try_probe_tmc2208(void)
{
    if (!s_uart_initialized || s_phase2_passed) return;
    for (uint8_t addr = 0; addr <= 3; addr++) {
        s_driver.cfg.slave_addr = addr;
        tmc2208_write_register(&s_driver, TMC2208_REG_GCONF, 0xC0);
    }
    for (uint8_t addr = 0; addr <= 3; addr++) {
        s_driver.cfg.slave_addr = addr;
        uint8_t ver = 0;
        if (tmc2208_get_version(&s_driver, &ver) == ESP_OK) {
            ESP_LOGI(TAG, "TMC2208 UART OK -- slave=0x%02x VERSION=0x%02x", addr, ver);
            s_phase2_passed = true;
            return;
        }
    }
    ESP_LOGW(TAG, "No response on any slave 0x00-0x03 -- check VM(12V)/VIO/jumper/GND");
}

static void log_drv_status(const char *when)
{
    uint32_t st = 0;
    if (tmc2208_read_register(&s_driver, TMC2208_REG_DRV_STATUS, &st) != ESP_OK) {
        ESP_LOGW(TAG, "DRV_STATUS %s: read failed (reads are marginal at 1k -- non-fatal)", when);
        return;
    }
    ESP_LOGI(TAG, "DRV_STATUS %s = 0x%08x [ot=%d otpw=%d s2g=%d%d ol=%d%d cs=%d]",
             when, (unsigned)st,
             (int)((st >> 1) & 1), (int)((st >> 0) & 1),
             (int)((st >> 2) & 1), (int)((st >> 3) & 1),
             (int)((st >> 6) & 1), (int)((st >> 7) & 1),
             (int)((st >> 16) & 0x1F));
}

// Trapezoidal move: linear accel over ramp_steps, cruise, linear decel.
// half-period in microseconds: slow_us (start/end) -> fast_us (cruise).
static void move_trapezoid(int total_steps, int dir_level,
                           int slow_us, int fast_us, int ramp_steps)
{
    gpio_set_level(DIR_PIN, dir_level);
    esp_rom_delay_us(50);
    for (int i = 0; i < total_steps; i++) {
        int hp;
        if (i < ramp_steps) {
            hp = slow_us - (slow_us - fast_us) * i / ramp_steps;          // accel
        } else if (i >= total_steps - ramp_steps) {
            int j = total_steps - 1 - i;
            hp = slow_us - (slow_us - fast_us) * j / ramp_steps;          // decel
        } else {
            hp = fast_us;                                                  // cruise
        }
        gpio_set_level(STEP_PIN, 1);
        esp_rom_delay_us(hp);
        gpio_set_level(STEP_PIN, 0);
        esp_rom_delay_us(hp);
        if ((i & 0x1FF) == 0x1FF) vTaskDelay(1);   // feed WDT during long moves
    }
}

static void motor_demo(void)
{
    ESP_LOGI(TAG, "=== MOTOR DEMO ===");

    // ~520 mA RMS (IRUN=16, vsense=1), 16 microsteps, StealthChop, internal ref.
    tmc2208_write_register(&s_driver, TMC2208_REG_GCONF,      0x000000C0);
    tmc2208_write_register(&s_driver, TMC2208_REG_IHOLD_IRUN, 0x00021008);  // IHOLD=8 IRUN=16 DELAY=2
    tmc2208_write_register(&s_driver, TMC2208_REG_CHOPCONF,   0x14030055);  // TOFF=5 MRES=16 vsense=1 intpol

    uint8_t ifcnt = 0;
    tmc2208_get_ifcnt(&s_driver, &ifcnt);
    ESP_LOGI(TAG, "Configured (IFCNT=%d). Enabling driver.", ifcnt);

    gpio_set_level(EN_PIN, 0);    // enable
    vTaskDelay(pdMS_TO_TICKS(20));
    log_drv_status("start");

    // Profile: top speed ~1.3 rev/s (fast_us=120 -> 240us/step -> 4.2 kHz),
    // start/stop at ~0.26 rev/s, ramp over half a revolution.
    int cycle = 0;
    while (1) {
        cycle++;

        gpio_set_level(LED_PIN, 1);
        ESP_LOGI(TAG, "cycle %d: 3 rev CW", cycle);
        move_trapezoid(3 * STEPS_PER_REV, 1, 600, 120, STEPS_PER_REV / 2);
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(400));

        gpio_set_level(LED_PIN, 1);
        ESP_LOGI(TAG, "cycle %d: 3 rev CCW", cycle);
        move_trapezoid(3 * STEPS_PER_REV, 0, 600, 120, STEPS_PER_REV / 2);
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(400));

        if (cycle % 5 == 0) log_drv_status("running");
    }
}

// Set true to run the continuous motion demo; false keeps the motor idle/disabled.
static const bool s_run_demo = false;

void app_main(void)
{
    init_gpios();
    ESP_LOGI(TAG, "GPIOs ready; EN held HIGH (TMC2208 disabled)");

    init_tmc2208_uart();
    try_probe_tmc2208();

    if (s_phase2_passed && s_run_demo) {
        motor_demo();   // never returns
    }

    // Idle: motor stays de-energized (EN HIGH). Heartbeat so we can see it's alive.
    gpio_set_level(EN_PIN, 1);
    ESP_LOGI(TAG, "Motor IDLE (EN HIGH, de-energized). Demo disabled.");
    int n = 0;
    while (1) {
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        n++;
        ESP_LOGI(TAG, "idle heartbeat #%d", n);
        if (!s_phase2_passed && (n % PROBE_RETRY_EVERY_HEARTBEATS == 0)) {
            try_probe_tmc2208();
        }
    }
}
