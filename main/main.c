#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "tmc2208.h"
#include "motor.h"
#include "web.h"

#define LED_PIN  GPIO_NUM_2
#define UART_TX  GPIO_NUM_26
#define UART_RX  GPIO_NUM_27

static const char *TAG = "app";
static tmc2208_t s_driver;
static motor_config_t s_motor_cfg = MOTOR_CONFIG_DEFAULT;

// Probe the UART bus for a TMC. On success returns true and writes the chip's
// VERSION byte to *ver_out (0x20 = TMC2208, 0x21 = TMC2209) so the caller can
// pick the right driver profile. A failed probe is normal for legacy boards
// (A4988/DRV8825) that have no UART at all.
static bool probe_uart(uint8_t *ver_out)
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
        // 0x20 = TMC2208, 0x21 = TMC2209 — both report VERSION in IOIN[31:24].
        if (tmc2208_get_version(&s_driver, &ver) == ESP_OK && (ver == 0x20 || ver == 0x21)) {
            ESP_LOGI(TAG, "TMC UART OK (slave 0x%02x, VERSION 0x%02x = %s)",
                     a, ver, ver == 0x21 ? "TMC2209" : "TMC2208");
            if (ver_out) *ver_out = ver;
            return true;
        }
    }
    return false;
}

void app_main(void)
{
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    // Detect a TMC on the UART bus. If present, pick TMC2208 vs TMC2209 from
    // its VERSION and hand the driver handle to the motor module. If absent,
    // that's fine for a legacy board (A4988/DRV8825) -- boot in STEP/DIR mode
    // with no UART handle; the user picks the actual driver in the web UI.
    uint8_t ver = 0;
    tmc2208_t *drv = NULL;
    if (probe_uart(&ver)) {
        drv = &s_driver;
        s_motor_cfg.driver = (ver == 0x21) ? MOTOR_DRIVER_TMC2209 : MOTOR_DRIVER_TMC2208;
    } else {
        s_motor_cfg.driver = MOTOR_DRIVER_A4988;   // safe STEP/DIR default for a no-UART bench
        ESP_LOGW(TAG, "No TMC on UART (VM/12 V connected?). Starting in legacy "
                      "STEP/DIR mode -- choose your driver in the web UI.");
    }

    motor_init(drv, &s_motor_cfg);
    if (web_start() == ESP_OK) {
        ESP_LOGI(TAG, "Web control up -- open the printed IP in a browser.");
    } else {
        ESP_LOGW(TAG, "WiFi/web did not start; motor configured but not web-reachable.");
    }

    int n = 0;
    while (1) {
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        if ((++n % 10) == 0) ESP_LOGI(TAG, "alive (%d s)", n);
    }
}
