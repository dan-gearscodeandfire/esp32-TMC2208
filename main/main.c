#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "tmc2208.h"
#include "motor.h"

#define LED_PIN  GPIO_NUM_2
#define UART_TX  GPIO_NUM_26
#define UART_RX  GPIO_NUM_27

static const char *TAG = "app";
static tmc2208_t s_driver;

// === Exposed move parameters — edit to override (definitions in motor.h) ===
static motor_config_t s_motor_cfg = MOTOR_CONFIG_DEFAULT;

// On-boot demonstration move. Set false to boot straight to idle.
static const bool s_run_demo = true;

// Torque demo: number of CW/CCW 2.75-rev round-trips.
#define DEMO_CYCLES 4

static bool probe_uart(void)
{
    tmc2208_config_t cfg = {
        .uart_port  = UART_NUM_2,
        .tx_pin     = UART_TX,
        .rx_pin     = UART_RX,
        .baud       = 19200,
        .slave_addr = 0x00,
    };
    if (tmc2208_init(&s_driver, &cfg) != ESP_OK) return false;

    uint8_t ver = 0;
    for (uint8_t a = 0; a <= 3; a++) {
        s_driver.cfg.slave_addr = a;
        if (tmc2208_get_version(&s_driver, &ver) == ESP_OK && ver == 0x20) {
            ESP_LOGI(TAG, "TMC2208 UART OK (slave 0x%02x, VERSION 0x%02x)", a, ver);
            return true;
        }
    }
    return false;
}

void app_main(void)
{
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    if (!probe_uart()) {
        ESP_LOGE(TAG, "TMC2208 not responding -- is VM (12 V) connected? Motor logic halted.");
    } else {
        // Max torque within a two-tier current envelope:
        //   peak (during the brief move): ~1.49 A (IRUN=26, under the 1.5 A max)
        //   continuous (time-averaged):  ~0.5 A — far under 1.2 A, because the
        //     motor is DE-ENERGIZED during the 5 s pauses (duty ~1/3).
        // Max torque also wants LOW speed + a GENTLE ramp: at a slow step rate
        // the winding current fully settles each step (step period > L/R), so
        // the motor develops near its full holding torque (torque-speed curve).
        s_motor_cfg.run_current_ma = 1500;   // -> IRUN=26, ~1.49 A peak (< 1.5 A max)
        s_motor_cfg.start_sps      = 100;    // gentle pull-in from standstill
        s_motor_cfg.cruise_sps     = 250;    // slow -> near holding torque
        motor_init(&s_driver, &s_motor_cfg);

        if (s_run_demo) {
            // 2.75-rev CW, pause 5 s, 2.75-rev CCW, pause 5 s, repeated.
            // On the bench the shaft just spins; if mounted, start near a
            // mid/closed position so full-travel moves don't ram a hard stop.
            // "CW" here = DIR high (+); flip the signs if your wiring is opposite.
            ESP_LOGI(TAG, "Max-torque cycle: 2.75 rev CW, 5 s, 2.75 rev CCW, 5 s, x%d", DEMO_CYCLES);
            for (int c = 1; c <= DEMO_CYCLES; c++) {
                ESP_LOGI(TAG, "cycle %d/%d: 2.75 rev CW", c, DEMO_CYCLES);
                motor_move_revs(+2.75f);
                vTaskDelay(pdMS_TO_TICKS(5000));
                ESP_LOGI(TAG, "cycle %d/%d: 2.75 rev CCW", c, DEMO_CYCLES);
                motor_move_revs(-2.75f);
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            ESP_LOGI(TAG, "Cycle complete; motor de-energized.");
        }
    }

    int n = 0;
    while (1) {
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "idle heartbeat #%d", ++n);
    }
}
